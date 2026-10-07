#include "stdafx.h"
#include "MobVac.h"
#include "Memory.h"

// ---------------------------------------------------------------------------------------------
// Mob vacuum - see MobVac.h for what it does and why the client can do it at all.
//
// Facts read off the shipped binary (imagebase 0x400000, no ASLR - addresses are absolute):
//
//   * CMob::Update (0x006675A8) is only ever reached through the CLife interface vtable
//     (slot 10 = 0x00AF8270), i.e. once per frame for every mob in the pool - hooking its entry
//     covers the whole pool, including mobs that spawn later, with no pool walk of our own.
//
//   * The mob's sprite layer (CMob+0x4C0) is bound to the mob's vector controller, so moving the
//     controller moves the sprite: CMob::Init creates the layer and hands the controller's
//     IWzVector2D interface ([mob+0x118]) to IWzGr2DLayer::put_origin (call site 0x00662BD5 = layer
//     vtable +0x64). The engine reads a layer's position off its origin, so the layer's own x/y is
//     only an offset from the controller. (Shifting that offset is what made the first attempt's
//     mobs vanish - its basis is not the mob's world position.)
//
//   * The controller is repositioned through the client's own API rather than by writing fields:
//     CVecCtrl::SetActive (0x009B12A8, CVecCtrlMob override 0x009BBD3D) takes
//     (bActive, x, y, vx, vy, bFloat, foothold); it clamps to the map bounds, writes the position
//     doubles, binds the foothold and re-seeds the move path at the new point. A mob carries two such
//     controllers (both created by CMob::Init): [mob+0x118] is the one the sprite layer's origin is
//     bound to, the one CMob::Update advances and gates on, and the one whose acknowledged-point
//     cache the AI re-bases from; [mob+0x11C] is the one the AI re-bases and flushes. The first is
//     the one this module places (see TeleportMob).
//
//   * Foothold: the controller carries a CStaticFoothold* at +0x110, and CMob::Update skips the
//     whole move-path generation - and with it the C->S 0xBC report - while that pointer is null.
//     CVecCtrl::raw_Move (0x009B5E7F) clears exactly that pointer, which is why the first attempt's
//     raw_Move teleport left the mob logically at the target but with no ground under it. This
//     module resolves the foothold at the destination through CWvsPhysicalSpace2D::GetFootholdClosest
//     (0x00A45677) on the space object the client keeps at 0x00BEBFA0, and never passes null.
//
//   * The C->S report needs no packet of ours, but it does need controller #1's own "last
//     acknowledged point" cache to move with the mob. That controller embeds its CMovePath at +0x1AC
//     and keeps at +0x1F0 a 24-byte copy of the last movement element it reported (+0x1F2 x, +0x1F4 y,
//     +0x1F6 vx, +0x1F8 vy, +0x1FA bFloat, +0x1FC foothold id) - exactly the fields CMob::SetActive
//     (0x006637EC) reads and CMob::GenerateMovePath (0x0066B6FC) re-places the controller from. That
//     re-basing is deliberate: every new move path starts on a point the server has already
//     acknowledged. A teleport that skips the cache is therefore drawn for one frame, re-based away
//     by the next AI decision, and never reported. This module writes the cache together with the
//     position, and from then on the client's own MOVE_LIFE (0xBC, built at 0x0066BC67 and accepted
//     by the server because the reporting player is the mob's controller) carries the target.
//
//   * The local player's position is the POINT at CUserLocal+0x1170: CUser::GetPos returns it, and
//     the CUserLocal members that move the character write that very field.
//
//   * CMob+0x188 is the CMobTemplate*, and template+0x208 is the WZ-derived boss flag - the value
//     CMob::Update tests before it skips its per-mob HP tag loop. Bosses are left alone.
//
//   * The current field comes from the ZRef carrier at 0x00BEDED4 (payload at +4). A change of that
//     payload, of the CUserLocal singleton, or a death ends the session.
//
// The hotkey and the state machine run on a thread this module owns rather than on a hook of
// CUserLocal::Update: BossHP.cpp already detours that entry (0x0094A144), and a second DetourAttach
// on the same entry would have to chain through the first one's trampoline. Polling from our own
// thread keeps this module out of the client's update chain entirely - it only reads globals.
//
// The pull runs BEFORE the client's own per-mob update, so that every decision that update makes about
// this mob - attack range, skill choice, and the move path it reports to the server - is made with the
// mob already on the point. It is not repeated while the mob stays there, and never more often than
// MobVac::nIntervalMs per mob: each pull re-seeds the mob's move path, so a mob held on the point
// every frame cannot move, attack, or be hit. The point is the player's position at the moment the
// vacuum is switched on; it does not follow the player afterwards.
// ---------------------------------------------------------------------------------------------

static const DWORD ADDR_CMob_Update = 0x006675A8;             // first byte B8
static const DWORD ADDR_CUserLocal_OnSetDead = 0x0095AF4E;    // first byte 83
static const DWORD ADDR_CVecCtrlMob_SetActive = 0x009BBD3D;   // first bytes 55 8B EC
static const DWORD ADDR_GetFootholdClosest = 0x00A45677;      // first bytes 55 8B EC 51
static const DWORD ADDR_GetFoothold = 0x0050D811;             // first byte B8 (id -> foothold)
static const DWORD ADDR_CMob_SetChaseTarget = 0x0066B562;     // first bytes 56 8B F1
static const DWORD ADDR_PhysicalSpace = 0x00BEBFA0;           // CWvsPhysicalSpace2D* (global)
static const DWORD ADDR_UserLocal_Instance = 0x00BEBF98;      // TSingleton<CUserLocal>::ms_pInstance
static const DWORD ADDR_FieldCarrier = 0x00BEDED4;            // ZRef carrier; CField* at +4

// Offsets inside CMob (relative to the CMob object itself, not to its CLife subobject).
static const int OFF_CMob_Template = 0x188;
static const int OFF_CMob_Pos = 0x510;        // {x,y} - what CLife::GetPos returns
static const int OFF_CMob_PosPrev = 0x518;    // {x,y} - refreshed from the above every Update
static const int OFF_CMob_VecCtrlIf = 0x118;  // controller #1: the sprite layer's origin, the +0x110
                                              // foothold gate and the +0x1F0 acknowledged-point cache
static const int OFF_CMob_VecCtrl = 0x11C;    // controller #2: the one the AI re-bases and flushes

// Offset inside CMobTemplate: set for the mobs that use the big gage (bosses).
static const int OFF_CMobTemplate_Boss = 0x208;

// Offsets inside the controller object (base = interface pointer - 0x0C: the IWzVector2D interface
// subobject sits at object+0x0C, which is what CMob stores).
static const int OFF_VecObject_Interface = 0x0C;
static const int VTBL_INDEX_VEC_SET_ACTIVE = 1;   // CVecCtrlMob::SetActive
static const int OFF_VecCtrl_Foothold = 0x110;    // CStaticFoothold* (null => no move path, no report)
static const int OFF_VecCtrl_Active = 0x18;       // int: the controller's own active flag (left alone)

// The controller's "last acknowledged point" cache (CMovePath+0x44, see the header comment).
static const int OFF_VecCtrl_AbsPos_X = 0x1F2;        // short
static const int OFF_VecCtrl_AbsPos_Y = 0x1F4;        // short
static const int OFF_VecCtrl_AbsPos_VX = 0x1F6;       // short
static const int OFF_VecCtrl_AbsPos_VY = 0x1F8;       // short
static const int OFF_VecCtrl_AbsPos_BFLOAT = 0x1FA;   // byte
static const int OFF_VecCtrl_AbsPos_FOOTHOLD = 0x1FC; // short, id (fed to CWvsPhysicalSpace2D::GetFoothold)

// Offset of the local player's position inside CUserLocal (CUser::GetPos returns &(this+0x1170)).
static const int OFF_CUserLocal_Pos = 0x1170;

// Offset of the local player's own controller, as its IWzVector2D interface pointer - the same slot
// shape a mob has (interface at object+0x0C, so the object is the pointer minus 0x0C). It keeps the
// same AbsPos cache, so the foothold the player stands on can be read straight off it.
static const int OFF_CUserLocal_VecCtrl = 0x11A4;

// Ctrl+0 toggles; the game itself does not bind that combination. Both the top-row and the numpad
// zero are accepted, because which one "0" means is a keyboard-layout detail.
static const int VK_TOGGLE_KEY = '0';
static const int VK_TOGGLE_KEY_PAD = VK_NUMPAD0;

// How often the vacuum thread looks at the keyboard, in milliseconds.
static const DWORD POLL_INTERVAL_MS = 15;

// How far (per axis, pixels) a mob may stray from the point before it is put back on it, and how long
// a pulled mob is left alone before it may be pulled again - both read from config.ini
// (mobVacRange / mobVacIntervalMs) so the tuning needs no rebuild. Defaults: 80px, 80ms.
//
// The interval is the knob that decides how the vacuum looks: every pull re-seeds the controller's
// move path, so a small interval pins the mobs hard (they barely move) while a large one leaves them
// to walk and act between pulls. It is also what keeps them usable at all - a mob re-seeded every
// frame never finishes a movement, never builds the action rectangle CMob::GetHitPoint reads (so it
// cannot be hit) and never reaches its attack step.
static const int MOB_VACUUM_RANGE_DEFAULT = 80;
static const DWORD MOB_VACUUM_INTERVAL_DEFAULT_MS = 80;

// Bounds the config values are clamped into, so a typo cannot pin the mobs every frame or switch the
// vacuum off by accident.
static const int MOB_VACUUM_RANGE_MIN = 10;
static const int MOB_VACUUM_RANGE_MAX = 1000;
static const DWORD MOB_VACUUM_INTERVAL_MIN_MS = 16;
static const DWORD MOB_VACUUM_INTERVAL_MAX_MS = 5000;

// A pin that has not been ticked for this long counts as gone (see MobPin): a mob that leaves the
// field has its CMob freed by the pool and a newly spawned mob recycles the very same heap block, so
// an entry that matches a pointer again after this long belongs to a different mob.
static const DWORD MOB_PIN_TTL_MS = 1000;

typedef void(__thiscall* tCMobUpdate)(void* pThis);
typedef void(__thiscall* tCUserLocalOnSetDead)(void* pThis, int bDead);
typedef void(__thiscall* tVecSetActive)(void* pVec, int bActive, int x, int y, int vx, int vy, int bFloat, void* pFoothold);
typedef void*(__thiscall* tGetFootholdClosest)(void* pSpace, int x, int y);
typedef void*(__thiscall* tGetFoothold)(void* pSpace, int footholdId);
typedef void(__thiscall* tCMobSetChaseTarget)(void* pMob, int bEnable, void* pTargetOwner, int nFlags);

namespace MobVac
{
	int nRange = MOB_VACUUM_RANGE_DEFAULT;           // dllmain sets both from config.ini
	int nIntervalMs = static_cast<int>(MOB_VACUUM_INTERVAL_DEFAULT_MS);
}

static tCMobUpdate g_origCMobUpdate = nullptr;
static tCUserLocalOnSetDead g_origCUserLocalOnSetDead = nullptr;

static bool g_bEnabled = false;      // config.ini switch - the thread only starts when set
static bool g_bActive = false;       // vacuum currently on (toggled by Ctrl+0)
static bool g_bComboDown = false;    // Ctrl+0 edge detection
static void* g_pLastField = nullptr; // current CField*, to notice a map change
static void* g_pLastUser = nullptr;  // CUserLocal*, to notice a relogin/character change

// The point the mobs are held on: the local player's position at the moment the vacuum was switched
// on. It does not follow the player; the foothold under it is resolved once, with it.
static POINT g_ptVacuum = { 0, 0 };
static void* g_pVacuumFoothold = nullptr;
static int g_nVacuumFootholdId = 0;

// One entry per mob the vacuum has pulled, so the interval between two pulls of the same mob is kept
// per mob. The entry outlives the mob - the pool frees a mob that leaves the field and the next spawn
// can reuse that memory - so a matching entry that has not been ticked for MOB_PIN_TTL_MS is treated
// as a different mob and reset (see PinFor).
struct MobPin
{
	void* pMob;
	DWORD dwNextPull;    // GetTickCount floor: this mob may not be pulled before it
	DWORD dwLastSeen;    // last frame this mob was ticked - a pin stops being trusted once it goes stale
};

static const int MOB_PIN_SLOTS = 48;
static MobPin g_pins[MOB_PIN_SLOTS] = {};

static MobPin* PinFor(void* pMob)
{
	const DWORD dwNow = GetTickCount();
	MobPin* pFree = nullptr;
	MobPin* pEvict = nullptr;

	for (int i = 0; i < MOB_PIN_SLOTS; ++i)
	{
		if (g_pins[i].pMob == pMob)
		{
			if (dwNow - g_pins[i].dwLastSeen > MOB_PIN_TTL_MS)
			{
				// A different mob at a recycled address: start it clean rather than inherit the
				// pull cooldown and the readings of the CMob that used to live here.
				g_pins[i] = MobPin{};
				g_pins[i].pMob = pMob;
			}

			return &g_pins[i];
		}

		if (g_pins[i].pMob == nullptr)
		{
			if (pFree == nullptr)
			{
				pFree = &g_pins[i];
			}
		}
		else if (pEvict == nullptr || g_pins[i].dwNextPull < pEvict->dwNextPull)
		{
			pEvict = &g_pins[i];   // the pool holds more mobs than slots: recycle the least patient
		}
	}

	MobPin* pSlot = pFree != nullptr ? pFree : (pEvict != nullptr ? pEvict : &g_pins[0]);
	*pSlot = MobPin{};
	pSlot->pMob = pMob;
	return pSlot;
}

// get_field (0x00437A0C) in two instructions, without calling into the client.
static void* GetCurrentField()
{
	void* pCarrier = *reinterpret_cast<void**>(ADDR_FieldCarrier);
	return pCarrier != nullptr ? *reinterpret_cast<void**>(reinterpret_cast<char*>(pCarrier) + 4) : nullptr;
}

// The local player's position, read straight off the CUserLocal singleton. The sanity window is the
// guard against a wrong base: a misread would give a wild pair, while real map coordinates stay far
// inside it.
static bool GetPlayerPos(void* pUserLocal, POINT& ptOut)
{
	if (pUserLocal == nullptr)
	{
		return false;
	}

	const POINT pt = *reinterpret_cast<const POINT*>(reinterpret_cast<const char*>(pUserLocal) + OFF_CUserLocal_Pos);
	if (pt.x <= -3000000 || pt.x >= 3000000 || pt.y <= -3000000 || pt.y >= 3000000)
	{
		return false;
	}

	ptOut = pt;
	return true;
}

// Bosses use the big gage; the client marks them with this template flag (see the header comment).
static bool IsVacuumable(const void* pMob)
{
	const char* pTemplate = *reinterpret_cast<const char* const*>(
		reinterpret_cast<const char*>(pMob) + OFF_CMob_Template);

	if (pTemplate == nullptr)
	{
		return false;
	}

	return *reinterpret_cast<const int*>(pTemplate + OFF_CMobTemplate_Boss) == 0;
}

static bool IsDataPointer(const void* p)
{
	const uintptr_t v = reinterpret_cast<uintptr_t>(p);
	return v >= 0x00AF0000u && v < 0x00C00000u;   // where the client keeps its vtables
}

// The foothold the local player is standing on, as the id the client's own caches carry. Taking it
// from the player's controller keeps the id and the foothold object this module binds consistent
// (CWvsPhysicalSpace2D::GetFoothold turns it back into the object). 0 = the player has no foothold
// (in the air).
static int GetPlayerFootholdId(void* pUserLocal)
{
	if (pUserLocal == nullptr)
	{
		return 0;
	}

	void* pVecIf = *reinterpret_cast<void**>(reinterpret_cast<char*>(pUserLocal) + OFF_CUserLocal_VecCtrl);
	if (pVecIf == nullptr)
	{
		return 0;
	}

	const char* pVec = reinterpret_cast<const char*>(pVecIf) - OFF_VecObject_Interface;
	return *reinterpret_cast<const short*>(pVec + OFF_VecCtrl_AbsPos_FOOTHOLD);
}

// Place one of the mob's controllers on a point through the client's own repositioning call.
//
// The gate is strict on purpose: the call goes through vtable slot 1, and that slot has to be
// CVecCtrlMob::SetActive. A mismatch means the object behind the slot is not what this module thinks
// it is - and calling a foreign function with a foreign `this` corrupts that object and crashes the
// pool update later. In that case the mob is simply left where it is.
static bool PlaceController(void* pVecIf, POINT pt, void* pFoothold, int nActiveFlag)
{
	if (pVecIf == nullptr)
	{
		return false;
	}

	char* pVec = reinterpret_cast<char*>(pVecIf) - OFF_VecObject_Interface;

	void** ppVtbl = *reinterpret_cast<void***>(pVec);
	if (ppVtbl == nullptr || !IsDataPointer(ppVtbl))
	{
		return false;
	}

	void* pSetActive = ppVtbl[VTBL_INDEX_VEC_SET_ACTIVE];
	if (pSetActive != reinterpret_cast<void*>(ADDR_CVecCtrlMob_SetActive))
	{
		return false;
	}

	reinterpret_cast<tVecSetActive>(pSetActive)(pVec, nActiveFlag, pt.x, pt.y, 0, 0, 0, pFoothold);
	return true;
}

// Put the mob on a point.
//
// A mob carries TWO controllers, both CVecCtrlMob, both created by CMob::Init:
//   [mob+0x118] - the one the sprite layer's origin is bound to (CMob::Init's put_origin), the one
//                 CMob::Update advances and gates on (+0x110 foothold), and the one whose +0x1F0
//                 cache CMob::GenerateMovePath / CMob::SetActive re-base from;
//   [mob+0x11C] - the one the AI re-bases and flushes, and whose move path is mirrored into the
//                 first one's path on the way out as the 0xBC report.
// This module therefore places the first one (leaving its active flag as it is - only the position is
// ours to change) and moves that one's +0x1F0 cache onto the target: the next AI decision re-bases
// the second controller here, rebuilds its path from here, mirrors it back and reports it, which
// keeps sprite, foothold gate and the server's copy on the same point without a packet of ours.
static bool TeleportMob(void* pMob, POINT pt, void* pFoothold, int nFootholdId)
{
	char* pMobBytes = reinterpret_cast<char*>(pMob);
	void* pAckedIf = *reinterpret_cast<void**>(pMobBytes + OFF_CMob_VecCtrlIf);
	if (pAckedIf == nullptr)
	{
		return false;   // CMob::Init has not run for this instance yet
	}

	char* pAcked = reinterpret_cast<char*>(pAckedIf) - OFF_VecObject_Interface;

	if (pFoothold == nullptr)
	{
		// Last resort: keep the foothold it already had rather than dropping the mob into free fall.
		pFoothold = *reinterpret_cast<void**>(pAcked + OFF_VecCtrl_Foothold);
	}

	// Only locally active mobs are placed (see VacuumTick), so both controllers go back to
	// active as the client itself leaves them.
	if (!PlaceController(pAckedIf, pt, pFoothold, 1))
	{
		return false;
	}

	// Both controllers are placed, not only the rendered one: they are separate instances with
	// different roles - one is advanced by CMob::Update and feeds CMob+0x510 (which the hit rectangle
	// and the attack-range check are built from), the other is the one the AI re-bases and flushes -
	// so they have to agree on the point before the client's own update of this mob runs.
	void* pActiveIf = *reinterpret_cast<void**>(pMobBytes + OFF_CMob_VecCtrl);
	if (pActiveIf != nullptr && pActiveIf != pAckedIf)
	{
		PlaceController(pActiveIf, pt, pFoothold, 1);   // best effort: the first call is the gate
	}

	// Move the client's own "last acknowledged point" cache onto the target (see the header comment):
	// the AI re-bases every new move path - and with it every 0xBC report - on this cache, so a
	// teleport that leaves it behind is re-based away on the next AI decision and never reported.
	*reinterpret_cast<short*>(pAcked + OFF_VecCtrl_AbsPos_X) = static_cast<short>(pt.x);
	*reinterpret_cast<short*>(pAcked + OFF_VecCtrl_AbsPos_Y) = static_cast<short>(pt.y);
	*reinterpret_cast<short*>(pAcked + OFF_VecCtrl_AbsPos_VX) = 0;
	*reinterpret_cast<short*>(pAcked + OFF_VecCtrl_AbsPos_VY) = 0;
	*reinterpret_cast<char*>(pAcked + OFF_VecCtrl_AbsPos_BFLOAT) = 0;
	if (nFootholdId != 0)
	{
		*reinterpret_cast<short*>(pAcked + OFF_VecCtrl_AbsPos_FOOTHOLD) = static_cast<short>(nFootholdId);
	}

	// SetActive takes the mob's chase target away: CVecCtrlMob::SetActive zeroes +0x250 and tears down
	// the movement attribute, and the client only restores both inside CMob::GenerateMovePath - which
	// the AI has to decide to call first. Without this the mob stays frozen where it was put: no
	// chasing, no attacks, and no prepared action rectangle to be hit in. CMobPool::SetLocalMob pairs
	// the two calls the same way after it activates a mob, so this is the client's own recipe:
	// sub_66B562(mob, 1, player controller owner, 0)
	//   = CMob::IsActive gate + CVecCtrlMob::ChaseTargetImp([mob+0x11C]-12, ...) + CMob::SetShoeAttr.
	void* pUserLocal = *reinterpret_cast<void**>(ADDR_UserLocal_Instance);
	if (pUserLocal != nullptr)
	{
		reinterpret_cast<tCMobSetChaseTarget>(ADDR_CMob_SetChaseTarget)(pMob, 1,
			reinterpret_cast<char*>(pUserLocal) + 4, 0);
	}

	return true;
}

static int AbsDiff(int a, int b)
{
	return a > b ? a - b : b - a;
}

// Foothold under the vacuum point, resolved once when the vacuum is switched on. The player's own
// controller carries the id of the foothold it is standing on, so the object and the id written into
// the controller cache agree; a point without one falls back to the client's geometric lookup (and
// then the id stays 0 - the cache keeps whatever id it had).
static void ResolveVacuumFoothold(void* pUserLocal, POINT pt)
{
	g_pVacuumFoothold = nullptr;
	g_nVacuumFootholdId = 0;

	void* pSpace = *reinterpret_cast<void**>(ADDR_PhysicalSpace);
	if (pSpace == nullptr)
	{
		return;
	}

	const int nPlayerFootholdId = GetPlayerFootholdId(pUserLocal);
	if (nPlayerFootholdId != 0)
	{
		void* pById = reinterpret_cast<tGetFoothold>(ADDR_GetFoothold)(pSpace, nPlayerFootholdId);
		if (pById != nullptr)
		{
			g_pVacuumFoothold = pById;
			g_nVacuumFootholdId = nPlayerFootholdId;
			return;
		}
	}

	g_pVacuumFoothold = reinterpret_cast<tGetFootholdClosest>(ADDR_GetFootholdClosest)(pSpace, pt.x, pt.y);
}

static void ToggleVacuum(void* pUserLocal)
{
	if (!g_bActive)
	{
		// Switching on: the point is where the player stands right now.
		POINT pt;
		if (!GetPlayerPos(pUserLocal, pt))
		{
			return;   // no sane player position (wrong base, or not in a field yet): stay off
		}

		g_ptVacuum = pt;
		ResolveVacuumFoothold(pUserLocal, pt);
	}

	g_bActive = !g_bActive;
}

// Is this mob one the server handed to this player? That is CMob::IsActive, i.e. the second
// controller's own flag (+0x18 of the object at [mob+0x11C]). Mobs that are not ours are driven by
// the server's data instead: placing them every frame only fights that stream (it re-seeds them) and
// churns their move path, so they are left alone.
static bool IsLocallyActive(const void* pMob)
{
	void* pActiveIf = *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(pMob) + OFF_CMob_VecCtrl);
	if (pActiveIf == nullptr)
	{
		return false;
	}

	const char* pActive = reinterpret_cast<const char*>(pActiveIf) - OFF_VecObject_Interface;
	return *reinterpret_cast<const int*>(pActive + OFF_VecCtrl_Active) != 0;
}

// The per-frame vacuum step, run BEFORE the client's own update of the mob (see the file header): a
// mob that has really wandered off the point is put back on it - with the client's own position copies
// moved along, so everything that reads them in this frame (attack range, the hit rectangle, the drawn
// frame) sees the mob on the point - while a mob that is still where it was left is not touched at all,
// and a mob pulled less than MobVac::nIntervalMs ago is left to walk.
static void VacuumTick(void* pMob)
{
	const DWORD dwNow = GetTickCount();
	MobPin* pPin = PinFor(pMob);
	char* pMobBytes = reinterpret_cast<char*>(pMob);

	pPin->dwLastSeen = dwNow;

	const POINT ptMob = *reinterpret_cast<const POINT*>(pMobBytes + OFF_CMob_Pos);
	if (AbsDiff(ptMob.x, g_ptVacuum.x) <= MobVac::nRange
		&& AbsDiff(ptMob.y, g_ptVacuum.y) <= MobVac::nRange)
	{
		return;   // on the point (or close enough): nothing to do for this mob this frame
	}

	if (!IsLocallyActive(pMob))
	{
		return;   // not this player's mob: the server drives it, leave it be
	}

	if (dwNow < pPin->dwNextPull)
	{
		return;   // pulled recently: leave it alone for the interval
	}

	if (!TeleportMob(pMob, g_ptVacuum, g_pVacuumFoothold, g_nVacuumFootholdId))
	{
		return;   // the controller did not validate: never write through it
	}

	pPin->dwNextPull = dwNow + static_cast<DWORD>(MobVac::nIntervalMs);

	*reinterpret_cast<POINT*>(pMobBytes + OFF_CMob_Pos) = g_ptVacuum;
	*reinterpret_cast<POINT*>(pMobBytes + OFF_CMob_PosPrev) = g_ptVacuum;
}

// Own thread: keep the state tied to one character/field and poll the hotkey. It only reads globals.
static DWORD WINAPI MobVacThread(LPVOID /*param*/)
{
	for (;;)
	{
		Sleep(POLL_INTERVAL_MS);

		if (!g_bEnabled)
		{
			continue;
		}

		void* pField = GetCurrentField();
		void* pUser = *reinterpret_cast<void**>(ADDR_UserLocal_Instance);

		if (pUser != g_pLastUser || pField != g_pLastField)
		{
			// Map change, relogin or character change: the mobs of the old field are gone.
			g_pLastUser = pUser;
			g_pLastField = pField;
			g_bActive = false;
		}

		if (pUser == nullptr || pField == nullptr)
		{
			g_bActive = false;   // not in field (login screen, cash shop, stage transition)
			continue;
		}

		const HWND hForeground = GetForegroundWindow();
		DWORD dwPid = 0;
		if (hForeground != nullptr)
		{
			GetWindowThreadProcessId(hForeground, &dwPid);
		}

		const bool bCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool bKey = ((GetAsyncKeyState(VK_TOGGLE_KEY) & 0x8000) != 0)
			|| ((GetAsyncKeyState(VK_TOGGLE_KEY_PAD) & 0x8000) != 0);
		const bool bFocused = dwPid == GetCurrentProcessId();
		const bool bDown = bFocused && bCtrl && bKey;

		if (bDown && !g_bComboDown)
		{
			g_bComboDown = true;
			ToggleVacuum(pUser);
		}
		else if (!bDown)
		{
			g_bComboDown = false;
		}
	}

	return 0;
}

static void __fastcall CUserLocal_OnSetDead_Hook(void* pThis, void* /*edx*/, int bDead)
{
	g_bActive = false;
	g_origCUserLocalOnSetDead(pThis, bDead);
}

// Every mob, every frame.
//
// The pull runs BEFORE the client's own update: everything the mob decides inside that update -
// whether the player is in attack range, which skill or body attack to use, and the move path that
// carries the C->S 0xBC report - reads the mob's position, so a pull applied only afterwards is on
// the vacuum point for the eye and nowhere else, and the server keeps the old coordinates.
static void __fastcall CMob_Update_Hook(void* pThis, void* /*edx*/)
{
	if (pThis != nullptr && g_bActive && IsVacuumable(pThis))
	{
		VacuumTick(pThis);
	}

	g_origCMobUpdate(pThis);
}

void Hook_MobVac(bool enable)
{
	g_bEnabled = enable;

	if (!enable)
	{
		std::cout << "mob vacuum disabled by config (mobVac=false)" << std::endl;
		return;
	}

	// Clamp the tuning knobs once: a typo in config.ini must not be able to pin the mobs every frame
	// (which freezes them - the reason the interval exists) or stop the vacuum from acting at all.
	if (MobVac::nRange < MOB_VACUUM_RANGE_MIN) { MobVac::nRange = MOB_VACUUM_RANGE_MIN; }
	if (MobVac::nRange > MOB_VACUUM_RANGE_MAX) { MobVac::nRange = MOB_VACUUM_RANGE_MAX; }
	if (MobVac::nIntervalMs < static_cast<int>(MOB_VACUUM_INTERVAL_MIN_MS)) { MobVac::nIntervalMs = static_cast<int>(MOB_VACUUM_INTERVAL_MIN_MS); }
	if (MobVac::nIntervalMs > static_cast<int>(MOB_VACUUM_INTERVAL_MAX_MS)) { MobVac::nIntervalMs = static_cast<int>(MOB_VACUUM_INTERVAL_MAX_MS); }

	// Sanity checks: every entry point has to look like this client build, and the two functions
	// this module calls directly get their own byte checks before anything is hooked.
	if (*reinterpret_cast<unsigned char*>(ADDR_CMob_Update) != 0xB8
		|| *reinterpret_cast<unsigned char*>(ADDR_CUserLocal_OnSetDead) != 0x83
		|| *reinterpret_cast<unsigned char*>(ADDR_CVecCtrlMob_SetActive) != 0x55
		|| *reinterpret_cast<unsigned char*>(ADDR_CVecCtrlMob_SetActive + 1) != 0x8B
		|| *reinterpret_cast<unsigned char*>(ADDR_CVecCtrlMob_SetActive + 2) != 0xEC
		|| *reinterpret_cast<unsigned char*>(ADDR_GetFootholdClosest) != 0x55
		|| *reinterpret_cast<unsigned char*>(ADDR_GetFootholdClosest + 1) != 0x8B
		|| *reinterpret_cast<unsigned char*>(ADDR_GetFootholdClosest + 2) != 0xEC
		|| *reinterpret_cast<unsigned char*>(ADDR_GetFootholdClosest + 3) != 0x51
		|| *reinterpret_cast<unsigned char*>(ADDR_GetFoothold) != 0xB8
		|| *reinterpret_cast<unsigned char*>(ADDR_CMob_SetChaseTarget) != 0x56
		|| *reinterpret_cast<unsigned char*>(ADDR_CMob_SetChaseTarget + 1) != 0x8B
		|| *reinterpret_cast<unsigned char*>(ADDR_CMob_SetChaseTarget + 2) != 0xF1)
	{
		std::cout << "mob vacuum skipped: unexpected client build (CMob::Update=" << std::hex
			<< static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_CMob_Update))
			<< " OnSetDead=" << static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_CUserLocal_OnSetDead))
			<< " VecSetActive=" << static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_CVecCtrlMob_SetActive))
			<< " GetFootholdClosest=" << static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_GetFootholdClosest))
			<< std::dec << ")" << std::endl;
		return;
	}

	g_origCMobUpdate = reinterpret_cast<tCMobUpdate>(ADDR_CMob_Update);
	g_origCUserLocalOnSetDead = reinterpret_cast<tCUserLocalOnSetDead>(ADDR_CUserLocal_OnSetDead);

	const bool bMobHook = Memory::SetHook(true, reinterpret_cast<void**>(&g_origCMobUpdate),
		reinterpret_cast<void*>(&CMob_Update_Hook));
	const bool bDeathHook = Memory::SetHook(true, reinterpret_cast<void**>(&g_origCUserLocalOnSetDead),
		reinterpret_cast<void*>(&CUserLocal_OnSetDead_Hook));

	const HANDLE hThread = CreateThread(nullptr, 0, &MobVacThread, nullptr, 0, nullptr);

	if (bMobHook && bDeathHook && hThread != nullptr)
	{
		std::cout << "mob vacuum hook created (Ctrl+0 toggles; range=" << MobVac::nRange
			<< "px, interval=" << MobVac::nIntervalMs << "ms)" << std::endl;
	}
	else
	{
		std::cout << "mob vacuum hook FAILED (mob=" << bMobHook << " death=" << bDeathHook
			<< " thread=" << (hThread != nullptr) << ")" << std::endl;
	}
}

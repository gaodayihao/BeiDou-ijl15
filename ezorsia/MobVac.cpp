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
//   * The mob's live position is CMob+0x510 ({x,y}): the CLife interface subobject sits at CMob+4
//     and CMob::GetPos (0x006625A0, vtable slot 5) is `lea eax,[ecx+50Ch]`. CMob+0x518 is the
//     previous-position copy: CMob::Init (0x00662884) seeds +0x518/+0x51C from the spawn packet
//     and then copies them into +0x510/+0x514, and the tail of every Update refreshes the copy
//     (0x006685CD: +0x518 <- +0x510, +0x51C <- +0x514). Both are written so the frame sees no
//     velocity at all.
//
//   * CMob+0x188 is the CMobTemplate*. The template loader (0x0067CF06, at 0x0067EF42) writes a
//     WZ-derived boolean to template+0x208, and CMob::Update skips the per-mob HP tag loop when it
//     is set (0x00668748: `cmp [eax+208h], 0` / `jnz`) - that is, the mobs drawn with the big gage.
//     Those are the bosses, and they are left alone.
//
//   * The local player's position is the POINT at CUserLocal+0x1170 (CUser inherits it; the offset
//     is what CUser::GetPos returns). The CUserLocal members that move the character write that
//     very field off their own `this` - TryDoingTeleport 0x0094E878 and OnTeleport 0x0095977B are
//     `mov [edi+1170h],...`, CUser::Init 0x0092E5ED is `mov [ebx+1170h],eax` - so the offset can be
//     read straight off the singleton at 0x00BEBF98.
//
//   * The current field comes from the ZRef carrier at 0x00BEDED4: get_field (0x00437A0C) is
//     `mov esi,[0BEDED4h]` / `mov eax,[esi+4]`, so the CField* is the payload at carrier+4. A
//     change of that payload means map change / relogin.
//
// The hotkey and the state machine run on a thread this module owns rather than on a hook of
// CUserLocal::Update: BossHP.cpp already detours that entry (0x0094A144), and a second DetourAttach
// on the same entry would have to chain through the first one's trampoline. Polling from our own
// thread keeps this module out of the client's update chain entirely - it only reads globals.
//
// The mob write is deliberately done AFTER the client's own update: whatever the mob AI and its
// vector controller did during the frame is overwritten before the frame is drawn, so the mob stays
// on the stored point without disabling or re-routing the movement system.
// ---------------------------------------------------------------------------------------------

static const DWORD ADDR_CMob_Update = 0x006675A8;             // first byte B8
static const DWORD ADDR_CUserLocal_OnSetDead = 0x0095AF4E;    // first bytes 83 7C 24 04 00
static const DWORD ADDR_UserLocal_Instance = 0x00BEBF98;      // TSingleton<CUserLocal>::ms_pInstance
static const DWORD ADDR_FieldCarrier = 0x00BEDED4;            // ZRef carrier; CField* at +4

// While a mob is vacuumed its own movement is what walks it back onto the path we pulled it off, so
// it is put into the client's own "cannot act" state instead of hooking any of the movement code:
// +0x234 is the STUN value slot, which both the movement logic and CMob::DoAttack (0x0066D9C0)
// test - a mob with a non-zero slot does not move and does not attack. It is a plain data write, so
// it cannot break the client's calling conventions the way a hook on a hot vector-controller entry
// can (that route crashed the client during map load and was removed).
static const int OFF_CMob_StunValue = 0x234;

// Offsets inside CMob (relative to the CMob object itself, not to its CLife subobject).
static const int OFF_CMob_Template = 0x188;
static const int OFF_CMob_Pos = 0x510;       // {x,y} - what CLife::GetPos returns
static const int OFF_CMob_PosPrev = 0x518;   // {x,y} - refreshed from the above every Update

// Offset inside CMobTemplate: set for the mobs that use the big gage (bosses).
static const int OFF_CMobTemplate_Boss = 0x208;

// Offset of the local player's position inside CUserLocal (CUser::GetPos returns &(this+0x1170)).
static const int OFF_CUserLocal_Pos = 0x1170;

// CMob+0x50C holds a reference-counted pointer to the mob's vector controller (CLife's own accessor
// for it is vtable slot 4 = sub_66B6D2, which refcounts that very dword). Only read for the
// diagnostics: if the position write turns out not to survive the frame, that object is where the
// client keeps the motion the sprite follows.
static const int OFF_CMob_VecCtrlRef = 0x50C;

// CMob+0x118 holds the mob's CVecCtrl *as its IWzVector2D interface pointer* (the interface lives at
// object+0x0C): CMob::Update reads it as `mov eax,[ebx+118h]` and then uses `eax-12` as the CVecCtrl*
// for CVecCtrl::UpdatePassive. That object is what actually moves the mob - CMob+0x510 is only the
// copy it refreshes, which is why writing +0x510 alone changes nothing on screen.
static const int OFF_CMob_VecCtrlIf = 0x118;

// IWzVector2D::raw_Move (0x009B5E7F) is CVecCtrl's own teleport: it writes x/y, zeroes the four
// velocity doubles and (when a move path is attached) rebuilds it through SetMovePathAttribute, so
// the mob stops where it is put instead of continuing to walk.
//
// Slot index: the interface vtable starts at QueryInterface, so its layout is 0=QueryInterface,
// 1=AddRef, 2=Release, ... 8=get_x, 9=put_x, 10=get_y, 11=put_y, 12..15=get/put_x2/y2,
// **16 = raw_Move (+0x40)**, 17=raw_Offset, 18=raw_Scale, 19=raw_Insert, 20=raw_Remove,
// 21=raw_Init. (Taking the table 12 bytes early - at QueryInterface-3 - shifts every index up by
// three and lands on raw_Insert, which inserts a VARIANT and corrupts the heap.)
static const int VTBL_INDEX_VEC_RAW_MOVE = 16;
static const DWORD ADDR_VecCtrl_raw_Move = 0x009B5E7F;

// Ctrl+0 toggles; the game itself does not bind that combination.
static const int VK_TOGGLE_KEY = '0';

// How often the vacuum thread looks at the keyboard / the player position.
static const DWORD POLL_INTERVAL_MS = 15;

typedef void(__thiscall* tCMobUpdate)(void* pThis);
typedef void(__thiscall* tCUserLocalOnSetDead)(void* pThis, int bDead);
typedef long(__stdcall* tVecRawMove)(void* pVecIf, long x, long y);

// Distance (per axis, pixels) the mob may drift from the stored point before it is teleported again.
static const int VACUUM_SLACK = 4;

namespace MobVac { bool bDebug = false; }

static tCMobUpdate g_origCMobUpdate = nullptr;
static tCUserLocalOnSetDead g_origCUserLocalOnSetDead = nullptr;

// The mobs this vacuum session has touched (they carry a STUN value we have to take back when the
// vacuum ends). The list is only ever appended while the vacuum is on and is cleared whenever it
// toggles; entries are validated before being written to, because a mob may have died meanwhile.
static const int MAX_VAC_MOBS = 512;
static void* g_apVacMobs[MAX_VAC_MOBS];
static int g_nVacMobs = 0;

static bool g_bEnabled = false;      // config.ini switch - the thread only starts when set
static bool g_bActive = false;       // vacuum currently on (toggled by Ctrl+0)
static bool g_bComboDown = false;    // Ctrl+0 edge detection
static POINT g_ptVac = { 0, 0 };     // the point the mobs are pulled to (player position at toggle)
static void* g_pLastField = nullptr; // current CField*, to notice a map change
static void* g_pLastUser = nullptr;  // CUserLocal*, to notice a relogin/character change
static long g_nMobUpdates = 0;       // how often the mob hook fired (diagnostics only)

// raw_Move rebuilds the mob's move path, so the calls are rationed: the poll thread refills this
// budget every tick, which spreads "the whole map comes to me at once" over a few frames instead of
// rebuilding a hundred paths inside a single pool update.
static const long MOVE_BUDGET_PER_TICK = 4;
static long g_nMoveBudget = MOVE_BUDGET_PER_TICK;

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

// The mob's CVecCtrl, handed over as its IWzVector2D interface pointer.
static void* GetVecCtrlIf(const void* pMob)
{
	return *reinterpret_cast<void* const*>(reinterpret_cast<const char*>(pMob) + OFF_CMob_VecCtrlIf);
}

// Is this still a live CMob we may write to? A mob that died while the vacuum was on would leave a
// dangling pointer in the list, so the template pointer and its boss flag are checked first.
static bool LooksLikeMob(const void* pMob)
{
	const char* pTemplate = *reinterpret_cast<const char* const*>(
		reinterpret_cast<const char*>(pMob) + OFF_CMob_Template);

	const uintptr_t t = reinterpret_cast<uintptr_t>(pTemplate);
	if (t < 0x00010000u || t > 0x7FFFFFFFu)
	{
		return false;
	}

	const int nBoss = *reinterpret_cast<const int*>(pTemplate + OFF_CMobTemplate_Boss);
	return nBoss == 0 || nBoss == 1;
}

static void ForgetVacuumMobs()
{
	// Hand the mobs back their own state: the STUN value we set is ours, not the game's.
	for (int i = 0; i < g_nVacMobs; ++i)
	{
		if (LooksLikeMob(g_apVacMobs[i]))
		{
			char* pMob = reinterpret_cast<char*>(g_apVacMobs[i]);
			if (*reinterpret_cast<int*>(pMob + OFF_CMob_StunValue) == 1)
			{
				*reinterpret_cast<int*>(pMob + OFF_CMob_StunValue) = 0;
			}
		}
	}

	g_nVacMobs = 0;
}

// Teleport the mob through the vector controller - the object that really drives it.
//
// The gate is strict on purpose: slot 19 must be exactly CVecCtrl::raw_Move. Every write raw_Move
// performs lands inside the vector controller itself (interface-0x0C ... interface+0x104), so once
// the entry really is raw_Move the call cannot touch anything else; with a looser check (any data
// page plus any code pointer) a mob whose +0x118 is not a vector controller makes it call a foreign
// function with a foreign `this`, which corrupts that object and crashes the pool update later.
static bool ResolveVecRawMove(void* pVecIf, void*& pRawMoveOut)
{
	pRawMoveOut = nullptr;

	if (pVecIf == nullptr)
	{
		return false;
	}

	void** ppVtbl = *reinterpret_cast<void***>(pVecIf);
	if (ppVtbl == nullptr || !IsDataPointer(ppVtbl))
	{
		return false;
	}

	void* pRawMove = ppVtbl[VTBL_INDEX_VEC_RAW_MOVE];
	if (pRawMove != reinterpret_cast<void*>(ADDR_VecCtrl_raw_Move))
	{
		return false;
	}

	pRawMoveOut = pRawMove;
	return true;
}

static bool MoveVecCtrl(void* pVecIf, void* pRawMove, POINT pt)
{
	if (pVecIf == nullptr || pRawMove == nullptr)
	{
		return false;
	}

	reinterpret_cast<tVecRawMove>(pRawMove)(pVecIf, pt.x, pt.y);
	return true;
}

static void ToggleVacuum(void* pUserLocal)
{
	ForgetVacuumMobs();

	if (g_bActive)
	{
		g_bActive = false;
		if (MobVac::bDebug)
		{
			std::cout << "[mobvac] OFF" << std::endl;
		}
		return;
	}

	POINT pt;
	if (GetPlayerPos(pUserLocal, pt))
	{
		g_ptVac = pt;
		g_bActive = true;
		if (MobVac::bDebug)
		{
			std::cout << "[mobvac] ON point=(" << pt.x << "," << pt.y << "), mob ticks seen so far = " << g_nMobUpdates << std::endl;
		}
	}
	else if (MobVac::bDebug)
	{
		std::cout << "[mobvac] ON rejected: player position out of range (wrong CUserLocal base?)" << std::endl;
	}
}

// Own thread: keep the state tied to one character/field, poll the hotkey. It only reads globals -
// no client function is called from here.
static DWORD WINAPI MobVacThread(LPVOID /*param*/)
{
	for (;;)
	{
		Sleep(POLL_INTERVAL_MS);

		if (!g_bEnabled)
		{
			continue;
		}

		g_nMoveBudget = MOVE_BUDGET_PER_TICK;   // refill the teleport budget for this tick

		void* pField = GetCurrentField();
		void* pUser = *reinterpret_cast<void**>(ADDR_UserLocal_Instance);

		if (pUser != g_pLastUser || pField != g_pLastField)
		{
			// Map change, relogin or character change: the stored point belongs to the old map.
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
		const bool bKey = (GetAsyncKeyState(VK_TOGGLE_KEY) & 0x8000) != 0;
		const bool bFocused = dwPid == GetCurrentProcessId();
		const bool bDown = bFocused && bCtrl && bKey;

		static bool s_bWarnedUnfocused = false;
		if (bCtrl && bKey && !bFocused && !s_bWarnedUnfocused)
		{
			s_bWarnedUnfocused = true;
			if (MobVac::bDebug)
			{
				std::cout << "[mobvac] Ctrl+0 pressed while the game window is not the foreground one - ignored" << std::endl;
			}
		}

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

// The vacuumed mobs walk straight back onto the path they were pulled off, so they are put into the
// client's own "cannot act" state (see OFF_CMob_StunValue) instead of hooking movement code.
// Everybody else goes through untouched.
static void RememberVacuumMob(void* pMob)
{
	for (int i = 0; i < g_nVacMobs; ++i)
	{
		if (g_apVacMobs[i] == pMob)
		{
			return;
		}
	}

	if (g_nVacMobs < MAX_VAC_MOBS)
	{
		g_apVacMobs[g_nVacMobs++] = pMob;
	}
}

// Every mob, every frame: re-apply the stored point after the client's own update.
static void __fastcall CMob_Update_Hook(void* pThis, void* /*edx*/)
{
	g_origCMobUpdate(pThis);

	++g_nMobUpdates;

	if (!g_bActive || pThis == nullptr || !IsVacuumable(pThis))
	{
		return;
	}

	char* pMob = reinterpret_cast<char*>(pThis);
	RememberVacuumMob(pThis);

	// The client's own "this mob cannot act" state: it stops the mob walking back onto its path and
	// keeps the pile from hitting the player. Handed back when the vacuum ends.
	*reinterpret_cast<int*>(pMob + OFF_CMob_StunValue) = 1;

	POINT* pLive = reinterpret_cast<POINT*>(pMob + OFF_CMob_Pos);
	POINT* pPrev = reinterpret_cast<POINT*>(pMob + OFF_CMob_PosPrev);
	void* pVecIf = GetVecCtrlIf(pThis);
	void* pRawMove = nullptr;
	const bool bVecOk = ResolveVecRawMove(pVecIf, pRawMove);

	// The vector controller is what moves the mob and what the sprite follows; +0x510 is only the copy
	// it refreshes every frame. Teleport it only once the mob has drifted off the point: raw_Move
	// zeroes the velocity fields, so the mob then stays put - and the move path is not rebuilt
	// every single frame.
	const POINT ptBefore = *pLive;   // before our write: what the client left this frame
	const int nDx = ptBefore.x - g_ptVac.x;
	const int nDy = ptBefore.y - g_ptVac.y;
	const bool bOnPoint = nDx > -VACUUM_SLACK && nDx < VACUUM_SLACK && nDy > -VACUUM_SLACK && nDy < VACUUM_SLACK;

	bool bMoved = false;
	if (!bOnPoint)
	{
		if (g_nMoveBudget > 0)
		{
			--g_nMoveBudget;
			bMoved = MoveVecCtrl(pVecIf, pRawMove, g_ptVac);
		}

		*pLive = g_ptVac;
		*pPrev = g_ptVac;
	}

	// What matters here is `moved`: if it stays 0 while the mob is off the point, the vector
	// controller could not be reached (vecIf/vtbl are printed to see why).
	if (MobVac::bDebug)
	{
		static DWORD s_dwNextLog = 0;
		const DWORD dwNow = GetTickCount();
		if (dwNow >= s_dwNextLog)
		{
			s_dwNextLog = dwNow + 1000;
			const void* pVecRef = *reinterpret_cast<const void* const*>(pMob + OFF_CMob_VecCtrlRef);
			const void* pUser = *reinterpret_cast<void**>(ADDR_UserLocal_Instance);
			POINT ptNow = { 0, 0 };
			GetPlayerPos(const_cast<void*>(pUser), ptNow);
			std::cout << "[mobvac] mob 0x" << std::hex << pThis << std::dec
				<< " liveBefore=(" << ptBefore.x << "," << ptBefore.y << ")"
				<< " liveNow=(" << pLive->x << "," << pLive->y << ")"
				<< " want=(" << g_ptVac.x << "," << g_ptVac.y << ")"
				<< " playerNow=(" << ptNow.x << "," << ptNow.y << ")"
				<< " onPoint=" << bOnPoint << " moved=" << bMoved
				<< " vecOk=" << bVecOk << " rawMove=0x" << std::hex << pRawMove
				<< " vecIf=0x" << pVecIf
				<< " vecVtbl=0x" << (pVecIf != nullptr ? *reinterpret_cast<void**>(pVecIf) : nullptr)
				<< " vecRef=0x" << pVecRef << std::dec << std::endl;
		}
	}
}

void Hook_MobVac(bool enable)
{
	g_bEnabled = enable;

	if (!enable)
	{
		std::cout << "mob vacuum disabled by config (mobVac=false)" << std::endl;
		return;
	}

	// Sanity checks: every entry point we hook must look like this client build. CUserLocal::Update is
	// deliberately not among them - BossHP owns that entry (see the header).
	if (*reinterpret_cast<unsigned char*>(ADDR_CMob_Update) != 0xB8
		|| *reinterpret_cast<unsigned char*>(ADDR_CUserLocal_OnSetDead) != 0x83)
	{
		std::cout << "mob vacuum skipped: unexpected client build (CMob::Update=" << std::hex
			<< static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_CMob_Update))
			<< " OnSetDead=" << static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_CUserLocal_OnSetDead))
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
		std::cout << "mob vacuum hook created (Ctrl+0 toggles)" << std::endl;
	}
	else
	{
		std::cout << "mob vacuum hook FAILED (mob=" << bMobHook << " death=" << bDeathHook
			<< " thread=" << (hThread != nullptr) << ")" << std::endl;
	}
}

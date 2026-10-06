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
//     previous-position copy: CMob::Init (0x006628D0) seeds +0x518/+0x51C from the spawn packet
//     and then copies them into +0x510/+0x514, and the tail of every Update refreshes the copy
//     (0x006685CD: +0x518 <- +0x510, +0x51C <- +0x514). Both are written so the frame sees no
//     velocity at all.
//
//   * CMob+0x188 is the CMobTemplate*. The template loader (0x0067CF06, at 0x0067EF42) writes a
//     WZ-derived boolean to template+0x208, and CMob::Update skips the per-mob HP tag loop when it
//     is set (0x00668748: `cmp [eax+208h], 0` / `jnz`) - that is, the mobs drawn with the big gage.
//     Those are the bosses, and they are left alone.
//
//   * CUserLocal::Update (0x0094A144) is the local player's per-frame update and gives us both the
//     CUserLocal* and a once-per-frame place for the hotkey. CUser::GetPos is vtable slot 2 of the
//     CUserLocal vtable (0x00B3D1F4) and is `lea eax,[ecx+1170h]` (0x004B2386).
//
//   * The local player's position is the POINT at CUserLocal+0x1170 (CUser inherits it; the offset
//     is what CUser::GetPos returns). The CUserLocal members that move the character write that
//     very field off their own `this` - TryDoingTeleport 0x0094E878 and OnTeleport 0x0095977B are
//     `mov [edi+1170h],...`, CUser::Init 0x0092E5ED is `mov [ebx+1170h],eax` - so the offset can be
//     read straight off the object we get from CUserLocal::Update, with no vtable hop.
//
//   * get_field (0x00437A0C) returns the current CField*; a change of it means map change/relogin.
//
// The write is deliberately done AFTER the client's own update: whatever the mob AI and its vector
// controller did during the frame is overwritten before the frame is drawn, so the mob stays on the
// stored point without disabling or re-routing the movement system.
// ---------------------------------------------------------------------------------------------

static const DWORD ADDR_CMob_Update = 0x006675A8;             // first byte B8
static const DWORD ADDR_CUserLocal_Update = 0x0094A144;       // first byte B8
static const DWORD ADDR_CUserLocal_OnSetDead = 0x0095AF4E;    // first bytes 83 7C 24 04 00
static const DWORD ADDR_get_field = 0x00437A0C;

// Offsets inside CMob (relative to the CMob object itself, not to its CLife subobject).
static const int OFF_CMob_Template = 0x188;
static const int OFF_CMob_Pos = 0x510;       // {x,y} - what CLife::GetPos returns
static const int OFF_CMob_PosPrev = 0x518;   // {x,y} - refreshed from the above every Update

// Offset inside CMobTemplate: set for the mobs that use the big gage (bosses).
static const int OFF_CMobTemplate_Boss = 0x208;

// Offset of the local player's position inside CUserLocal (CUser::GetPos returns &(this+0x1170)).
static const int OFF_CUserLocal_Pos = 0x1170;

// Ctrl+0 toggles; the game itself does not bind that combination.
static const int VK_TOGGLE_KEY = '0';

typedef void(__thiscall* tCMobUpdate)(void* pThis);
typedef void(__thiscall* tCUserLocalUpdate)(void* pThis);
typedef void(__thiscall* tCUserLocalOnSetDead)(void* pThis, int bDead);
typedef void* (__cdecl* tGetField)(void);

static tCMobUpdate g_origCMobUpdate = nullptr;
static tCUserLocalUpdate g_origCUserLocalUpdate = nullptr;
static tCUserLocalOnSetDead g_origCUserLocalOnSetDead = nullptr;

static bool g_bEnabled = false;      // config.ini switch - the hooks are only installed when set
static bool g_bActive = false;       // vacuum currently on (toggled by Ctrl+0)
static bool g_bComboDown = false;    // Ctrl+0 edge detection
static POINT g_ptVac = { 0, 0 };     // the point the mobs are pulled to (player position at toggle)
static void* g_pLastField = nullptr; // current CField*, to notice a map change
static void* g_pLastUser = nullptr;  // CUserLocal*, to notice a relogin/character change

// The local player's position, read straight off the CUserLocal we are handed by its own Update.
// The sanity window is the guard against a wrong base: a misread would give a wild pair, while real
// map coordinates stay far inside it.
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

static void ToggleVacuum(void* pUserLocal)
{
	if (g_bActive)
	{
		g_bActive = false;
		return;
	}

	POINT pt;
	if (GetPlayerPos(pUserLocal, pt))
	{
		g_ptVac = pt;
		g_bActive = true;
	}
}

// Once per frame: keep the state tied to one character/field, poll the hotkey.
static void __fastcall CUserLocal_Update_Hook(void* pThis, void* /*edx*/)
{
	g_origCUserLocalUpdate(pThis);

	if (!g_bEnabled)
	{
		return;
	}

	void* pField = reinterpret_cast<tGetField>(ADDR_get_field)();
	if (pThis != g_pLastUser || pField != g_pLastField)
	{
		// Map change, relogin or character change: the stored point belongs to the old map.
		g_pLastUser = pThis;
		g_pLastField = pField;
		g_bActive = false;
	}

	const HWND hForeground = GetForegroundWindow();
	DWORD dwPid = 0;
	if (hForeground != nullptr)
	{
		GetWindowThreadProcessId(hForeground, &dwPid);
	}

	const bool bDown = dwPid == GetCurrentProcessId()
		&& (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0
		&& (GetAsyncKeyState(VK_TOGGLE_KEY) & 0x8000) != 0;

	if (bDown && !g_bComboDown)
	{
		g_bComboDown = true;
		ToggleVacuum(pThis);
	}
	else if (!bDown)
	{
		g_bComboDown = false;
	}
}

static void __fastcall CUserLocal_OnSetDead_Hook(void* pThis, void* /*edx*/, int bDead)
{
	g_bActive = false;
	g_origCUserLocalOnSetDead(pThis, bDead);
}

// Every mob, every frame: re-apply the stored point after the client's own update.
static void __fastcall CMob_Update_Hook(void* pThis, void* /*edx*/)
{
	g_origCMobUpdate(pThis);

	if (!g_bActive || pThis == nullptr || !IsVacuumable(pThis))
	{
		return;
	}

	char* pMob = reinterpret_cast<char*>(pThis);
	*reinterpret_cast<POINT*>(pMob + OFF_CMob_Pos) = g_ptVac;
	*reinterpret_cast<POINT*>(pMob + OFF_CMob_PosPrev) = g_ptVac;
}

void Hook_MobVac(bool enable)
{
	g_bEnabled = enable;

	if (!enable)
	{
		std::cout << "mob vacuum disabled by config (mobVac=false)" << std::endl;
		return;
	}

	// Sanity checks: the three entry points we hook must look like this client build.
	if (*reinterpret_cast<unsigned char*>(ADDR_CMob_Update) != 0xB8
		|| *reinterpret_cast<unsigned char*>(ADDR_CUserLocal_Update) != 0xB8
		|| *reinterpret_cast<unsigned char*>(ADDR_CUserLocal_OnSetDead) != 0x83)
	{
		std::cout << "mob vacuum skipped: unexpected client build" << std::endl;
		return;
	}

	g_origCMobUpdate = reinterpret_cast<tCMobUpdate>(ADDR_CMob_Update);
	g_origCUserLocalUpdate = reinterpret_cast<tCUserLocalUpdate>(ADDR_CUserLocal_Update);
	g_origCUserLocalOnSetDead = reinterpret_cast<tCUserLocalOnSetDead>(ADDR_CUserLocal_OnSetDead);

	const bool bMobHook = Memory::SetHook(true, reinterpret_cast<void**>(&g_origCMobUpdate),
		reinterpret_cast<void*>(&CMob_Update_Hook));
	const bool bUserHook = Memory::SetHook(true, reinterpret_cast<void**>(&g_origCUserLocalUpdate),
		reinterpret_cast<void*>(&CUserLocal_Update_Hook));
	const bool bDeathHook = Memory::SetHook(true, reinterpret_cast<void**>(&g_origCUserLocalOnSetDead),
		reinterpret_cast<void*>(&CUserLocal_OnSetDead_Hook));

	if (bMobHook && bUserHook && bDeathHook)
	{
		std::cout << "mob vacuum hook created (Ctrl+0 toggles)" << std::endl;
	}
	else
	{
		std::cout << "mob vacuum hook FAILED" << std::endl;
	}
}

#include "stdafx.h"
#include "ItemWndExpanded.h"
#include "AddyLocations.h"

// ---------------------------------------------------------------------------------------------
// Item window: the expanded "every tab at once" layout
//
// The window's layout lives in exactly one field, CUIItem+604h: 0 = narrow (one tab, scrollbar,
// 155px wide), 1 = wide (every tab side by side, no scrollbar, 583px wide). CUIItem::CUIItem fills
// it once from CConfig::GetInventoryExpanded, and OnCreate / Draw / GetItemSlotRect /
// GetSlotPositionFromPoint / OnTabChanged all read that same field, so what the constructor reads is
// what the whole window does:
//
//   0081C4BE  E8 17 31 C8 FF        call CConfig::GetInventoryExpanded
//   0081C4C3  89 86 04 06 00 00     mov  [esi+604h], eax
//
// The button that switches the layout writes the same value back through CConfig's setter, so the
// whole feature hangs on one per-character registry value:
//
//   0081E541  ...                  CUIItem's expand/collapse button handler
//             [obj+604h] toggled, then CConfig::SetInventoryExpanded(0x0049F62C), then the window is
//             destroyed and rebuilt from the field
//
// Two independent things are wrong with it, and each half below fixes one.
//
// PART 1 - the default a character with no stored value gets.
// CConfig::GetInventoryExpanded asks the game-option key for "InventoryExpanded" and passes
// CConfig::GetOpt_BOOL a hard-coded default for the absent case:
//   0049F603  6A 00                push 0                 <- the only byte part 1 changes
//   0049F605  50                   push eax               ; "InventoryExpanded"
//   0049F606  6A 02                push 2                 ; game-option key
//   0049F608  8B CE                mov  ecx, esi
//   0049F60A  E8 CC F7 FF FF       call CConfig::GetOpt_BOOL
// Flipping that immediate to 1 makes a character that never touched the button open the window
// expanded. A value that does exist still beats the default, and the button still flips the field
// itself, so a player who collapses the window keeps it collapsed for the rest of that session.
//
// PART 2 - the value the button writes does not survive.
// CConfig::SaveCharacter (0x0049D90D) is the only code in the client that enumerates and deletes
// registry values (a single xref each for RegEnumValueA and RegDeleteValueA). It wipes every value
// of the game-option key and then writes back only its own option list, which has no
// InventoryExpanded in it:
//   0049DA62  FF 34 81              push [ecx+eax*4]      ; value name
//   0049DA65  FF B6 C8 00 00 00     push [esi+0C8h]       ; game-option key = CConfig+0xC8
//   0049DA6B  FF 15 B8 05 BF 00     call RegDeleteValueA
// It runs on leaving the game (CWvsContext::OnLeaveGame), on the game-option dialog's OK
// (CUIGameOpt::SetRet), on status-bar creation and in CConfig's destructor, so the choice is gone
// before the next session could read it.
// Rather than doing surgery inside that delete loop, this part reuses the two CConfig accessors the
// value already flows through: read it while the key still holds it, let the client wipe and rewrite
// its own list, then write this one value back. Nothing else about the save changes.
//
// Addresses and every byte pattern below were confirmed byte for byte in the shipped BeiDou.exe
// (see the record doc, §6). Each site is verified before it is touched, so a different client build
// is skipped instead of being patched with something meaningless.
// Reverse-engineering record: docs/客户端逆向-背包默认展平.md
// ---------------------------------------------------------------------------------------------

static const DWORD STRINGPOOL_ID_INVENTORY_EXPANDED = 0x1197;	// 4503 = "InventoryExpanded"

static const unsigned char OP_PUSH_IMM8 = 0x6A;
static const unsigned char DEFAULT_NARROW = 0x00;
static const unsigned char DEFAULT_EXPANDED = 0x01;
static const unsigned char OP_CALL = 0xE8;
static const DWORD EXPECTED_CALL_REL32 = 0xFFFFF7CC;	// 0x0049F60B -> 0x0049EDDB GetOpt_BOOL

// The prologue of CConfig::SaveCharacter: "mov eax, offset <handler>", "call __EH_prolog", then the
// "sub esp, 128h" and the "cmp dword ptr [esi+0C8h], 0" that gates the entire save on the
// game-option key this module reads.
static const unsigned char OP_MOV_EAX = 0xB8;
static const DWORD OFF_SaveChar_KeyCmp = 19;			// 83 BE C8 00 00 00 00
static const DWORD EXPECT_SaveChar_KeyCmpHead = 0x00C8BE83;	// "83 BE C8 00"
static const unsigned short EXPECT_SaveChar_KeyCmpTail = 0x0000;	// "00 00"

// Both CConfig accessors of this value fetch the same StringPool string (id 4503) right after their
// exception prologue, which is what pins an address as the getter or the setter of this one option.
static bool IsInvExpandedAccessor(DWORD dwAddress)
{
	return *reinterpret_cast<unsigned char*>(dwAddress) == OP_MOV_EAX
		&& *reinterpret_cast<unsigned char*>(dwAddress + 0x0F) == 0x68
		&& *reinterpret_cast<DWORD*>(dwAddress + 0x10) == STRINGPOOL_ID_INVENTORY_EXPANDED;
}

// PART 1 ---------------------------------------------------------------------------------------

void Hook_ItemWndExpanded(bool enable)
{
	if (!enable)
	{
		std::cout << "Item window expanded by default disabled (itemWndExpanded=false)" << std::endl;
		return;
	}

	// The site must still be "push <default value>" immediately followed by the GetOpt_BOOL call. On
	// a different client build the immediate would belong to something else, so that build is left
	// untouched instead of having an unrelated byte flipped.
	const unsigned char ucOpcode = *reinterpret_cast<unsigned char*>(dwInvExpandedDefaultPush);
	const unsigned char ucDefault = *reinterpret_cast<unsigned char*>(dwInvExpandedDefaultValue);
	const unsigned char ucCallOpcode = *reinterpret_cast<unsigned char*>(dwInvExpandedGetOptCall);
	const DWORD dwCallRel32 = *reinterpret_cast<DWORD*>(dwInvExpandedGetOptCall + 1);

	if (ucOpcode != OP_PUSH_IMM8 || ucDefault != DEFAULT_NARROW
		|| ucCallOpcode != OP_CALL || dwCallRel32 != EXPECTED_CALL_REL32)
	{
		std::cout << "Item window expanded by default skipped: unexpected client build at 0x"
			<< std::hex << dwInvExpandedDefaultPush << std::dec << std::endl;
		return;
	}

	Memory::WriteByte(dwInvExpandedDefaultValue, DEFAULT_EXPANDED);

	if (*reinterpret_cast<unsigned char*>(dwInvExpandedDefaultValue) == DEFAULT_EXPANDED)
	{
		std::cout << "Item window expanded by default applied (GetOpt_BOOL default 0 -> 1)" << std::endl;
	}
	else
	{
		std::cout << "Item window expanded by default FAILED" << std::endl;
	}
}

// PART 2 ---------------------------------------------------------------------------------------

typedef int(__thiscall* tGetInventoryExpanded)(void* pConfig);
typedef void(__thiscall* tSetInventoryExpanded)(void* pConfig, int bExpanded);
typedef void*(__thiscall* tSaveCharacter)(void* pConfig);

static tGetInventoryExpanded g_getInventoryExpanded = nullptr;
static tSetInventoryExpanded g_setInventoryExpanded = nullptr;
static tSaveCharacter g_origSaveCharacter = nullptr;

// __fastcall: "this" in ECX, EDX unused. MSVC emits the same callee cleanup the original __thiscall
// uses for a function without stack arguments, the shape NewCharEquipFix.cpp already relies on.
static void* __fastcall SaveCharacter_Hook(void* pConfig, void* /*edx*/)
{
	// Mirrors the client's own gate - SaveCharacter only touches the key when it is open - and keeps
	// the two calls below out of the shutdown paths where there is nothing to preserve.
	void* pGameOptKey = (pConfig != nullptr)
		? *reinterpret_cast<void**>(static_cast<char*>(pConfig) + OFF_CConfig_GameOptKey)
		: nullptr;

	if (pGameOptKey == nullptr)
	{
		return g_origSaveCharacter(pConfig);
	}

	const int nExpanded = g_getInventoryExpanded(pConfig);	// read it while the key still has it
	void* pResult = g_origSaveCharacter(pConfig);		// the client wipes the key, then writes its list
	g_setInventoryExpanded(pConfig, nExpanded);		// put the player's own choice back
	return pResult;
}

void Hook_ItemWndRememberChoice(bool enable)
{
	if (!enable)
	{
		std::cout << "Item window layout choice not preserved (itemWndRemember=false)" << std::endl;
		return;
	}

	// Both accessors have to still be the getter/setter of this one option, and the save function has
	// to still be the one whose key check uses CConfig+0xC8 - the offset the hook above reads.
	const DWORD dwKeyCmpHead = *reinterpret_cast<DWORD*>(dwSaveCharacter + OFF_SaveChar_KeyCmp);
	const unsigned short wKeyCmpTail = *reinterpret_cast<unsigned short*>(dwSaveCharacter + OFF_SaveChar_KeyCmp + 4);

	if (!IsInvExpandedAccessor(dwInvExpandedGet) || !IsInvExpandedAccessor(dwInvExpandedSet)
		|| *reinterpret_cast<unsigned char*>(dwSaveCharacter) != OP_MOV_EAX
		|| dwKeyCmpHead != EXPECT_SaveChar_KeyCmpHead || wKeyCmpTail != EXPECT_SaveChar_KeyCmpTail)
	{
		std::cout << "Item window layout choice not preserved: unexpected client build at 0x"
			<< std::hex << dwSaveCharacter << std::dec << std::endl;
		return;
	}

	g_getInventoryExpanded = reinterpret_cast<tGetInventoryExpanded>(dwInvExpandedGet);
	g_setInventoryExpanded = reinterpret_cast<tSetInventoryExpanded>(dwInvExpandedSet);
	g_origSaveCharacter = reinterpret_cast<tSaveCharacter>(dwSaveCharacter);

	if (Memory::SetHook(true, reinterpret_cast<void**>(&g_origSaveCharacter),
		reinterpret_cast<void*>(&SaveCharacter_Hook)))
	{
		std::cout << "Item window layout choice kept across sessions (CConfig::SaveCharacter)" << std::endl;
	}
	else
	{
		std::cout << "Item window layout choice hook FAILED" << std::endl;
	}
}

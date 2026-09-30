#include "stdafx.h"
#include "NewCharEquipFix.h"
#include "Memory.h"

// ---------------------------------------------------------------------------------------------
// BUG-023 - new-character equipment list window: NULL slot list dereference (client side)
//
// Getter: sub_61828B at 0x0061828B. __thiscall, "this" = CLogin*, stack args = out ZXString* and
// slot index; returns the out pointer (the caller pushes that return value into the next
// ZXString::operator=, see the call site at 0x00618F84 -> 0x00618F89), so the replacement must
// keep returning it.
//
//   00618294  cmp  byte ptr [ecx+234h],dl   ; gender flag
//   006182A0  mov  eax,[ecx+eax*4+244h]     ; male   slot list data pointer  <- NULL when empty
//   006182A9  mov  eax,[ecx+eax*4+268h]     ; female slot list data pointer
//   006182B3  add  eax,4                    ; first item's name (item = {int id; ZXString name;})
//   006182B6  push eax
//   006182B9  mov  [esi],edx                ; *out = 0
//   006182BB  call ZXString<char>::operator=   <- reads [eax] => fault at 0x00000004
//
// Fix: keep the client's own "empty out string" prologue (0x006182B9) and skip the assignment when
// the slot list is NULL, i.e. the row draws as an empty name instead of faulting. Nothing else in
// the client is touched: the caller still gets its out pointer back and copies it as usual.
// ---------------------------------------------------------------------------------------------

static const DWORD ADDR_GetNewCharEquipName = 0x0061828B;

static const int OFF_CLogin_GenderFlag  = 0x234;  // byte: 0 = male, non-zero = female
static const int OFF_CLogin_EquipMale   = 0x244;  // 9 slot data pointers
static const int OFF_CLogin_EquipFemale = 0x268;  // 9 slot data pointers
static const int NEWCHAR_EQUIP_SLOTS    = 9;      // the paint loop walks slot 0..8

// Displacement fields inside the shipped getter; checked before hooking so a different client
// build (other offsets) is left alone instead of being fed this module's layout.
static const DWORD OFF_GetName_GenderDisp = 0x0B;  // 0x00618296 -> 0x00000234
static const DWORD OFF_GetName_MaleDisp   = 0x18;  // 0x006182A3 -> 0x00000244
static const DWORD OFF_GetName_FemaleDisp = 0x21;  // 0x006182AC -> 0x00000268

typedef int* (__thiscall* tGetNewCharEquipName)(void* pCLogin, int* pDest, int slot);
static tGetNewCharEquipName g_origGetNewCharEquipName = nullptr;

// __fastcall: "this" in ECX, EDX unused, the two stack args follow - MSVC emits the same "ret 8"
// callee cleanup the original __thiscall uses.
static int* __fastcall GetNewCharEquipName_Hook(void* pCLogin, void* /*edx*/, int* pDest, int slot)
{
	if (pDest == nullptr)
	{
		return nullptr;
	}

	// Every path has to leave the out string defined: the shipped getter always stores 0 into it
	// before the assignment (0x006182B9) and the callers release whatever it holds, so a stale
	// pointer left behind here would be released a second time.
	if (pCLogin == nullptr)
	{
		*pDest = 0;
		return pDest;
	}

	// The shipped paint loop only walks 0..8; an out-of-range slot becomes an empty name instead of
	// a wild read into CLogin's neighbouring fields.
	if (slot < 0 || slot >= NEWCHAR_EQUIP_SLOTS)
	{
		*pDest = 0;
		return pDest;
	}

	char* pBase = static_cast<char*>(pCLogin);
	const bool bFemale = (*reinterpret_cast<unsigned char*>(pBase + OFF_CLogin_GenderFlag) != 0);
	const int  nArrayOffset = bFemale ? OFF_CLogin_EquipFemale : OFF_CLogin_EquipMale;
	void* pSlotList = *reinterpret_cast<void**>(pBase + nArrayOffset + slot * 4);

	if (pSlotList == nullptr)
	{
		// Same value the client stores before the assignment (0x006182B9): an empty ZXString. The
		// caller releases the temporary that owns it, so the field has to stay 0.
		*pDest = 0;
		return pDest;
	}

	return g_origGetNewCharEquipName(pCLogin, pDest, slot);
}

void Hook_NewCharEquipNullGuard(bool enable)
{
	if (!enable)
	{
		std::cout << "NewChar equip null guard disabled by config (fixNewCharEquipNull=false)" << std::endl;
		return;
	}

	// Build check 1: the getter starts with "push ebp" (0x55) in this client build.
	if (*reinterpret_cast<unsigned char*>(ADDR_GetNewCharEquipName) != 0x55)
	{
		std::cout << "NewChar equip null guard skipped: unexpected client build at 0x" << std::hex
			<< ADDR_GetNewCharEquipName << std::dec << std::endl;
		return;
	}

	// Build check 2: the CLogin field offsets this module hardcodes must still be the ones the
	// shipped getter uses.
	const DWORD dwGenderDisp = *reinterpret_cast<DWORD*>(ADDR_GetNewCharEquipName + OFF_GetName_GenderDisp);
	const DWORD dwMaleDisp = *reinterpret_cast<DWORD*>(ADDR_GetNewCharEquipName + OFF_GetName_MaleDisp);
	const DWORD dwFemaleDisp = *reinterpret_cast<DWORD*>(ADDR_GetNewCharEquipName + OFF_GetName_FemaleDisp);
	if (dwGenderDisp != static_cast<DWORD>(OFF_CLogin_GenderFlag)
		|| dwMaleDisp != static_cast<DWORD>(OFF_CLogin_EquipMale)
		|| dwFemaleDisp != static_cast<DWORD>(OFF_CLogin_EquipFemale))
	{
		std::cout << "NewChar equip null guard skipped: CLogin layout differs (gender=0x" << std::hex
			<< dwGenderDisp << " male=0x" << dwMaleDisp << " female=0x" << dwFemaleDisp << std::dec
			<< ")" << std::endl;
		return;
	}

	g_origGetNewCharEquipName = reinterpret_cast<tGetNewCharEquipName>(ADDR_GetNewCharEquipName);

	if (Memory::SetHook(true, reinterpret_cast<void**>(&g_origGetNewCharEquipName),
		reinterpret_cast<void*>(&GetNewCharEquipName_Hook)))
	{
		std::cout << "NewChar equip null guard hook created (empty slot list -> empty name)" << std::endl;
	}
	else
	{
		std::cout << "NewChar equip null guard hook FAILED" << std::endl;
	}
}

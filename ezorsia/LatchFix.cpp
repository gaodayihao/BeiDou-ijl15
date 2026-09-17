#include "stdafx.h"
#include "LatchFix.h"
#include "Memory.h"

// ---------------------------------------------------------------------------------------------
// BUG-005 - mob abnormal status silently discarded (client side)
//
// CMob::CMob(CMobTemplate*) lives at 0x006621D9 and is reached from CreateMob (0x0066219D:
// ZAllocEx<ZAllocAnonSelector>::Alloc(1352) followed by the constructor).
//
// Facts (verified in the client binary):
//   * the constructor's field-init list covers +0x520/+0x524/+0x52C/+0x530/+0x538/+0x53C/+0x540
//     but never +0x528;
//   * the allocator (0x00403065) is a size-class free list that pops recycled blocks and does
//     not zero them, so +0x528 keeps whatever the previous occupant left there;
//   * CMob::OnStatSet (0x0066C34E) reads +0x528 as a gate: while it is non-zero every
//     movement-affecting status packet (FREEZE 0x100 / STUN / DOOM / BEF8B0) is queued at
//     mob+0x530 instead of applied;
//   * the queue only drains while the latch is zero (CMob::Update 0x00668397) and the latch is
//     only ever cleared by CMob::OnDoomed(1) (0x0066D710), which requires a DOOM status.
//
// Consequence: a mob whose recycled block carried a non-zero value there is immune to freeze
// and stun for its whole life (damage and death still work) - exactly the "random single mob
// refuses every debuff" symptom. It is invisible in the packet stream.
//
// Fix: finish the missing initialisation. Nothing else in the client is touched - the gate, the
// queue and the Doom logic stay exactly as shipped.
// ---------------------------------------------------------------------------------------------

static const DWORD ADDR_CMob_Ctor = 0x006621D9;

// Offset of the status latch inside CMob; the client only ever writes 0 or 1 here.
static const int OFF_CMob_Latch = 0x528;

// CreateMob passes the CMobTemplate* as the only stack argument.
typedef void(__thiscall* tCMobCtor)(void* pThis, void* pMobTemplate);
static tCMobCtor g_origCMobCtor = nullptr;

static void __fastcall CMobCtor_Hook(void* pThis, void* /*edx*/, void* pMobTemplate)
{
	g_origCMobCtor(pThis, pMobTemplate);

	if (pThis != nullptr)
	{
		*reinterpret_cast<int*>(reinterpret_cast<char*>(pThis) + OFF_CMob_Latch) = 0;
	}
}

void Hook_CMobCtorLatchFix(bool enable)
{
	if (!enable)
	{
		std::cout << "CMob latch fix disabled by config (fixMobLatch=false)" << std::endl;
		return;
	}

	// Sanity check: this client build starts the constructor with "mov eax, imm32" (B8).
	if (*reinterpret_cast<unsigned char*>(ADDR_CMob_Ctor) != 0xB8)
	{
		std::cout << "CMob latch fix skipped: unexpected client build at 0x" << std::hex << ADDR_CMob_Ctor
			<< std::dec << std::endl;
		return;
	}

	g_origCMobCtor = reinterpret_cast<tCMobCtor>(ADDR_CMob_Ctor);

	if (Memory::SetHook(true, reinterpret_cast<void**>(&g_origCMobCtor),
		reinterpret_cast<void*>(&CMobCtor_Hook)))
	{
		std::cout << "CMob latch fix hook created (mob+0x528 initialised to 0)" << std::endl;
	}
	else
	{
		std::cout << "CMob latch fix hook FAILED" << std::endl;
	}
}

#include "stdafx.h"
#include "GodMode.h"

// ---------------------------------------------------------------------------------------------
// Invincibility - see GodMode.h for what it does and why the client can do it at all.
//
// Facts read off the shipped binary (imagebase 0x400000, no ASLR - addresses are absolute):
//
//   * CUserLocal::SetDamaged (0x009581A9) is the client's only "my character took damage from a mob"
//     path. It is the CUserLocal override of the CUser virtual at slot 18 (+0x48) of the vtable
//     CUserLocal::CUserLocal installs (0x00948E2F: `mov [esi], offset off_B3D20C`), it reads the
//     mob's attack info (CMobTemplate::GetAttackInfo), takes
//     the defensive skills off the damage (CUserLocal::GetAchillesReduce / CalcBuffDefenseAttr),
//     writes the character's HP (the TSecType<long> at CUserLocal+0x1EB8), plays the hurt look
//     (CUser::MakeIncDecHPEffect, CAvatar::SetEmotion, play_skill_sound, Effect_General) and finally
//     builds and sends CP_UserHit: COutPacket(0x30) at 0x009593FC, CClientSocket::SendPacket at
//     0x00959585.
//
//   * CP_UserHit (0x30) has exactly four senders in the whole client. Three are inside
//     CUserLocal::Update (0x0094B14F, 0x0094B290, 0x0094B8D7) and report FIELD damage: they log
//     StringPool 0x1561 and none of them names a mob. The fourth is SetDamaged. So mob damage has a
//     single entry point, and it is the one this module gates.
//
//   * SetDamaged's prologue refuses to do anything while a "no damage until" timestamp is still in
//     the future. Two such fields live in the object (both are CUser's, which CUserLocal inherits):
//       +0x1F5C - read at 0x00958218, cleared at 0x00958236. Nothing else in the client reads it, so
//                 holding it open has no other effect at all;
//       +0x1F58 - read at 0x009581F2, cleared at 0x00958212. CUser::Update also reads it
//                 (0x009321EB, 0x009325CC) to hide the avatar's additional effect layers, i.e. this
//                 is the one that also drives the invincibility blink.
//     The game fills both with `now + <attack duration>` in CUserLocal::TryDoingMeleeAttack
//     (0x00951866, 0x009518BD) and CUserRemote::OnMeleeAttack (0x00980F7C, 0x00980FD6); that is what
//     the fields are for. This module writes +0x1F5C only, the pure damage gate, so the look of the
//     character is left alone.
//
//   * The clock both comparisons use is get_update_time (0x00987257), a millisecond tick.
//
//   * Debuffs arrive on their own packet. The server builds it in PacketCreator.giveDebuff: send
//     opcode GIVE_BUFF (0x20), a 16-byte (128-bit) stat mask written as two longs, then the values.
//     Disease.isFirst() is false for every disease, so the first long is always 0 and the second
//     long carries the disease bits - the same shape PacketCreator.cancelDebuff uses. The client's
//     GIVE_BUFF handler is CWvsContext::OnTemporaryStatSet (0x00A202BE); its first action is to
//     decode that mask (sub_781D0E, DecodeBuffer of 16 bytes at 0x00781D33) and it applies the stats
//     after. A packet whose mask carries disease bits and nothing else is therefore a debuff packet,
//     and dropping it means the client never applies the debuff. Only this player's own debuff can
//     arrive that way: the server sends the broadcast about OTHER players on a different opcode
//     (GIVE_FOREIGN_BUFF 0xC7) which never reaches this handler (see the hook below).
// ---------------------------------------------------------------------------------------------

static const DWORD ADDR_CUserLocal_SetDamaged = 0x009581A9;            // first byte B8
static const DWORD ADDR_CWvsContext_OnTemporaryStatSet = 0x00A202BE;   // first byte B8
static const DWORD ADDR_GetUpdateTime = 0x00987257;                    // first byte A1
static const DWORD ADDR_UserLocal_Instance = 0x00BEBF98;               // TSingleton<CUserLocal>::ms_pInstance
static const DWORD ADDR_FieldCarrier = 0x00BEDED4;                     // ZRef carrier; CField* at +4

// "No damage until" timestamp inside the CUserLocal object (see the file header).
static const int OFF_CUserLocal_NoDamageUntil = 0x1F5C;

// Offset of the SetDamaged slot inside the vtable CUserLocal::CUserLocal installs (0x00B3D20C; see
// the file header). Used to prove that the object at the singleton address really is a live
// CUserLocal before anything is written into it.
static const int OFF_Vtbl_SetDamaged = 0x48;

// How far the timestamp is pushed into the future, and how often the thread re-pushes it. The window
// only has to outlive one poll interval; keeping it short means a mode that is switched off stops
// being immune within a second even if the field could not be cleared. The client clears the field
// itself as soon as it is in the past, so nothing has to be undone on the way out.
static const int NO_DAMAGE_WINDOW_MS = 1000;

// How often the thread looks at the keyboard, in milliseconds.
static const DWORD POLL_INTERVAL_MS = 15;

// Ctrl+9 toggles; the game itself does not bind that combination. Both the top-row and the numpad
// nine are accepted, because which one "9" means is a keyboard-layout detail.
static const int VK_TOGGLE_KEY = '9';
static const int VK_TOGGLE_KEY_NUMPAD = VK_NUMPAD9;

// The disease bits of the GIVE_BUFF stat mask's second long, exactly as the server writes them
// (Disease in BeiDou-Server: SLOW 0x1, SEDUCE 0x80, ZOMBIFY 0x4000, CONFUSE 0x80000, STUN 0x2000000-
// 000000000, POISON 0x4000000000000, SEAL 0x8000000000000, DARKNESS 0x10000000000000, WEAKEN
// 0x4000000000000000, CURSE 0x8000000000000000).
static const unsigned __int64 DISEASE_MASK = 0xC01E000000084081ui64;

typedef int(__cdecl* tGetUpdateTime)();
typedef void(__thiscall* tOnTemporaryStatSet)(void* pThis, void* pPacket);

// CInPacket, the shape HpMpAlert.cpp verified against CInPacket::DecodeBuffer (0x00432257): the copy
// reads from *(this+8) + *(this+20) and advances *(this+20), so Data is at +0x08 and the read cursor
// at +0x14. At the moment a handler runs the cursor sits on the payload, just past the opcode.
struct CInPacket
{
	void* Vtbl;
	int Flags;
	unsigned char* Data;
	int Unk0C;
	int Unk10;
	int Position;
	int Size;
};

static tGetUpdateTime g_GetUpdateTime = reinterpret_cast<tGetUpdateTime>(ADDR_GetUpdateTime);
static tOnTemporaryStatSet g_origOnTemporaryStatSet = nullptr;

static bool g_bEnabled = false;      // config.ini switch - the thread only starts when set
static bool g_bActive = false;       // invincibility currently on (toggled by Ctrl+9)
static bool g_bComboDown = false;    // Ctrl+9 edge detection
static void* g_pLastUser = nullptr;  // CUserLocal*, to notice a relogin/character change

// Is this GIVE_BUFF packet a debuff? See the file header: the server writes the mask as two longs
// with the diseases always in the second one, so a mask whose first long is 0 and whose second long
// has disease bits and nothing else can only be a debuff packet.
//
// The 16 bytes are read without a length check on purpose: the client's own handler decodes exactly
// those 16 bytes from the very same cursor right after this hook returns, so they are readable by
// construction. The guard is only against a packet object that is not what it should be.
static bool IsDebuffPacket(const void* pPacketVoid)
{
	__try
	{
		const CInPacket* pPacket = static_cast<const CInPacket*>(pPacketVoid);
		if (pPacket == nullptr || pPacket->Data == nullptr)
		{
			return false;
		}

		const int nPosition = pPacket->Position;
		if (nPosition < 0 || nPosition > 0x10000)
		{
			return false;
		}

		unsigned __int64 nFirstMask = 0;
		unsigned __int64 nSecondMask = 0;
		memcpy(&nFirstMask, pPacket->Data + nPosition, sizeof(nFirstMask));
		memcpy(&nSecondMask, pPacket->Data + nPosition + sizeof(nFirstMask), sizeof(nSecondMask));

		return nFirstMask == 0 && nSecondMask != 0 && (nSecondMask & ~DISEASE_MASK) == 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
}

// get_field (0x00437A0C) in two instructions, without calling into the client.
static void* GetCurrentField()
{
	void* pCarrier = *reinterpret_cast<void**>(ADDR_FieldCarrier);
	return pCarrier != nullptr ? *reinterpret_cast<void**>(reinterpret_cast<char*>(pCarrier) + 4) : nullptr;
}

static bool IsDataPointer(const void* p)
{
	const uintptr_t v = reinterpret_cast<uintptr_t>(p);
	return v >= 0x00AF0000u && v < 0x00C00000u;   // where the client keeps its vtables
}

// Is the object at the singleton really a live CUserLocal? Its vtable has to sit in the client's own
// vtable region and that vtable's SetDamaged slot (0x00B3D20C + 0x48 = 0x00B3D254) has to hold
// CUserLocal::SetDamaged; a stale pointer left behind by a logout fails both. Nothing is written
// unless this passes, so a wrong base cannot corrupt whatever is there now.
static bool IsLiveUserLocal(void* pUser)
{
	if (pUser == nullptr)
	{
		return false;
	}

	void** ppVtbl = *reinterpret_cast<void***>(pUser);
	if (ppVtbl == nullptr || !IsDataPointer(ppVtbl))
	{
		return false;
	}

	return ppVtbl[OFF_Vtbl_SetDamaged / 4] == reinterpret_cast<void*>(ADDR_CUserLocal_SetDamaged);
}

// Hold the client's own invincibility state open: put the "no damage until" timestamp a window into
// the future, so the next SetDamaged returns from its prologue without touching HP, without the hurt
// look and without sending CP_UserHit. The field is plain writable data the client itself writes, so
// it is stored directly rather than through Memory::WriteInt's VirtualProtect dance.
static void SetNoDamageUntil(void* pUser, unsigned int nValue)
{
	*reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(pUser) + OFF_CUserLocal_NoDamageUntil) = nValue;
}

static void HoldInvincible(void* pUser)
{
	SetNoDamageUntil(pUser, static_cast<unsigned int>(g_GetUpdateTime() + NO_DAMAGE_WINDOW_MS));
}

// Own thread: keep the state tied to one character and poll the hotkey. It only reads globals.
static DWORD WINAPI GodModeThread(LPVOID /*param*/)
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

		if (pUser != g_pLastUser)
		{
			// Relogin or character change: the object the state was being held on is gone.
			g_pLastUser = pUser;
			g_bActive = false;
		}

		if (pUser == nullptr || pField == nullptr || !IsLiveUserLocal(pUser))
		{
			// Login screen, cash shop, or the object is not a live CUserLocal. A relogin always
			// passes through here, so the state cannot survive one on a recycled address either.
			// A map change inside the game is not one of these: the character keeps the state.
			g_bActive = false;
			continue;
		}

		if (g_bActive)
		{
			HoldInvincible(pUser);
		}

		const HWND hForeground = GetForegroundWindow();
		DWORD dwPid = 0;
		if (hForeground != nullptr)
		{
			GetWindowThreadProcessId(hForeground, &dwPid);
		}

		const bool bCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
		const bool bKey = ((GetAsyncKeyState(VK_TOGGLE_KEY) & 0x8000) != 0)
			|| ((GetAsyncKeyState(VK_TOGGLE_KEY_NUMPAD) & 0x8000) != 0);
		const bool bFocused = dwPid == GetCurrentProcessId();
		const bool bDown = bFocused && bCtrl && bKey;

		if (bDown && !g_bComboDown)
		{
			g_bComboDown = true;
			g_bActive = !g_bActive;

			if (!g_bActive)
			{
				// Switching off: clear the timestamp so the immunity ends now instead of running
				// out its last window.
				SetNoDamageUntil(pUser, 0);
			}
		}
		else if (!bDown)
		{
			g_bComboDown = false;
		}
	}

	return 0;
}

// The client's handler for the GIVE_BUFF (0x20) packet - the packet that carries this player's OWN
// temporary stats. CWvsContext::OnPacket (0x00A07A08) dispatches opcode 0x20 to it (case 32), and the
// packet names no character: whatever arrives here is the local player's. The debuffs the server
// broadcasts ABOUT other players ride a different opcode, GIVE_FOREIGN_BUFF (0xC7), which
// CWvsContext::OnPacket does not even have a case for - it goes through CUserPool::OnPacket
// (0x0097208C) into CUserPool::OnUserRemotePacket (0x009724F9) and touches a CUserRemote instead.
// So dropping a packet here can only ever hide this player's own debuff, never anyone else's.
//
// A debuff packet is dropped whole - it carries nothing but the disease, so nothing else is lost
// with it, and the client stays unaware of the debuff.
static void __fastcall CWvsContext_OnTemporaryStatSet_Hook(void* pThis, void* /*edx*/, void* pPacket)
{
	if (g_bActive && IsDebuffPacket(pPacket))
	{
		return;
	}

	g_origOnTemporaryStatSet(pThis, pPacket);
}

void Hook_GodMode(bool enable)
{
	g_bEnabled = enable;

	if (!enable)
	{
		std::cout << "god mode disabled by config (godMode=false)" << std::endl;
		return;
	}

	// Sanity checks: the two entry points have to look like this client build, and the tick the
	// timestamp is built from gets its own byte check before anything is hooked.
	if (*reinterpret_cast<unsigned char*>(ADDR_CUserLocal_SetDamaged) != 0xB8
		|| *reinterpret_cast<unsigned char*>(ADDR_CWvsContext_OnTemporaryStatSet) != 0xB8
		|| *reinterpret_cast<unsigned char*>(ADDR_GetUpdateTime) != 0xA1)
	{
		std::cout << "god mode skipped: unexpected client build (SetDamaged=" << std::hex
			<< static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_CUserLocal_SetDamaged))
			<< " OnTemporaryStatSet=" << static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_CWvsContext_OnTemporaryStatSet))
			<< " get_update_time=" << static_cast<int>(*reinterpret_cast<unsigned char*>(ADDR_GetUpdateTime))
			<< std::dec << ")" << std::endl;
		return;
	}

	g_origOnTemporaryStatSet = reinterpret_cast<tOnTemporaryStatSet>(ADDR_CWvsContext_OnTemporaryStatSet);

	const bool bStatHook = Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnTemporaryStatSet),
		reinterpret_cast<void*>(&CWvsContext_OnTemporaryStatSet_Hook));

	const HANDLE hThread = CreateThread(nullptr, 0, &GodModeThread, nullptr, 0, nullptr);

	if (bStatHook && hThread != nullptr)
	{
		std::cout << "god mode ready (Ctrl+9 toggles invincibility)" << std::endl;
	}
	else
	{
		std::cout << "god mode FAILED (stat=" << bStatHook
			<< " thread=" << (hThread != nullptr) << ")" << std::endl;
	}
}

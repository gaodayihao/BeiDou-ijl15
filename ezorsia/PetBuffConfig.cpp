#include "stdafx.h"
#include "PetBuffConfig.h"
#include "PetSkillSlot.h"
#include "Memory.h"

// ===== Reverse-engineering anchors (Angel.exe / BeiDou.exe, v83) ================================
// The packet pair is an Ursa addition; 0x1001 was free in both directions in the v83 opcode enums
// (0x1000 = SET_HPMPALERT / UPDATE_HPMPAALERT, 0x3713 = CUSTOM_PACKET, 0xFFFE = MAPLETV are the only
// values at or above it). Server side + rationale: Ursa-Server docs/client/003 §5.11 and
// tools/gen-opcodes.ps1's "Ursa additions" table.
//
// Layout, both directions: version(1) + 6 x int32 skillId (order = pet*2 + slot, 0 = empty).
//
// Receive side: CClientSocket::ProcessPacket (0x004965F1) -- the same function HpMpAlert.cpp hooks,
// but the LAYOUT below is read off the client, not copied from that file (its CInPacket offsets are
// wrong; see the struct comment). The buffer keeps the wire framing: RawSeq(2) | DataLen(2) |
// opcode(2) | payload, so the opcode sits at Data + 4 and the payload at Data + 6.
// =================================================================================================

static const DWORD ADDR_ClientSocket = 0x00BE7914;
static const DWORD ADDR_ClientSocket_SendPacket = 0x0049637B;
static const DWORD ADDR_ClientSocket_ProcessPacket = 0x004965F1;
static const DWORD ADDR_CInPacket_Decode1 = 0x004065F3;
static const DWORD ADDR_CInPacket_Decode2 = 0x0042470C;
static const DWORD ADDR_CInPacket_Decode4 = 0x00406629;

// The packet's read cursor, the one field Decode2 itself uses (`*(this + 5)` there). A live packet
// dump showed it at 4 on every received packet -- right after AppendBuffer consumed the 4-byte frame
// header (RawSeq | DataLen) -- which is also why the opcode sits at Data + 4.
static const int OFF_CInPacket_Position = 0x14;

static const int nOpcodePetBuffConfig = 0x1001;
static const unsigned char nConfigVersion = 1;
static const int kPetCount = 3;
static const int kSlotCount = 2;
static const int kValueCount = kPetCount * kSlotCount;
static const int nPayloadSize = 1 + 4 * kValueCount;            // version + six int32
static const int nReceivePrefix = 6;                            // bytes before the payload (see above)

// COutPacket, laid out the way HpMpAlert.cpp / PetAutoBuff.cpp declare it: Data/Size describe a buffer
// the caller owns, so nothing here or in the client has to allocate or free.
struct COutPacket
{
    int Loopback;
    union
    {
        unsigned char* Data;
        void* Unk;
        unsigned short* Header;
    };
    unsigned long Size;
    unsigned int Offset;
    int EncryptedByShanda;
};

// The receive view: only the buffer pointer and the read cursor are used, and both were confirmed
// against a live dump (`Data` = the dword at +0x08, cursor = 4 at +0x14 on every packet). The length
// is deliberately NOT modelled: the copy ManipulatePacket builds carries 0 at +0x18, and the buffer's
// first four bytes are the shuffled frame header -- reading the payload through the client's own
// Decode1/Decode2/Decode4 is what keeps this module out of that guessing game (they hook the real
// bounds check and throw when the packet is short).
// Do NOT copy HpMpAlert.cpp's declaration: it puts Data at +0x04, which is a flag (2 when the copy is
// built), so every read goes to address 6 -- an access violation that its own __try swallows,
// leaving that module's receive path silently dead.
struct CInPacket
{
    void* Vtbl;                     // +0x00
    int Flags;                      // +0x04  (2 on the copy ProcessPacket gets)
    unsigned char* Data;            // +0x08
    int Unk0C;                      // +0x0C
    int Unk10;                      // +0x10
    int Position;                   // +0x14  (4: right past the frame header)
    int Unk18;                      // +0x18  (0 on the copy -- NOT the length)
};

typedef void(__fastcall* SendPacket_t)(void* pSocket, void* edx, void* pPacket);
typedef void(__fastcall* ProcessPacket_t)(void* pThis, void* edx, CInPacket* pPacket);
typedef unsigned char(__fastcall* Decode1_t)(void* pPacket, void* edx);
typedef unsigned short(__fastcall* Decode2_t)(void* pPacket, void* edx);
typedef unsigned int(__fastcall* Decode4_t)(void* pPacket, void* edx);

static auto _send_packet = reinterpret_cast<SendPacket_t>(ADDR_ClientSocket_SendPacket);
static auto _decode1 = reinterpret_cast<Decode1_t>(ADDR_CInPacket_Decode1);
static auto _decode2 = reinterpret_cast<Decode2_t>(ADDR_CInPacket_Decode2);
static auto _decode4 = reinterpret_cast<Decode4_t>(ADDR_CInPacket_Decode4);

// One free function for both halves: reading the six values out of a received buffer and writing them
// into one. Keeping the order (pet*2 + slot) in a single place is what makes the two directions agree.
static int SlotIndex(int nPet, int nSlot)
{
    return nPet * kSlotCount + nSlot;
}

static void WriteRaw(void* pSocket, const unsigned char* pPayload, unsigned long nSize)
{
    COutPacket packet;
    packet.Loopback = 0;
    packet.Data = const_cast<unsigned char*>(pPayload);
    packet.Size = nSize;
    packet.Offset = 0;
    packet.EncryptedByShanda = 0;

    _send_packet(pSocket, nullptr, &packet);
}

void PetBuffConfig_Send()
{
    void* pSocket = *reinterpret_cast<void**>(ADDR_ClientSocket);
    if (pSocket == nullptr)
    {
        return;
    }

    // opcode(2) + version(1) + 6 x int32 = 27 bytes. The buffer is oversized and the write is bounded
    // on purpose: one byte past a tight array trips the /GS cookie, and __report_gsfailure ends the
    // process through __fastfail -- no crash dump, no catchable exception (PetAutoBuff.cpp documents
    // the same trap).
    unsigned char aPayload[32];
    int n = 0;
    aPayload[n++] = static_cast<unsigned char>(nOpcodePetBuffConfig & 0xFF);
    aPayload[n++] = static_cast<unsigned char>((nOpcodePetBuffConfig >> 8) & 0xFF);
    aPayload[n++] = nConfigVersion;
    for (int nPet = 0; nPet < kPetCount; ++nPet)
    {
        for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
        {
            const int nSkillId = PetSkillSlot::GetSkill(nPet, nSlot);
            memcpy(aPayload + n, &nSkillId, sizeof(int));
            n += sizeof(int);
        }
    }

    if (n != 2 + nPayloadSize || n > static_cast<int>(sizeof(aPayload)))
    {
        return;
    }

    __try
    {
        WriteRaw(pSocket, aPayload, static_cast<unsigned long>(n));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Reads the packet with the CLIENT'S OWN readers (Decode1/2/4). The cursor is saved and put back, so
// when this is not our packet the client's own ProcessPacket still sees the opcode where it expects
// it. C++ EH rather than SEH: those readers throw a ZException on a short packet, and an escaping
// C++ exception would unwind into the client's frames (the SEH wrapper outside only has to catch a
// bad pointer). Returns true when the packet is ours -- the caller must then consume it (v83 has no
// dispatcher case for 0x1001), whether or not the payload was applied.
static bool ReadConfigPacketRaw(CInPacket* packet, int* pSlots, bool* pbApplied)
{
    *pbApplied = false;

    auto* pBase = reinterpret_cast<unsigned char*>(packet);
    int* pCursor = reinterpret_cast<int*>(pBase + OFF_CInPacket_Position);
    const int nSavedCursor = *pCursor;

    try
    {
        const int nOpcode = _decode2(packet, nullptr);
        if (nOpcode != nOpcodePetBuffConfig)
        {
            *pCursor = nSavedCursor;                       // not ours: hand it back untouched
            return false;
        }

        if (_decode1(packet, nullptr) != nConfigVersion)
        {
            return true;                                   // ours, but a version we cannot read
        }

        for (int i = 0; i < kValueCount; ++i)
        {
            pSlots[i] = static_cast<int>(_decode4(packet, nullptr));
        }

        *pbApplied = true;
        return true;
    }
    catch (...)
    {
        *pCursor = nSavedCursor;                           // short/garbled: leave it to the client
        return false;
    }
}

static bool TryConsumeConfigPacket(CInPacket* packet, bool* pbApplied)
{
    *pbApplied = false;
    if (packet == nullptr)
    {
        return false;
    }

    bool bMine = false;
    __try
    {
        int aSlots[kValueCount] = {};
        bMine = ReadConfigPacketRaw(packet, aSlots, pbApplied);
        if (*pbApplied)
        {
            for (int nPet = 0; nPet < kPetCount; ++nPet)
            {
                for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
                {
                    PetSkillSlot::SetSkill(nPet, nSlot, aSlots[SlotIndex(nPet, nSlot)]);
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bMine = false;                                     // unreadable packet: client's business
        *pbApplied = false;
    }

    return bMine;
}

static ProcessPacket_t g_origProcessPacket = nullptr;

static void __fastcall ProcessPacket_Hook(void* pThis, void* edx, CInPacket* packet)
{
    bool bConsumed = false;
    bool bApplied = false;

    __try
    {
        bConsumed = TryConsumeConfigPacket(packet, &bApplied);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        bConsumed = false;                 // unreadable header: let the client deal with it as before
    }

    if (bConsumed)
    {
        return;
    }

    if (g_origProcessPacket != nullptr)
    {
        g_origProcessPacket(pThis, edx, packet);
    }
}

void PetBuffConfig::Hook()
{
    Hook_PetBuffConfig(true);
}

void Hook_PetBuffConfig(bool enable)
{
    if (!enable)
    {
        std::cout << "pet auto-buff config sync disabled" << std::endl;
        return;
    }

    if (g_origProcessPacket == nullptr)
    {
        g_origProcessPacket = reinterpret_cast<ProcessPacket_t>(ADDR_ClientSocket_ProcessPacket);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origProcessPacket),
            reinterpret_cast<void*>(&ProcessPacket_Hook)))
        {
            g_origProcessPacket = nullptr;
            std::cout << "pet auto-buff config sync: ProcessPacket hook FAILED" << std::endl;
        }
    }
}

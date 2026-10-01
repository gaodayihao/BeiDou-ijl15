#include "stdafx.h"
#include "PetBuffConfig.h"
#include "PetSkillSlot.h"
#include "Memory.h"
#include <stdio.h>
#include <stdarg.h>

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

static const int nOpcodePetBuffConfig = 0x1001;
static const unsigned char nConfigVersion = 1;
static const int kPetCount = 3;
static const int kSlotCount = 2;
static const int kValueCount = kPetCount * kSlotCount;
static const int nPayloadSize = 1 + 4 * kValueCount;            // version + six int32
static const int nReceivePrefix = 6;                            // bytes before the payload (see above)

bool PetBuffConfig::bDebug = true;

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

// The receive view, with the field offsets the client itself uses -- read off CInPacket::Decode2
// (0x0042470C: base at +0x08, cursor at +0x14, length at +0x18) and the copy constructor
// CClientSocket::ManipulatePacket calls (0x006EC39F: `memcpy(newBuf, src + 0x08, src + 0x18)`).
// Do NOT copy HpMpAlert.cpp's declaration here: it puts Data at +0x04, which is a flag (2 when the
// packet copy is built), so every read goes to address 6 -- an access violation that its own __try
// swallows, leaving that module's receive path silently dead.
//
// The buffer keeps the wire framing AppendBuffer consumed: RawSeq(2) | DataLen(2) | opcode(2) |
// payload, so the first Decode2 in ProcessPacket reads the opcode at Data + 4 (cursor starts at 4)
// and the payload starts at Data + 6. Size = 4 + DataLen (the whole buffer, header included).
struct CInPacket
{
    void* Vtbl;                     // +0x00
    int Flags;                      // +0x04
    unsigned char* Data;            // +0x08
    int Unk0C;                      // +0x0C
    int Unk10;                      // +0x10
    int Position;                   // +0x14
    int Size;                       // +0x18
};

typedef void(__fastcall* SendPacket_t)(void* pSocket, void* edx, void* pPacket);
typedef void(__fastcall* ProcessPacket_t)(void* pThis, void* edx, CInPacket* pPacket);

static auto _send_packet = reinterpret_cast<SendPacket_t>(ADDR_ClientSocket_SendPacket);

static void LogLine(const char* sFormat, ...)
{
    if (!PetBuffConfig::bDebug)
    {
        return;
    }

    FILE* pFile = nullptr;
    if (fopen_s(&pFile, "petbuff.log", "a") != 0 || pFile == nullptr)
    {
        return;
    }

    va_list args;
    va_start(args, sFormat);
    vfprintf(pFile, sFormat, args);
    va_end(args);
    fputc('\n', pFile);
    fclose(pFile);
}

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
        LogLine("  config: sent %d/%d/%d/%d/%d/%d",
            PetSkillSlot::GetSkill(0, 0), PetSkillSlot::GetSkill(0, 1),
            PetSkillSlot::GetSkill(1, 0), PetSkillSlot::GetSkill(1, 1),
            PetSkillSlot::GetSkill(2, 0), PetSkillSlot::GetSkill(2, 1));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogLine("  config: send faulted, ignored");
    }
}

// True when this packet is the configuration push. Reads nothing but the buffer header, and on true
// also applies it: the caller must NOT forward such a packet (v83 has no dispatcher case for 0x1001).
static bool TryConsumeConfigPacket(CInPacket* packet)
{
    if (packet == nullptr || packet->Data == nullptr)
    {
        return false;
    }

    if (packet->Size < nReceivePrefix + nPayloadSize)
    {
        return false;
    }

    const unsigned char* data = packet->Data;
    const unsigned short opcode = static_cast<unsigned short>(data[4] | (data[5] << 8));
    if (opcode >= 0x1000)
    {
        // Every opcode in this range is a candidate for "what the server just sent us"; the raw dump
        // is what pinned the field offsets down, so keep it (rare: 0x1000 / 0x1001 / 0x3713 / 0xFFFE).
        LogLine("  recv: op=0x%04X size=%d payload0=%d", opcode, packet->Size, data[6]);
    }

    if (opcode != nOpcodePetBuffConfig)
    {
        return false;
    }

    if (data[6] != nConfigVersion)
    {
        LogLine("  config: version %d unknown, ignored", static_cast<int>(data[6]));
        return true;                       // ours, but a version we cannot read: still consumed
    }

    for (int nPet = 0; nPet < kPetCount; ++nPet)
    {
        for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
        {
            int nSkillId = 0;
            memcpy(&nSkillId, data + nReceivePrefix + 1 + 4 * SlotIndex(nPet, nSlot), sizeof(int));
            PetSkillSlot::SetSkill(nPet, nSlot, nSkillId);
        }
    }

    LogLine("  config: loaded %d/%d/%d/%d/%d/%d",
        PetSkillSlot::GetSkill(0, 0), PetSkillSlot::GetSkill(0, 1),
        PetSkillSlot::GetSkill(1, 0), PetSkillSlot::GetSkill(1, 1),
        PetSkillSlot::GetSkill(2, 0), PetSkillSlot::GetSkill(2, 1));
    return true;
}

static ProcessPacket_t g_origProcessPacket = nullptr;

static void __fastcall ProcessPacket_Hook(void* pThis, void* edx, CInPacket* packet)
{
    bool bConsumed = false;

    __try
    {
        bConsumed = TryConsumeConfigPacket(packet);
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

    LogLine("--- pet auto-buff config sync armed (opcode 0x%04X) ---", nOpcodePetBuffConfig);
}

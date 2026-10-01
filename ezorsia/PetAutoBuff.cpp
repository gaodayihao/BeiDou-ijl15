#include "stdafx.h"
#include "PetAutoBuff.h"
#include "PetSkillSlot.h"
#include "PetBuffWhitelist.h"
#include "Memory.h"
#include <stdio.h>
#include <stdarg.h>

// ===== Reverse-engineering anchors (Angel.exe / BeiDou.exe, v83; bookmarks prefixed "PETBUFF:") ====
//
// --- what a buff looks like in the client -----------------------------------
//   Everything this module needs is already in the client's memory, in the temporary-stat list, so no
//   packet is read at all. CWvsContext + 0x2EA8 is the view, +0x04 its ZList, +0x0C its count, each
//   node's entry at node+4 with:
//       +0x1C  nType         2 == a skill buff
//       +0x20  nId           the skill id (item buffs carry a negated id)
//       +0x28  the icon layer, +0x2C the overlay layer
//       +0x38  the remaining time in ms, live
//       +0x3C  the blink threshold (default 3000, overridden for skill 5221006)
//   +0x38 and +0x3C are the fields CTemporaryStatView's per-entry Update (sub_7B4819) uses for the
//   3-second blink -- `v3 = *(this+14); *(this+14) = remaining; if (remaining <= 3000) blink` -- which
//   is proof the client counts the buff down itself rather than relying on the server telling it when
//   the buff ends. Reading that countdown is why this module never has to parse GIVE_BUFF.
//
//   An earlier version did parse it (to get the duration), which meant touching a CInPacket the client
//   had not consumed yet; missing the cursor restore made the client's own read run off the end and
//   the game failed with "error code: 38" on entering. The memory field removes that whole class of
//   risk, so the packet hook is gone.
//
// --- the gates that do NOT need the cast path --------------------------------
//   CUserLocal::DoActiveSkill (0x00966F7A) refuses a cast long before it builds the packet. Its
//   prologue, in the client's own order:
//       if (field && CField::IsSkillForbiden(field, skillId)) { chat log; give up; }   // 0x967006
//       *(user + 0x3228) && sub_766986(skillId)?  /  DarkSight  /  a few skill ids  -> give up
//       *(user + 0x31AC)  /  *(user + 0x1F4C) > 0 (ride vehicle)  /  IsImmovable()  /
//       IsAttract() && !is_heros_will_skill(skillId)  /  *(user + 0x2AE8) (prepared skill)  -> give up
//   The map gate and the player's own dead marker are the two that are reachable without a
//   CUserLocal instance, so they are the two this module reproduces (see Tick).
//
//   CField::IsSkillForbiden (0x00537C7F) is a thunk, and its CField* argument is load-bearing:
//       push [esp+4]      ; skillId
//       add  ecx, 770h    ; this = CField + 0x770  <-- the map's Field::SkillInfo is a member of CField
//       call sub_587F5F   ; Field::SkillInfo::IsSkillForbiden(skillInfo, skillId)
//   so it needs a real field (a null field faults on +0x770) -- which is exactly why DoActiveSkill
//   wraps its call in `if (field)`. Hex-Rays drops the `add ecx, 770h` and renders the thunk as
//   `IsSkillForbiden(skillId)`, i.e. as a predicate that takes nothing but the skill id; reading the
//   push/`add ecx` at the call site is what keeps that from being taken at face value.
//
//   The dead marker is the client's own: CUserLocal::OnSetDead -> CWvsContext::UI_OpenRevive
//   (0x00A066F3) stamps get_update_time() into CWvsContext + 0x3520, and UI_CloseRevive (0x00A0670C)
//   clears it (called from CUIRevive::Revive, CUserLocal::OnRevive and CWvsContext::OnEnterField).
//   Non-zero therefore means "dead, revive dialog pending" -- one dword read, no call.
//
// --- the cast ---------------------------------------------------------------
//   CUserLocal::SendSkillUseRequest (0x0096D399) composes the packet:
//       COutPacket(91)                                 0x5B
//       Encode4(get_update_time())                     0x00987257 -- NOT GetTickCount
//       Encode4(skillId)
//       Encode1(level)
//       Encode2(tDelay)                                0 for a buff
//       CClientSocket::SendPacket(*(0x00BE7914), &pkt)
//       then *(user + 8356) = 1; *(user + 8360) = get_update_time();   <-- SKIPPED on purpose
//   The two trailing stores are the local cast-wait state; leaving them alone is what keeps the
//   cast silent. The server reads only the leading fields.
//
//   The packet is handed over as a filled-in COutPacket the way HpMpAlert.cpp does it, so no
//   COutPacket constructor / destructor has to be replicated.
//
// =================================================================================================

static const DWORD ADDR_CWvsContext_Update = 0x00A03350;
static const DWORD ADDR_CWvsContext_Instance = 0x00BE7918;
static const DWORD ADDR_ZList_FindIndex = 0x007B4D1D;
static const DWORD ADDR_GetUpdateTime = 0x00987257;
static const DWORD ADDR_ClientSocket = 0x00BE7914;
static const DWORD ADDR_ClientSocket_SendPacket = 0x0049637B;
static const DWORD ADDR_CSkillInfo_GetSkillLevel = 0x007616F6;
static const DWORD ADDR_CSkillInfo_CheckConsumeForActiveSkill = 0x00764256;
static const DWORD ADDR_CWvsContext_GetCharacterData = 0x00425D0B;
static const DWORD ADDR_SkillInfoInstance = 0x00BE78DC;
static const DWORD ADDR_get_field = 0x00437A0C;
static const DWORD ADDR_CField_IsSkillForbiden = 0x00537C7F;
// The local player: TSingleton<CUserLocal>::ms_pInstance (CField::Init null-checks the same global
// before finishing the field setup). Its pet array is three 8-byte ZRef<CPet> slots, the pointer in
// the second dword of each -- the layout CUserLocal::TryConsumePetHP walks (`*(this + 1975) + 8 * i
// + 4`, 0x0095B9A4) and the reason a removed pet stops being enumerated.
static const DWORD ADDR_CUserLocal_Instance = 0x00BEBF98;
static const int OFF_CUserLocal_Pets = 0x1EDC;
static const int nPetSlotStride = 8;
static const int OFF_PetSlot_Pet = 4;

// The two status objects the consume check is handed. What they are is settled by how the callee reads
// them (0x00764256): the first at +96/+104 (maxHP, the 1311006 half-HP gate) => BasicStat; the second
// at +219/+221, +417/+419, +453/+455, +267/+269 => SecondaryStat. CUserLocal::DoActiveSkill captures
// them at its top (`lea eax, [esi+20BCh]` / `add esi, 2134h`, esi = CWvsContext) and passes them
// straight through, so these are the client's own arguments, not a reconstruction -- and
// CWvsContext::OnRevive clears the same two addresses (SecondaryStat::Clear(this + 8500)).
static const int OFF_CWvsContext_ConsumeCtxA = 0x20BC;
static const int OFF_CWvsContext_ConsumeCtxB = 0x2134;

static const int OFF_CWvsContext_TemporaryStatView = 0x2EA8;
// The local player's death stamp: written by UI_OpenRevive, cleared by UI_CloseRevive.
static const int OFF_CWvsContext_ReviveStamp = 0x3520;
static const int OFF_View_ListBase = 0x04;
static const int OFF_View_ListSize = 0x0C;
static const int OFF_Node_Entry = 0x04;
static const int OFF_TempStat_Type = 0x1C;
static const int OFF_TempStat_Id = 0x20;
// The entry's live countdown in ms, and the threshold it blinks under. Both are the client's own:
// CTemporaryStatView's per-entry Update (sub_7B4819) stores the remaining time at +0x38 and compares
// it against 3000 (or +0x3C for skill 5221006) to start the 3-second blink.
static const int OFF_TempStat_Remaining = 0x38;
static const int nTempStatType_Skill = 2;

static const int nOpcodeSkillUse = 0x5B;
static const int kMaxSlots = 3 * 2; // three pet tabs, two cells each

bool PetAutoBuff::bEnabled = true;
int PetAutoBuff::nLeadMs = 3000;
int PetAutoBuff::nTickMs = 1000;
bool PetAutoBuff::bDebug = true;

typedef void (__fastcall* WvsContextUpdate_t)(void* pThis, void* edx);
typedef void* (__fastcall* ZListFindIndex_t)(void* pList, void* edx, unsigned int nIndex);
typedef int(__fastcall* GetUpdateTime_t)(void);
typedef long(__thiscall* SkillInfoGetSkillLevel_t)(void* pSkillInfo, const void* pCharacterData,
    long nSkillId, void** ppEntry);
typedef int(__thiscall* SkillInfoCheckConsume_t)(void* pSkillInfo, void* pCharacterData,
    void* pCtxA, void* pCtxB, int nSkillId);
typedef void(__fastcall* SendPacket_t)(void* pSocket, void* edx, void* pPacket);
typedef void* (__cdecl* GetField_t)(void);
typedef int(__thiscall* IsSkillForbiden_t)(void* pField, int nSkillId);

static auto _zlist_find_index = reinterpret_cast<ZListFindIndex_t>(ADDR_ZList_FindIndex);
static auto _get_update_time = reinterpret_cast<GetUpdateTime_t>(ADDR_GetUpdateTime);
static auto _get_skill_level = reinterpret_cast<SkillInfoGetSkillLevel_t>(ADDR_CSkillInfo_GetSkillLevel);
static auto _check_consume = reinterpret_cast<SkillInfoCheckConsume_t>(ADDR_CSkillInfo_CheckConsumeForActiveSkill);
static auto _send_packet = reinterpret_cast<SendPacket_t>(ADDR_ClientSocket_SendPacket);
static auto _get_field = reinterpret_cast<GetField_t>(ADDR_get_field);
static auto _is_skill_forbiden = reinterpret_cast<IsSkillForbiden_t>(ADDR_CField_IsSkillForbiden);

// The client's COutPacket, laid out the way HpMpAlert.cpp declares it: Data/Size describe a buffer
// the caller owns, so nothing here has to allocate or free.
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

// Per cell: the earliest time the next attempt may be made, so a rejected cast (out of MP, forbidden
// map, dead, server refusal) is retried on a slow clock instead of every tick.
static unsigned int g_adwRetryAfter[kMaxSlots];
static unsigned int g_dwLastTick = 0;
static const unsigned int kRetryMs = 2000;

// How many pets the local character currently has out (0..3), by walking the client's own pet array
// and stopping at the first empty slot -- the same "pets are contiguous from index 0" assumption
// CUserLocal::TryConsumePetHP makes. A tab without a pet has nothing to refresh, so its cells are
// skipped: unequipping a pet must stop its pair, and the stored configuration is left alone so it
// comes back when the pet does.
static int GetActivePetCount()
{
    __try
    {
        char* pUser = *reinterpret_cast<char**>(ADDR_CUserLocal_Instance);
        if (pUser == nullptr)
        {
            return 0;                       // not in a field: CUserLocal exists only in game
        }

        char* pPets = *reinterpret_cast<char**>(pUser + OFF_CUserLocal_Pets);
        if (pPets == nullptr)
        {
            return 0;
        }

        int nCount = 0;
        for (int i = 0; i < 3; ++i)
        {
            if (*reinterpret_cast<void**>(pPets + nPetSlotStride * i + OFF_PetSlot_Pet) == nullptr)
            {
                break;
            }
            ++nCount;
        }
        return nCount;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static void LogLine(const char* sFormat, ...)
{
    if (!PetAutoBuff::bDebug)
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

// True while the client's own temporary-stat list holds a skill buff with this id, and the time it
// has left. This is the whole state source: the client keeps both. The remaining time is the entry's
// own countdown field -- the one its blink-at-3-seconds logic reads (CTemporaryStatView's per-entry
// Update, sub_7B4819: `v3 = *(this+14); *(this+14) = remaining; ... if (remaining <= 3000) blink`),
// so it is live, not something this module has to reconstruct from GIVE_BUFF.
static bool IsBuffUp(void* pWvsContext, int nSkillId, int* pnRemainingMs)
{
    if (pnRemainingMs != nullptr)
    {
        *pnRemainingMs = -1;
    }

    if (pWvsContext == nullptr || nSkillId <= 0)
    {
        return false;
    }

    __try
    {
        char* pView = reinterpret_cast<char*>(pWvsContext) + OFF_CWvsContext_TemporaryStatView;
        if (IsBadReadPtr(pView, OFF_View_ListSize + sizeof(int)))
        {
            return false;
        }

        int nCount = *reinterpret_cast<int*>(pView + OFF_View_ListSize);
        if (nCount > 64)
        {
            nCount = 64;
        }

        void* pList = pView + OFF_View_ListBase;
        for (int i = 0; i < nCount; ++i)
        {
            void* pNode = _zlist_find_index(pList, nullptr, static_cast<unsigned int>(i));
            if (pNode == nullptr || IsBadReadPtr(pNode, 8))
            {
                continue;
            }

            int* pEntry = *reinterpret_cast<int**>(reinterpret_cast<char*>(pNode) + OFF_Node_Entry);
            if (pEntry == nullptr || IsBadReadPtr(pEntry, OFF_TempStat_Remaining + sizeof(int)))
            {
                continue;
            }

            if (*reinterpret_cast<int*>(reinterpret_cast<char*>(pEntry) + OFF_TempStat_Type) != nTempStatType_Skill)
            {
                continue;
            }

            const int nId = *reinterpret_cast<int*>(reinterpret_cast<char*>(pEntry) + OFF_TempStat_Id);
            if (nId != nSkillId)
            {
                continue;
            }

            if (pnRemainingMs != nullptr)
            {
                *pnRemainingMs = *reinterpret_cast<int*>(reinterpret_cast<char*>(pEntry) + OFF_TempStat_Remaining);
            }

            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return false;
}

// CWvsContext::GetCharacterData returns a ZRef; its payload pointer is the second dword.
static void* GetCharacterDataRaw(void* pWvsContext)
{
    try
    {
        unsigned char aRef[16];
        memset(aRef, 0, sizeof(aRef));
        typedef void(__thiscall* GetCharacterData_t)(void* pThis, void* pRetBuf);
        reinterpret_cast<GetCharacterData_t>(ADDR_CWvsContext_GetCharacterData)(pWvsContext, aRef);
        return *reinterpret_cast<void**>(aRef + 4);
    }
    catch (...)
    {
        return nullptr;
    }
}

static void* GetCharacterData(void* pWvsContext)
{
    __try
    {
        return GetCharacterDataRaw(pWvsContext);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// The client's own gate for an active skill: HP/MP cost, the meso/item it consumes and the cooldown,
// all in one call. It also performs the client's optimistic HP/MP pre-deduction -- which is exactly
// what CUserLocal::DoActiveSkill does before it sends the packet, with the server settling
// authoritatively afterwards -- so calling it here is the same accounting the client does, not a
// second deduction.
//
// Return codes, taken from the branches themselves (0x00764256): 1 = may cast, and the HP/MP were just
// deducted; 2 = HP too low (this is also where 1311006's maxHP/2 gate answers); 3 = MP too low; 4 = not
// enough meso; any other non-zero value = the id of the item the skill consumes, which the character
// lacks; 0 = not castable at all (unknown skill, skill level 0, or a passive-type skill id).
static int CheckConsumeRaw(void* pWvsContext, void* pCharacterData, int nSkillId)
{
    try
    {
        void* pInfo = *reinterpret_cast<void**>(ADDR_SkillInfoInstance);
        if (pInfo == nullptr)
        {
            return 0;
        }

        return _check_consume(pInfo, pCharacterData,
            reinterpret_cast<char*>(pWvsContext) + OFF_CWvsContext_ConsumeCtxA,
            reinterpret_cast<char*>(pWvsContext) + OFF_CWvsContext_ConsumeCtxB,
            nSkillId);
    }
    catch (...)
    {
        LogLine("  autobuff: consume check raised, skipped");
        return 0;
    }
}

static int CheckConsume(void* pWvsContext, void* pCharacterData, int nSkillId)
{
    __try
    {
        return CheckConsumeRaw(pWvsContext, pCharacterData, nSkillId);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// The skill's current level, or 0 when the character does not have it. The server checks the level
// too, so a wrong value only gets the cast refused.
//
// Same SEH / C++-EH split as SendSkillUse: CWvsContext::GetCharacterData and CSkillInfo::GetSkillLevel
// both hand back COM-flavoured values and raise _com_error on failure, and an escaping exception
// takes the client down silently.
static int GetSkillLevelRaw(void* pSkillInfo, void* pCharacterData, int nSkillId)
{
    try
    {
        // A null ppEntry: the level is all this module needs, and that is how the client's own
        // SendSkillUseRequest asks for it.
        const long nLevel = _get_skill_level(pSkillInfo, pCharacterData, nSkillId, nullptr);
        return nLevel > 0 ? static_cast<int>(nLevel) : 0;
    }
    catch (...)
    {
        LogLine("  autobuff: level lookup raised, ignored");
        return 0;
    }
}

static int GetSkillLevel(void* pCharacterData, int nSkillId)
{
    if (pCharacterData == nullptr)
    {
        return 0;
    }

    __try
    {
        void* pInfo = *reinterpret_cast<void**>(ADDR_SkillInfoInstance);
        if (pInfo == nullptr)
        {
            return 0;
        }

        return GetSkillLevelRaw(pInfo, pCharacterData, nSkillId);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// The client's own packet, byte for byte: opcode, timestamp, skill id, level, tDelay. The cast-wait
// stores SendSkillUseRequest makes afterwards are deliberately not reproduced.
//
// Split into a C++-EH half and an SEH half on purpose: the client's COM-flavoured wrappers raise
// _com_error, and an exception that escapes a hook unwinds into the client's own frames, which has no
// handler for it and takes the process down without even a crash dump. SEH alone does not catch it
// and C++ EH alone does not catch a bad pointer, so the two live in separate functions -- the same
// split BuffTimer documents.
static void SendSkillUseRaw(void* pSocket, const unsigned char* pPayload, unsigned long nSize)
{
    COutPacket packet;
    packet.Loopback = 0;
    packet.Data = const_cast<unsigned char*>(pPayload);
    packet.Size = nSize;
    packet.Offset = 0;
    packet.EncryptedByShanda = 0;

    try
    {
        _send_packet(pSocket, nullptr, &packet);
    }
    catch (...)
    {
        LogLine("  autobuff: SendPacket raised, ignored");
    }
}

static void SendSkillUse(int nSkillId, int nLevel)
{
    void* pSocket = *reinterpret_cast<void**>(ADDR_ClientSocket);
    if (pSocket == nullptr || nLevel <= 0 || nLevel > 255)
    {
        return;
    }

    // opcode(2) + timestamp(4) + skillId(4) + level(1) + tDelay(2) = 13 bytes. The buffer is sized
    // with room to spare on purpose: writing one byte past a 12-byte array here tripped the /GS stack
    // cookie, and __report_gsfailure ends the process through __fastfail -- no crash dump, no catchable
    // exception, just an instant exit.
    unsigned char aPayload[16];
    int n = 0;
    aPayload[n++] = static_cast<unsigned char>(nOpcodeSkillUse & 0xFF);
    aPayload[n++] = static_cast<unsigned char>((nOpcodeSkillUse >> 8) & 0xFF);

    const unsigned int dwTime = static_cast<unsigned int>(_get_update_time());
    memcpy(aPayload + n, &dwTime, 4);
    n += 4;

    memcpy(aPayload + n, &nSkillId, 4);
    n += 4;

    aPayload[n++] = static_cast<unsigned char>(nLevel);
    aPayload[n++] = 0; // tDelay low
    aPayload[n++] = 0; // tDelay high

    if (n > static_cast<int>(sizeof(aPayload)))
    {
        return;
    }

    __try
    {
        SendSkillUseRaw(pSocket, aPayload, static_cast<unsigned long>(n));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogLine("  autobuff: SendPacket faulted, ignored");
    }
}

// The client's own "not now" state for a cast, restricted to the two gates that need no CUserLocal
// instance (see the anchors at the top): the local death stamp and the map pointer. Returns the field
// used by the per-skill map gate, or null to skip the whole tick, with the reason for the log.
static void* GetFieldOrVeto(void* pWvsContext, const char** ppszReason)
{
    *ppszReason = nullptr;

    // Non-zero = dead with the revive dialog pending. The server refuses a dead caster anyway
    // (SpecialMoveHandler checks IsAlive); this only keeps the packets home.
    if (*reinterpret_cast<int*>(reinterpret_cast<char*>(pWvsContext) + OFF_CWvsContext_ReviveStamp) != 0)
    {
        *ppszReason = "dead";
        return nullptr;
    }

    // 0 in the cash shop, at login and while a field transfer is in flight -- the client has no skill
    // context there either, and this is the pointer the map gate has to be handed.
    void* pField = _get_field();
    if (pField == nullptr)
    {
        *ppszReason = "not in a map";
    }

    return pField;
}

// Walks the six cells and re-casts whatever is missing. Runs from CWvsContext::Update (every frame),
// so the whole body is throttled to PetAutoBuff::nTickMs; a cell that was just attempted is held off
// for kRetryMs on top of that, which is what bounds the actual packets.
static void Tick(void* pWvsContext)
{
    if (!PetAutoBuff::bEnabled || pWvsContext == nullptr)
    {
        return;
    }

    const unsigned int dwNow = GetTickCount();
    const int nConfiguredTick = PetAutoBuff::nTickMs;
    const unsigned int dwTickMs = static_cast<unsigned int>(
        nConfiguredTick < 100 ? 100 : (nConfiguredTick > 5000 ? 5000 : nConfiguredTick));
    if (dwNow - g_dwLastTick < dwTickMs)
    {
        return;
    }
    g_dwLastTick = dwNow;

    static bool s_bVetoLogged = false;
    const char* pszVeto = nullptr;
    void* pField = nullptr;
    __try
    {
        pField = GetFieldOrVeto(pWvsContext, &pszVeto);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        pszVeto = "gate read faulted";
    }

    if (pField == nullptr)
    {
        // Logged on the edge only: the veto is re-evaluated every kTickMs, and a player sitting dead in
        // front of the revive dialog would otherwise write five lines a second.
        if (!s_bVetoLogged)
        {
            LogLine("  autobuff: holding off (%s)", pszVeto);
            s_bVetoLogged = true;
        }
        return;
    }
    s_bVetoLogged = false;

    // Pets currently out. Tabs without one are skipped: a removed pet must stop refreshing its pair,
    // and the configured values stay put for when it comes back. Logged on the edge only.
    static int s_nLastPetCount = -1;
    const int nPetCount = GetActivePetCount();
    if (nPetCount != s_nLastPetCount)
    {
        LogLine("  autobuff: %d pet(s) out, tabs 0..%d active", nPetCount, nPetCount - 1);
        s_nLastPetCount = nPetCount;
    }

    for (int nPet = 0; nPet < nPetCount; ++nPet)
    {
        for (int nSlot = 0; nSlot < 2; ++nSlot)
        {
            const int nSkillId = PetSkillSlot::GetSkill(nPet, nSlot);
            if (nSkillId == 0 || !IsPetBuffSkill(nSkillId))
            {
                continue;
            }

            const int nCell = nPet * 2 + nSlot;
            if (dwNow < g_adwRetryAfter[nCell])
            {
                continue;
            }

            // Both facts come from the client's own temporary-stat entry: whether the buff is on and
            // how long it has left (the countdown its 3-second blink reads). Nothing is reconstructed.
            int nRemaining = -1;
            const bool bUp = IsBuffUp(pWvsContext, nSkillId, &nRemaining);
            if (bUp && nRemaining > PetAutoBuff::nLeadMs)
            {
                continue;
            }

            // The map gate, the first thing CUserLocal::DoActiveSkill checks and the one a hand-rolled
            // packet would otherwise walk straight past. The field pointer is dereferenced by the thunk
            // (CField + 0x770), so it has to come from get_field() -- hence the veto above.
            if (_is_skill_forbiden(pField, nSkillId) != 0)
            {
                g_adwRetryAfter[nCell] = dwNow + kRetryMs;
                LogLine("  autobuff pet=%d slot=%d skill=%d: forbidden on this map", nPet, nSlot, nSkillId);
                continue;
            }

            void* pCharacterData = GetCharacterData(pWvsContext);
            const int nLevel = GetSkillLevel(pCharacterData, nSkillId);
            if (nLevel <= 0)
            {
                g_adwRetryAfter[nCell] = dwNow + kRetryMs;
                LogLine("  autobuff pet=%d slot=%d skill=%d: not learned, skipped", nPet, nSlot, nSkillId);
                continue;
            }

            // The client's own gate, called the way CUserLocal::DoActiveSkill calls it: cooldown, MP,
            // HP cost and the required item in one shot, plus the optimistic HP/MP pre-deduction that
            // pairs with the packet. Anything but 1 means the cast must not happen.
            const int nConsume = CheckConsume(pWvsContext, pCharacterData, nSkillId);
            if (nConsume != 1)
            {
                g_adwRetryAfter[nCell] = dwNow + kRetryMs;
                LogLine("  autobuff pet=%d slot=%d skill=%d: consume refused (%d; 2=HP 3=MP 4=meso, other=missing item id)",
                    nPet, nSlot, nSkillId, nConsume);
                continue;
            }

            SendSkillUse(nSkillId, nLevel);
            g_adwRetryAfter[nCell] = dwNow + kRetryMs;
            LogLine("  autobuff pet=%d slot=%d skill=%d level=%d remaining=%d up=%d: cast sent",
                nPet, nSlot, nSkillId, nLevel, nRemaining, bUp ? 1 : 0);
        }
    }
}

static WvsContextUpdate_t g_origUpdate = nullptr;

static void __fastcall Update_Hook(void* pThis, void* edx)
{
    if (g_origUpdate != nullptr)
    {
        g_origUpdate(pThis, edx);
    }

    __try
    {
        Tick(pThis);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

void PetAutoBuff::Hook()
{
    Hook_PetAutoBuff(bEnabled);
}

void Hook_PetAutoBuff(bool enable)
{
    if (!enable)
    {
        std::cout << "pet auto-buff refresh disabled by config (petAutoBuff=false)" << std::endl;
        return;
    }

    if (g_origUpdate == nullptr)
    {
        g_origUpdate = reinterpret_cast<WvsContextUpdate_t>(ADDR_CWvsContext_Update);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origUpdate),
            reinterpret_cast<void*>(&Update_Hook)))
        {
            g_origUpdate = nullptr;
            std::cout << "pet auto-buff refresh: Update hook FAILED" << std::endl;
        }
    }

    LogLine("--- pet auto-buff refresh armed (lead=%dms) ---", PetAutoBuff::nLeadMs);
}

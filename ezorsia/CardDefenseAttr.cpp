#include "stdafx.h"
#include "CardDefenseAttr.h"
#include "Memory.h"
#include <cstdarg>
#include <cstdio>

// ===== Reverse-engineering anchors (BeiDou.exe v83; IDA bookmarks "BUG029:") ========================
//
// --- the client's own rule, and why the two buffs used to kill each other -------------------------
//   CTemporaryStatView::SetTemporary (0x007B24D5) walks its entries and, for every entry that is not
//   nType 3/4, runs `entry->mask &= ~newMask` (UINT128::operator&= sub_873FE6 at 0x00873FE6) and
//   removes the entry when that left the mask empty (sub_873B69 at 0x00873B69 reads "is non-zero";
//   the removal itself is sub_7B4BD1 at 0x007B4BD1). A monster card and a rate coupon land on the same
//   mask bit (DEFENSE_ATT == COUPON_DRP1 == 0x800000, plus four more pairs), so the later packet
//   emptied the earlier entry's mask and its icon vanished.
//   The same removal rule serves the cancel path: CWvsContext::OnTemporaryStatReset (0x00A2071F) calls
//   CTemporaryStatView::ResetTemporary (0x007B2717) with the view at CWvsContext + 0x2EA8, and that
//   function repeats `entry->mask &= ~resetMask` + drop-when-empty.
//   Bit 0 of the mask belongs to no BuffStat on either mask word (the smallest value in use is
//   MORPH = 0x2), so OR-ing it into an entry we want to keep makes the AND leave one bit behind.
//
// --- entry layout and list traversal --------------------------------------------------------------
//   Entry (ctor sub_7B3176): +0x00 vtbl, +0x0C mask (UINT128), +0x1C nType, +0x20 nId (this build
//   stores the *positive* item id for nType==1), +0x28/+0x2C the two layers, +0x38 remaining ms.
//   The list lives at view+0x04; ZList::FindIndex (0x007B4D1D) returns the *node*, the entry is the
//   pointer stored at node+4 -- the same pair the client's own loops use (`v19 = *(v9+4)` for the entry
//   data, `v10 = v9` for the node). Reading through that pair is safe; **writing the list is not**:
//
// --- what is deliberately NOT done here -----------------------------------------------------------
//   No list surgery at all. Calling the client's own remove (sub_7B4BD1) + relayout (sub_7B2BB0) to
//   delete a stale duplicate crashed the client within seconds (AV reading 0xFFFFFFFF -- the client
//   takes a reference on the entry and unlinks it inside its own critical section, which an outside
//   caller cannot reproduce); see Ursa-Server docs/bugs/031. The correct shape is to never create the
//   duplicate: parse the incoming GIVE_BUFF for its buff ids and inject the inert bit only into
//   entries whose id *differs* from the ones being sent, so the client replaces its own same-id entry
//   natively. Until that parse lands, this module only injects (and logs what it would have removed).
//   Also not done: "set remaining to 1 and let the countdown reap it" (measured: the local countdown
//   only drives blink/label, such entries stayed for minutes and went negative) and "keep whichever
//   copy has the largest remaining" (a re-sent buff carries the *remaining* duration, so copies can
//   compare equal).
static const DWORD ADDR_CWvsContext_Instance = 0x00BE7918;
static const DWORD ADDR_CWvsContext_OnTemporaryStatSet = 0x00A202BE;
static const DWORD ADDR_CWvsContext_OnTemporaryStatReset = 0x00A2071F;
static const DWORD ADDR_ZList_FindIndex = 0x007B4D1D;
// 0x007B4BD1 (sub_7B4BD1, `retn 4`, the client's own remove: `(list, node)`) and 0x007B2BB0
// (AdjustPosition) are deliberately NOT called any more: using them for list surgery made the client
// crash with an access violation within seconds (Ursa-Server docs/bugs/031). The way forward is to
// never create the duplicate -- parse the incoming GIVE_BUFF for its buff ids and protect only entries
// whose id differs (see the module header).

static const int OFF_CWvsContext_TemporaryStatView = 0x2EA8;
static const int OFF_View_ListBase = 0x04;
static const int OFF_View_ListSize = 0x0C;
static const int OFF_Node_Entry = 0x04;
static const int OFF_TempStat_Mask = 0x0C;
static const int OFF_TempStat_Type = 0x1C;
static const int OFF_TempStat_Id = 0x20;
static const int OFF_TempStat_Remaining = 0x38;

static const int nTempStatType_Item = 1;
static const int nMaxViewEntries = 64;

// Belongs to no BuffStat on either mask word, so it can never switch a stat on.
static const unsigned int kInertMaskBit = 0x00000001;

static const int nMaxLogLines = 40000;

struct Mask128
{
    unsigned int d[4];
};

struct ViewEntry
{
    void* pNode;
    char* pEntry;
    Mask128 mask;
};

bool CardDefenseAttr::bCoexist = true;

typedef void* (__fastcall* ZListFindIndex_t)(void* pList, void* edx, unsigned int nIndex);
typedef void(__fastcall* OnTemporaryStatSet_t)(void* pThis, void* edx, void* pPacket);
typedef void(__fastcall* OnTemporaryStatReset_t)(void* pThis, void* edx, void* pPacket);

static auto _zlist_find_index = reinterpret_cast<ZListFindIndex_t>(ADDR_ZList_FindIndex);
static OnTemporaryStatSet_t g_origOnTemporaryStatSet = nullptr;
static OnTemporaryStatReset_t g_origOnTemporaryStatReset = nullptr;

static FILE* g_pLog = nullptr;
static bool g_bLogOpened = false;
static int g_nLogLines = 0;

static void CDLog(const char* pFormat, ...)
{
    if (g_nLogLines >= nMaxLogLines)
    {
        return;
    }

    if (!g_bLogOpened)
    {
        g_bLogOpened = true;
        fopen_s(&g_pLog, "carddefense.log", "a");
    }

    if (g_pLog == nullptr)
    {
        return;
    }

    char szBody[512];
    va_list args;
    va_start(args, pFormat);
    _vsnprintf_s(szBody, sizeof(szBody), _TRUNCATE, pFormat, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_pLog, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, szBody);
    fflush(g_pLog);
    ++g_nLogLines;
}

static bool IsRateCouponItem(int nItemId)
{
    const int nType = nItemId / 1000;
    return nType == 5211 || nType == 5360;
}

static int AbsItemId(int nId)
{
    return nId < 0 ? -nId : nId;
}

// Item id of an item-buff entry, or 0 when the entry is not one. This build stores the positive item
// id for nType==1 (measured: `type=1 id=5360000`); both signs are accepted so the classification
// cannot hinge on the sign again.
static int GetItemBuffId(const char* pEntry)
{
    if (pEntry == nullptr || IsBadReadPtr(pEntry, OFF_TempStat_Remaining + sizeof(int)))
    {
        return 0;
    }

    if (*reinterpret_cast<const int*>(pEntry + OFF_TempStat_Type) != nTempStatType_Item)
    {
        return 0;
    }

    return AbsItemId(*reinterpret_cast<const int*>(pEntry + OFF_TempStat_Id));
}

static bool IsProtectedItem(int nItemId)
{
    return nItemId > 0 && (IsRateCouponItem(nItemId) || nItemId / 10000 == 238);
}

// Walks the temporary-stat view; the callback gets (node, entry) and returns false to stop.
template <typename TCallback>
static void ForEachViewEntry(void* pWvsContext, TCallback callback)
{
    char* pView = reinterpret_cast<char*>(pWvsContext) + OFF_CWvsContext_TemporaryStatView;
    if (IsBadReadPtr(pView, OFF_View_ListSize + sizeof(int)))
    {
        return;
    }

    int nCount = *reinterpret_cast<int*>(pView + OFF_View_ListSize);
    if (nCount > nMaxViewEntries)
    {
        nCount = nMaxViewEntries;
    }

    void* pList = pView + OFF_View_ListBase;
    for (int i = 0; i < nCount; ++i)
    {
        void* pNode = _zlist_find_index(pList, nullptr, static_cast<unsigned int>(i));
        if (pNode == nullptr || IsBadReadPtr(pNode, 8))
        {
            continue;
        }

        char* pEntry = *reinterpret_cast<char**>(reinterpret_cast<char*>(pNode) + OFF_Node_Entry);
        if (pEntry == nullptr || IsBadReadPtr(pEntry, OFF_TempStat_Remaining + sizeof(int)))
        {
            continue;
        }

        if (!callback(pNode, pEntry))
        {
            return;
        }
    }
}

// Reads the buff ids the incoming GIVE_BUFF is about to apply, so an entry can be told apart from a
// buff being re-sent (a re-sent buff must be allowed to replace its own old entry -- protecting it is
// what used to leave duplicates behind). Nothing is called and nothing is consumed: the 16-byte mask
// (CInPacket::DecodeBuffer(mask, 16)) is read for its set-bit count only, each set bit contributes
// `short value + int buffid + int duration` (the client's own GIVE_BUFF decoder reads exactly that:
// sub_781D0E -> Decode2/Decode4/Decode4, see Ursa-Server docs/bugs/030 §2), and the read cursor at
// CInPacket+0x14 is left exactly as found -- the same save/restore contract PetBuffConfig.cpp uses.
// Returns the number of ids written (0 when the packet cannot be read: caller falls back to the
// protect-everything behaviour, which is duplicate-prone but never touches the list).
static const int OFF_CInPacket_Data = 0x08;
static const int OFF_CInPacket_Position = 0x14;
static const int nMaxIncomingIds = 64;

static int ReadIncomingBuffIds(void* pPacket, int* pOut, int nMax)
{
    if (pPacket == nullptr || pOut == nullptr || IsBadReadPtr(pPacket, OFF_CInPacket_Position + sizeof(int)))
    {
        return 0;
    }

    const char* pBase = *reinterpret_cast<const char* const*>(reinterpret_cast<const char*>(pPacket) + OFF_CInPacket_Data);
    if (pBase == nullptr)
    {
        return 0;
    }

    int nIds = 0;
    __try
    {
        const int nPos = *reinterpret_cast<const int*>(reinterpret_cast<const char*>(pPacket) + OFF_CInPacket_Position);
        const unsigned char* pMask = reinterpret_cast<const unsigned char*>(pBase) + nPos;
        int nCursor = nPos + 16;                        // DecodeBuffer(mask, 16)

        for (int byte = 0; byte < 16; ++byte)
        {
            for (int bit = 0; bit < 8; ++bit)
            {
                if ((pMask[byte] & (1 << bit)) == 0)
                {
                    continue;
                }

                if (nIds >= nMax)
                {
                    return nIds == 0 ? 0 : nIds;        // more ids than we track: keep what we have
                }

                pOut[nIds++] = *reinterpret_cast<const int*>(pBase + nCursor + 2);   // value(short) + buffid(int)
                nCursor += 10;                                                     // + duration(int)
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    return nIds;
}

// True when the entry's item id is one of the ids the incoming packet applies.
static bool IsBeingResent(int nItemId, const int* pIncoming, int nIncoming)
{
    for (int i = 0; i < nIncoming; ++i)
    {
        const int nOther = pIncoming[i];
        if ((nOther < 0 ? -nOther : nOther) == nItemId)
        {
            return true;
        }
    }

    return false;
}

// Snapshot of every protected entry that the handler is NOT about to replace, taken before it runs.
static int SnapshotProtectedEntries(void* pWvsContext, ViewEntry* pOut, int nMax,
    const int* pIncoming, int nIncoming, bool bInjectInertBit)
{
    int nCount = 0;
    __try
    {
        ForEachViewEntry(pWvsContext, [&](void* pNode, char* pEntry) -> bool
        {
            const int nItemId = GetItemBuffId(pEntry);
            if (!IsProtectedItem(nItemId))
            {
                return true;
            }

            if (IsBeingResent(nItemId, pIncoming, nIncoming))
            {
                return true;                            // let the client replace its own entry natively
            }

            if (nCount >= nMax)
            {
                return false;
            }

            pOut[nCount].pNode = pNode;
            pOut[nCount].pEntry = pEntry;
            memcpy(pOut[nCount].mask.d, pEntry + OFF_TempStat_Mask, sizeof(Mask128));
            if (bInjectInertBit)
            {
                *reinterpret_cast<unsigned int*>(pEntry + OFF_TempStat_Mask) |= kInertMaskBit;
            }
            ++nCount;
            return true;
        });
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    return nCount;
}

static Mask128 ReadMask(const char* pEntry)
{
    Mask128 mask;
    memcpy(mask.d, pEntry + OFF_TempStat_Mask, sizeof(Mask128));
    return mask;
}

static bool MaskOverlaps(const Mask128& a, const Mask128& b)
{
    for (int i = 0; i < 4; ++i)
    {
        if ((a.d[i] & b.d[i]) != 0)
        {
            return true;
        }
    }

    return false;
}

static bool HasOverlappingRateCoupon(void* pWvsContext, const Mask128& mask)
{
    bool bFound = false;
    __try
    {
        ForEachViewEntry(pWvsContext, [&](void*, char* pEntry) -> bool
        {
            const int nItemId = GetItemBuffId(pEntry);
            if (nItemId == 0 || !IsRateCouponItem(nItemId))
            {
                return true;
            }

            if (MaskOverlaps(ReadMask(pEntry), mask))
            {
                bFound = true;
                return false;
            }

            return true;
        });
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return bFound;
}

// Reports (does NOT touch) entries a handler re-sent: an entry present in the snapshot whose (nType,id)
// also exists on a different node. Removal through the client's own list functions is abandoned --
// it crashed the client (docs/bugs/031) -- so the duplicate is only logged here and the design moves to
// "protect only entries whose id differs from the one being sent", which needs no list surgery at all.
static int ReportStaleDuplicates(void* pWvsContext, const ViewEntry* pSnapshot, int nSnapshot)
{
    if (pWvsContext == nullptr || nSnapshot <= 0)
    {
        return 0;
    }

    ViewEntry current[nMaxViewEntries];
    int nCurrent = 0;
    __try
    {
        ForEachViewEntry(pWvsContext, [&](void* pNode, char* pEntry) -> bool
        {
            if (nCurrent >= nMaxViewEntries)
            {
                return false;
            }

            current[nCurrent].pNode = pNode;
            current[nCurrent].pEntry = pEntry;
            current[nCurrent].mask = ReadMask(pEntry);
            ++nCurrent;
            return true;
        });
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }

    int nDropped = 0;
    int nLegacy = 0;
    for (int i = 0; i < nSnapshot; ++i)
    {
        const int nItemId = GetItemBuffId(pSnapshot[i].pEntry);
        if (nItemId == 0)
        {
            continue;
        }

        const int nType = *reinterpret_cast<const int*>(pSnapshot[i].pEntry + OFF_TempStat_Type);
        const int nId = *reinterpret_cast<const int*>(pSnapshot[i].pEntry + OFF_TempStat_Id);

        bool bResent = false;
        bool bLegacyDuplicate = false;
        for (int j = 0; j < nCurrent; ++j)
        {
            if (current[j].pNode == pSnapshot[i].pNode
                || *reinterpret_cast<const int*>(current[j].pEntry + OFF_TempStat_Type) != nType
                || *reinterpret_cast<const int*>(current[j].pEntry + OFF_TempStat_Id) != nId)
            {
                continue;
            }

            // A node that was not in the snapshot is the copy this call created.
            bool bInSnapshot = false;
            for (int k = 0; k < nSnapshot && !bInSnapshot; ++k)
            {
                bInSnapshot = pSnapshot[k].pNode == current[j].pNode;
            }

            if (!bInSnapshot)
            {
                bResent = true;
                break;
            }

            bLegacyDuplicate = true;
        }

        if (!bResent && bLegacyDuplicate)
        {
            // Keep the copy with the largest remaining time among a stale group.
            const int nRemaining = *reinterpret_cast<const int*>(pSnapshot[i].pEntry + OFF_TempStat_Remaining);
            for (int j = 0; j < nSnapshot; ++j)
            {
                if (j == i || GetItemBuffId(pSnapshot[j].pEntry) != nItemId)
                {
                    continue;
                }

                if (*reinterpret_cast<const int*>(pSnapshot[j].pEntry + OFF_TempStat_Remaining) > nRemaining)
                {
                    bResent = true;   // this copy is not the freshest of the stale group
                    break;
                }
            }

            if (bResent)
            {
                ++nLegacy;
            }
        }

        if (bResent)
        {
            CDLog("  stale duplicate left in place type=%d id=%d (list untouched)", nType, nId);
            ++nDropped;
        }
    }

    if (nDropped > 0)
    {
        CDLog("stale-scan: %d duplicate(s) reported, %d from pre-existing groups, none removed", nDropped, nLegacy);
    }

    return nDropped;
}

static void __fastcall OnTemporaryStatSet_Hook(void* pThis, void* edx, void* pPacket)
{
    ViewEntry snapshot[nMaxViewEntries];
    int nSnapshot = 0;

    if (CardDefenseAttr::bCoexist && pThis != nullptr)
    {
        int incoming[nMaxIncomingIds];
        const int nIncoming = ReadIncomingBuffIds(pPacket, incoming, nMaxIncomingIds);
        nSnapshot = SnapshotProtectedEntries(pThis, snapshot, nMaxViewEntries, incoming, nIncoming, true);
        CDLog("ontemp: incoming=%d protected=%d", nIncoming, nSnapshot);
    }

    __try
    {
        if (g_origOnTemporaryStatSet != nullptr)
        {
            g_origOnTemporaryStatSet(pThis, edx, pPacket);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    __try
    {
        for (int i = 0; i < nSnapshot; ++i)
        {
            if (!IsBadReadPtr(snapshot[i].pEntry, OFF_TempStat_Remaining + sizeof(int)))
            {
                memcpy(snapshot[i].pEntry + OFF_TempStat_Mask, snapshot[i].mask.d, sizeof(Mask128));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    ReportStaleDuplicates(pThis, snapshot, nSnapshot);
}

static void __fastcall OnTemporaryStatReset_Hook(void* pThis, void* edx, void* pPacket)
{
    ViewEntry snapshot[nMaxViewEntries];
    int nSnapshot = 0;

    // A CANCEL_BUFF carries only the mask, and a card/coupon pair shares its bit, so the client cannot
    // tell a coupon cancel from a card cancel. Cards are therefore kept only while a coupon shares
    // their bit -- that is the coupon chain's cancel/re-add churn (login, channel change, coupon
    // change), which used to take the card entry with it; coupons are never kept here, because their
    // entries carry a ~24 h countdown and a wrongly kept one would be a permanent phantom icon.
    if (CardDefenseAttr::bCoexist && pThis != nullptr)
    {
        ViewEntry candidates[nMaxViewEntries];
        // A CANCEL_BUFF carries only the mask (no ids), so every card entry is a candidate here.
        const int nCandidates = SnapshotProtectedEntries(pThis, candidates, nMaxViewEntries, nullptr, 0, false);
        for (int i = 0; i < nCandidates; ++i)
        {
            const int nItemId = GetItemBuffId(candidates[i].pEntry);
            if (nItemId == 0 || nItemId / 10000 != 238)
            {
                continue;
            }

            if (!HasOverlappingRateCoupon(pThis, candidates[i].mask))
            {
                continue;
            }

            snapshot[nSnapshot] = candidates[i];
            *reinterpret_cast<unsigned int*>(candidates[i].pEntry + OFF_TempStat_Mask) |= kInertMaskBit;
            ++nSnapshot;
        }
    }

    __try
    {
        if (g_origOnTemporaryStatReset != nullptr)
        {
            g_origOnTemporaryStatReset(pThis, edx, pPacket);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    __try
    {
        for (int i = 0; i < nSnapshot; ++i)
        {
            if (!IsBadReadPtr(snapshot[i].pEntry, OFF_TempStat_Remaining + sizeof(int)))
            {
                memcpy(snapshot[i].pEntry + OFF_TempStat_Mask, snapshot[i].mask.d, sizeof(Mask128));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    // No relayout call either: AdjustPosition is another internal list operation and this module now
    // touches no list at all (it only edits its own entries' masks); the client lays the icons out in
    // its own flow (see docs/bugs/031).
}

void CardDefenseAttr::Hook()
{
    Hook_CardDefenseAttr(bCoexist);
}

void Hook_CardDefenseAttr(bool coexist)
{
    CDLog("=== CardDefenseAttr (coexistence build 6, id-aware, no list surgery): coexist=%d ===", coexist ? 1 : 0);

    if (!coexist)
    {
        return;
    }

    if (g_origOnTemporaryStatSet == nullptr)
    {
        g_origOnTemporaryStatSet = reinterpret_cast<OnTemporaryStatSet_t>(ADDR_CWvsContext_OnTemporaryStatSet);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnTemporaryStatSet),
            reinterpret_cast<void*>(&OnTemporaryStatSet_Hook)))
        {
            g_origOnTemporaryStatSet = nullptr;
            CDLog("OnTemporaryStatSet hook FAILED");
        }
        else
        {
            CDLog("OnTemporaryStatSet hook installed");
        }
    }

    if (g_origOnTemporaryStatReset == nullptr)
    {
        g_origOnTemporaryStatReset = reinterpret_cast<OnTemporaryStatReset_t>(ADDR_CWvsContext_OnTemporaryStatReset);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnTemporaryStatReset),
            reinterpret_cast<void*>(&OnTemporaryStatReset_Hook)))
        {
            g_origOnTemporaryStatReset = nullptr;
            CDLog("OnTemporaryStatReset hook FAILED");
        }
        else
        {
            CDLog("OnTemporaryStatReset hook installed");
        }
    }

    CDLog("hooks done: ontemp=%d onreset=%d",
        g_origOnTemporaryStatSet != nullptr ? 1 : 0,
        g_origOnTemporaryStatReset != nullptr ? 1 : 0);
}

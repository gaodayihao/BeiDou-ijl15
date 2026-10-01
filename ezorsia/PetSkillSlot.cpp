#include "stdafx.h"
#include "PetSkillSlot.h"
#include "Memory.h"
#include "PetBuffWhitelist.h"
#include <stdio.h>
#include <stdarg.h>

// ===== Reverse-engineering anchors (Angel.exe / BeiDou.exe, v83; bookmarks prefixed "PETBUFF:") ====
//
// --- where the drop lands --------------------------------------------------
//   CDraggableSkill::OnDropped       0x004FAA22  (pFrom, pTo, x, y) -> int, __thiscall
//     the skill id sits at *(this + 6)  -- the original reads it as
//     `CSkillInfo::GetSkillLevel(cd, *(this + 6), 0) <= 0 -> return 0`, i.e. "not learned, refuse"
//   CWndMan::EndDragDrop             0x009E37C2  calls the draggable's vtable+4 (OnDropped) with
//     (pFrom, pTo, x, y) directly -- the target window's HitTest is NOT consulted, so accepting a
//     drop here needs no patch to sub_8011FA at all.
//
// --- telling the pet equip window apart ------------------------------------
//   ctor sub_7FE299 writes *obj = off_B38A70 (primary vtable), obj[1] = off_B38A24, obj[2] =
//   off_B38A20, then CWnd::CreateWnd(id = 0xB1, w = 181). The drag context hands out window
//   pointers as `obj + 4` (that is why OnDropped itself does `p - 4` everywhere), so the window is
//   recognised by comparing the pointer's own vtable with off_B38A24, or the dword just before it
//   with off_B38A70. Both are tried and the outcome is logged.
//
// --- the cells -------------------------------------------------------------
//   the window's rect table (0x00BE2260, interleaved x/y, 50 entries) has, at y = 44, only
//   x = 13 (item pouch, index 21) and x = 46 (meso magnet, index 22). Cells #3/#4 (x = 79 and
//   x = 112) are absent -> this module owns them. Geometry (32x32, pitch 33):
//     slot 0: x 79..111, y 44..76
//     slot 1: x 112..144, y 44..76
//
// --- what this file does NOT do yet ---------------------------------------
//   drawing the skill icon. The client resolves it as
//   `*(SKILLENTRY + 0xAC)` from `CSkillInfo::GetSkill(skillId)` (0x0075C755), used by the buff
//   entry ctor 0x007B3176's nType == 2 branch; handing that canvas to a layer needs
//   IWzGr2DLayer::Animate (vtable +260) whose stack shape is still to be pinned down.
//   Until then the module logs its decisions to petbuff.log so the drop path can be calibrated.
//
// =================================================================================================

static const DWORD ADDR_DraggableSkill_OnDropped = 0x004FAA22;
static const DWORD VTBL_CUIPetEquip_Primary = 0x00B38A70; // *(void**)obj
static const DWORD VTBL_CUIPetEquip_Second = 0x00B38A24;  // *(void**)(obj + 4)

static const int OFF_DraggableSkill_SkillId = 6 * 4; // *(this + 6)
static const int OFF_CUIPetEquip_Tab = 360 * 4;      // *(this + 360), 0..2

// Window-local pixels of the two free cells in the second row.
struct SlotRect
{
    int nLeft;
    int nTop;
    int nRight;
    int nBottom;
};

static const SlotRect kSlotRects[2] = {
    { 79, 44, 111, 76 },
    { 112, 44, 144, 76 },
};

static const int kPetCount = 3;
static const int kSlotCount = 2;

// The remembered configuration. Client-side only, lost when the client closes.
static int g_anSlots[kPetCount][kSlotCount] = {};

bool PetSkillSlot::bEnabled = true;
bool PetSkillSlot::bDebug = true;

typedef int(__thiscall* tDraggableSkillOnDropped)(void* pThis, void* pFrom, void* pTo, int nX, int nY);
static tDraggableSkillOnDropped g_origOnDropped = nullptr;

// petbuff.log in the client directory: the drop path is calibrated from it (window pointer, tab,
// skill id and the raw drop coordinates as the client reports them).
static void LogLine(const char* sFormat, ...)
{
    if (!PetSkillSlot::bDebug)
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

// True when pTo is the pet equip window; *ppObject receives the object base the offsets are
// relative to (the client's own convention is `interface - 4`, see OnDropped's `a3 - 4`).
static bool IsPetEquipWindow(void* pTo, void** ppObject)
{
    *ppObject = nullptr;
    if (pTo == nullptr)
    {
        return false;
    }

    __try
    {
        void* pVtbl = *reinterpret_cast<void**>(pTo);
        if (pVtbl == reinterpret_cast<void*>(VTBL_CUIPetEquip_Second))
        {
            *ppObject = reinterpret_cast<char*>(pTo) - 4;
            return true;
        }

        void* pPrev = *reinterpret_cast<void**>(reinterpret_cast<char*>(pTo) - 4);
        if (pPrev == reinterpret_cast<void*>(VTBL_CUIPetEquip_Primary))
        {
            *ppObject = pTo;
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *ppObject = nullptr;
        return false;
    }

    return false;
}

static int __fastcall OnDropped_Hook(void* pThis, void* /*edx*/, void* pFrom, void* pTo, int nX, int nY)
{
    int nResult = 0;
    if (g_origOnDropped != nullptr)
    {
        nResult = g_origOnDropped(pThis, pFrom, pTo, nX, nY);
    }

    // The client already consumed the drop (quickslot, macro, skill window, ...): leave it alone.
    if (nResult != 0)
    {
        return nResult;
    }

    void* pObject = nullptr;
    if (!IsPetEquipWindow(pTo, &pObject))
    {
        return 0;
    }

    __try
    {
        const int nSkillId = *reinterpret_cast<int*>(reinterpret_cast<char*>(pThis) + OFF_DraggableSkill_SkillId);
        const int nPet = *reinterpret_cast<int*>(reinterpret_cast<char*>(pObject) + OFF_CUIPetEquip_Tab);

        LogLine("drop to=%p obj=%p tab=%d skill=%d x=%d y=%d whitelisted=%d",
            pTo, pObject, nPet, nSkillId, nX, nY, IsPetBuffSkill(nSkillId) ? 1 : 0);

        if (!IsPetBuffSkill(nSkillId))
        {
            return 0;
        }

        if (nPet < 0 || nPet >= kPetCount)
        {
            LogLine("  refused: tab %d out of range", nPet);
            return 0;
        }

        // Provisional cell test. The client's drop coordinates have not been calibrated yet, so a
        // miss falls back to splitting the cell row by x and says so in the log.
        int nSlot = -1;
        for (int i = 0; i < kSlotCount; ++i)
        {
            const SlotRect& r = kSlotRects[i];
            if (nX >= r.nLeft && nX <= r.nRight && nY >= r.nTop && nY <= r.nBottom)
            {
                nSlot = i;
                break;
            }
        }

        if (nSlot < 0)
        {
            nSlot = (nX >= kSlotRects[1].nLeft) ? 1 : 0;
            LogLine("  cell fallback: slot=%d", nSlot);
        }

        if (g_anSlots[nPet][nSlot] != nSkillId)
        {
            g_anSlots[nPet][nSlot] = nSkillId;
            LogLine("  stored pet=%d slot=%d skill=%d", nPet, nSlot, nSkillId);
        }

        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogLine("  exception while reading the drop");
        return 0;
    }
}

int PetSkillSlot::GetSkill(int nPet, int nSlot)
{
    if (nPet < 0 || nPet >= kPetCount || nSlot < 0 || nSlot >= kSlotCount)
    {
        return 0;
    }

    return g_anSlots[nPet][nSlot];
}

void PetSkillSlot::Hook()
{
    Hook_PetSkillSlot(bEnabled);
}

void Hook_PetSkillSlot(bool enable)
{
    if (!enable)
    {
        std::cout << "pet auto-buff slots disabled by config (petBuff=false)" << std::endl;
        return;
    }

    // Version sentinel: OnDropped starts with "mov eax, [esp+...]" style prologue; the client build
    // is checked by the first byte being a normal push (0x56 = push esi is what this build has).
    if (g_origOnDropped == nullptr)
    {
        g_origOnDropped = reinterpret_cast<tDraggableSkillOnDropped>(ADDR_DraggableSkill_OnDropped);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnDropped),
            reinterpret_cast<void*>(&OnDropped_Hook)))
        {
            g_origOnDropped = nullptr;
            std::cout << "pet auto-buff slots: OnDropped hook FAILED" << std::endl;
            return;
        }

        std::cout << "pet auto-buff slots hook created (CDraggableSkill::OnDropped)" << std::endl;
        LogLine("--- pet auto-buff slots armed (build 0x%08X) ---", ADDR_DraggableSkill_OnDropped);
    }
}

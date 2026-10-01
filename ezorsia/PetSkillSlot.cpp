#include "stdafx.h"
#include "PetSkillSlot.h"
#include "Memory.h"
#include "PetBuffWhitelist.h"
#include <stdio.h>
#include <stdarg.h>
#include <oleauto.h> // VARIANTARG / VT_UNKNOWN (the Gr2D wrappers take Ztl_variant_t by reference)
#include <comdef.h>

// ===== Reverse-engineering anchors (Angel.exe / BeiDou.exe, v83; bookmarks prefixed "PETBUFF:") ====
//
// --- where the drop lands --------------------------------------------------
//   CDraggableSkill::OnDropped       0x004FAA22  (pFrom, pTo, x, y) -> int, __thiscall
//     the skill id sits at *(this + 6)  -- the original reads it as
//     `CSkillInfo::GetSkillLevel(cd, *(this + 6), 0) <= 0 -> return 0`, i.e. "not learned, refuse"
//     the drop coordinates are WINDOW-LOCAL (verified in game: a drop on cell #3 logged x=88 y=58)
//   CWndMan::EndDragDrop             0x009E37C2  calls the draggable's vtable+4 (OnDropped) with
//     (pFrom, pTo, x, y) directly -- the target window's HitTest is NOT consulted, so accepting a
//     drop here needs no patch to sub_8011FA at all.
//
// --- telling the pet equip window apart ------------------------------------
//   ctor sub_7FE299 writes *obj = off_B38A70 (primary vtable), obj[1] = off_B38A24, obj[2] =
//   off_B38A20, then CWnd::CreateWnd(id = 0xB1, w = 181). Drag-context window pointers are
//   `obj + 4` (verified: the log shows to = obj + 4), so the window is recognised by comparing
//   the pointer's own vtable with off_B38A24, or the dword just before it with off_B38A70.
//   A drop through the primary vtable arrives with `this == obj`, which is also the base the tab
//   field and the rect table are relative to.
//
// --- the cells -------------------------------------------------------------
//   the window's rect table (0x00BE2260, interleaved x/y, 50 entries) has, at y = 44, only
//   x = 13 (item pouch, index 21) and x = 46 (meso magnet, index 22). Cells #3/#4 (x = 79 and
//   x = 112) are absent -> this module owns them. Geometry (32x32, pitch 33), verified in game.
//
// --- painting the skill icon ----------------------------------------------
//   the client resolves a skill's icon as `*(SKILLENTRY + 0xAC)` from
//   `CSkillInfo::GetSkill(skillId)` (0x0075C755, singleton pointer at 0x00BE78DC -- the client
//   loads ecx from there right before the call). Its own buff row paints exactly that canvas:
//   the CTemporaryStatView entry ctor (0x007B3176) takes the nType == 2 branch, assigns the icon
//   through the com-ptr helper 0x004051E5, then hands it to a layer.
//
//   "show a canvas on a layer" is IWzGr2DLayer::Animate (0x00426BAB). It is a thunk that ends in
//   `retn 1Ch` -- 7 stack dwords -- and forwards to its object's vtable+260, expanding five
//   Ztl_variant_t BY VALUE. Both call sites that matter agree on the shape:
//     0x007B3176 (buff entry ctor): Animate(layer, &retbuf, canvas, 500, 210, 64, empty, empty)
//     0x00800214 (drag ghost)     : Animate(layer, &retbuf, canvas, empty, empty, empty, empty, empty)
//   so this module calls it as (layer, retbuf16, canvas, empty x5) -- and it never spends an icon
//   canvas of its own, it reuses the one the client cached in SKILLENTRY.
//
// --- layer lifecycle -------------------------------------------------------
//   the window is created and destroyed on every toggle of the 宠物装备 button
//   (CUIEquip::TogglePetEquip 0x007FFA84 -> ctor sub_7FE299 / destroy), so the layers are
//   positioned on each CUIPetEquip::OnMouseMove and hidden from CWnd::Destroy (0x009E00AF) when
//   the window goes away.
//
// =================================================================================================

static const DWORD ADDR_DraggableSkill_OnDropped = 0x004FAA22;
static const DWORD ADDR_CUIPetEquip_OnMouseMove = 0x00800F7B;
static const DWORD ADDR_CWnd_Destroy = 0x009E00AF;
static const DWORD ADDR_CWnd_GetAbsLeft = 0x009E03C5;
static const DWORD ADDR_CWnd_GetAbsTop = 0x009E0447;
static const DWORD ADDR_CSkillInfo_GetSkill = 0x0075C755;
static const DWORD ADDR_SkillInfoInstance = 0x00BE78DC;
static const DWORD ADDR_IWzGr2DLayer_Animate = 0x00426BAB;
static const DWORD ADDR_IWzGr2D_CreateLayer = 0x00426C7E;
static const DWORD ADDR_IWzGr2D_GetCenter = 0x004374CB;
static const DWORD ADDR_IWzGr2DLayer_Putcolor = 0x0045144A;
static const DWORD ADDR_Gr2DInstance = 0x00BF14EC;
static const DWORD ADDR_EmptyVariant = 0x00BF6300;

static const DWORD VTBL_CUIPetEquip_Primary = 0x00B38A70; // *(void**)obj
static const DWORD VTBL_CUIPetEquip_Second = 0x00B38A24;  // *(void**)(obj + 4)

static const int OFF_DraggableSkill_SkillId = 6 * 4;    // *(this + 6)
static const int OFF_CUIPetEquip_Tab = 360 * 4;         // *(this + 360), 0..2
static const int OFF_SKILLENTRY_Icon = 0xAC;            // *(SKILLENTRY + 0xAC) = 32x32 canvas

static const int nVtbl_IWzVector2D__put_origin = 100;
static const int nVtbl_IWzVector2D__raw_RelMove = 144;
static const int nVtbl_IWzGr2DLayer__PutZ = 180;

// Visible / invisible alpha for a slot layer (Putcolor is ARGB).
static const unsigned long kLayerVisible = 0xFFFFFFFFu;
static const unsigned long kLayerHidden = 0x00000000u;

// Window-local pixels of the two free cells in the second row (verified in game).
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

// The pet equip window the layers belong to, and one layer per slot.
static void* g_pWindow = nullptr;
static void* g_apLayers[kPetCount][kSlotCount] = {};
static int g_anDrawn[kPetCount][kSlotCount] = {};

bool PetSkillSlot::bEnabled = true;
bool PetSkillSlot::bDebug = true;

typedef void* (__fastcall* Gr2DCreateLayer_t)(void* pGr2D, void* edx, void** ppLayer, int nX, int nY,
    unsigned long nWidth, unsigned long nHeight, int nZ, const void* pV1, const void* pV2);
typedef void* (__fastcall* Gr2DGetCenter_t)(void* pGr2D, void* edx, void** ppCenter);
typedef long(__stdcall* VectorPutOrigin_t)(void* pVector, VARIANTARG vOrigin);
typedef long(__stdcall* VectorRawRelMove_t)(void* pVector, long nX, long nY, VARIANTARG v1, VARIANTARG v2);
typedef long(__stdcall* LayerPutZ_t)(void* pLayer, int nZ);
typedef void (__fastcall* LayerPutColor_t)(void* pLayer, void* edx, unsigned long nColor);
typedef void* (__thiscall* LayerAnimate_t)(void* pLayer, void* pRetBuf, void* pCanvas,
    const void* pV1, const void* pV2, const void* pV3, const void* pV4, const void* pV5);
typedef int(__thiscall* CWndGetAbs_t)(void* pWnd);
typedef void (__thiscall* CWndDestroy_t)(void* pWnd);
typedef int(__thiscall* PetEquipOnMouseMove_t)(void* pThis, int nX, int nY);
typedef void* (__thiscall* SkillInfoGetSkill_t)(void* pSkillInfo, int nSkillId);
typedef int(__thiscall* DraggableSkillOnDropped_t)(void* pThis, void* pFrom, void* pTo, int nX, int nY);

static auto _create_layer = reinterpret_cast<Gr2DCreateLayer_t>(ADDR_IWzGr2D_CreateLayer);
static auto _gr2d_get_center = reinterpret_cast<Gr2DGetCenter_t>(ADDR_IWzGr2D_GetCenter);
static auto _layer_put_color = reinterpret_cast<LayerPutColor_t>(ADDR_IWzGr2DLayer_Putcolor);
static auto _layer_animate = reinterpret_cast<LayerAnimate_t>(ADDR_IWzGr2DLayer_Animate);
static auto _wnd_abs_left = reinterpret_cast<CWndGetAbs_t>(ADDR_CWnd_GetAbsLeft);
static auto _wnd_abs_top = reinterpret_cast<CWndGetAbs_t>(ADDR_CWnd_GetAbsTop);
static auto _skill_get = reinterpret_cast<SkillInfoGetSkill_t>(ADDR_CSkillInfo_GetSkill);

static DraggableSkillOnDropped_t g_origOnDropped = nullptr;
static PetEquipOnMouseMove_t g_origOnMouseMove = nullptr;
static CWndDestroy_t g_origDestroy = nullptr;

// petbuff.log in the client directory: the drop path was calibrated from it (window pointer, tab,
// skill id and the drop coordinates as the client reports them).
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

// ---------------------------------------------------------------------------------------------
// Gr2D plumbing (same shape as BuffTimer.cpp / QuestBulb.cpp)
// ---------------------------------------------------------------------------------------------

static void CopyEmptyVariant(void* pOut)
{
    memcpy(pOut, reinterpret_cast<const void*>(ADDR_EmptyVariant), 16);
}

static void ReleaseComPtr(void* pUnknown)
{
    if (pUnknown == nullptr)
    {
        return;
    }

    __try
    {
        void** pVtbl = *reinterpret_cast<void***>(pUnknown);
        if (pVtbl != nullptr && !IsBadReadPtr(pVtbl, 12))
        {
            reinterpret_cast<void(__stdcall*)(void*)>(pVtbl[2])(pUnknown);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

static bool SetLayerOrigin(void* pLayer, void* pOrigin)
{
    void** pVtbl = *reinterpret_cast<void***>(pLayer);
    if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__put_origin + sizeof(void*)))
    {
        return false;
    }

    VARIANTARG vOrigin;
    memset(&vOrigin, 0, sizeof(vOrigin));
    vOrigin.vt = VT_UNKNOWN;
    vOrigin.punkVal = reinterpret_cast<IUnknown*>(pOrigin);

    __try
    {
        reinterpret_cast<VectorPutOrigin_t>(pVtbl[nVtbl_IWzVector2D__put_origin / sizeof(void*)])
            (pLayer, vOrigin);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool MoveLayer(void* pLayer, long nX, long nY)
{
    void** pVtbl = *reinterpret_cast<void***>(pLayer);
    if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__raw_RelMove + sizeof(void*)))
    {
        return false;
    }

    // Both variants must be spelled out: calling with x/y alone leaves 32 bytes of arguments
    // missing and the callee pops its own count, which corrupts the stack further out
    // (QuestBulb.cpp §7 hit exactly this).
    VARIANTARG v1;
    VARIANTARG v2;
    CopyEmptyVariant(&v1);
    CopyEmptyVariant(&v2);

    __try
    {
        reinterpret_cast<VectorRawRelMove_t>(pVtbl[nVtbl_IWzVector2D__raw_RelMove / sizeof(void*)])
            (pLayer, nX, nY, v1, v2);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static bool LayerPutZ(void* pLayer, int nZ)
{
    void** pVtbl = *reinterpret_cast<void***>(pLayer);
    if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzGr2DLayer__PutZ + sizeof(void*)))
    {
        return false;
    }

    __try
    {
        reinterpret_cast<LayerPutZ_t>(pVtbl[nVtbl_IWzGr2DLayer__PutZ / sizeof(void*)])(pLayer, nZ);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void SetLayerVisible(void* pLayer, bool bVisible)
{
    __try
    {
        _layer_put_color(pLayer, nullptr, bVisible ? kLayerVisible : kLayerHidden);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// One fresh layer, parented to the Gr2D centre (screen frame) so MoveLayer takes screen pixels.
static void* CreateSlotLayer()
{
    void* pGr2D = *reinterpret_cast<void**>(ADDR_Gr2DInstance);
    if (pGr2D == nullptr || IsBadReadPtr(pGr2D, sizeof(void*)))
    {
        return nullptr;
    }

    unsigned char aEmpty1[16];
    unsigned char aEmpty2[16];
    CopyEmptyVariant(aEmpty1);
    CopyEmptyVariant(aEmpty2);

    void* pLayer = nullptr;
    try
    {
        _create_layer(pGr2D, nullptr, &pLayer, 0, 0, 0, 0, 0, aEmpty1, aEmpty2);
    }
    catch (...)
    {
        return nullptr;
    }

    if (pLayer == nullptr)
    {
        return nullptr;
    }

    void* pCenter = nullptr;
    try
    {
        _gr2d_get_center(pGr2D, nullptr, &pCenter);
    }
    catch (...)
    {
        pCenter = nullptr;
    }

    if (pCenter != nullptr)
    {
        SetLayerOrigin(pLayer, pCenter);
        ReleaseComPtr(pCenter);
    }

    LayerPutZ(pLayer, 0);
    return pLayer;
}

// Hands the skill's own icon canvas to the layer: Animate(layer, retbuf, canvas, empty x5).
static bool AnimateIcon(void* pLayer, void* pCanvas)
{
    if (pLayer == nullptr || pCanvas == nullptr)
    {
        return false;
    }

    unsigned char aRetBuf[16];
    memset(aRetBuf, 0, sizeof(aRetBuf));

    unsigned char aVariants[5][16];
    for (int i = 0; i < 5; ++i)
    {
        CopyEmptyVariant(aVariants[i]);
    }

    __try
    {
        _layer_animate(pLayer, aRetBuf, pCanvas,
            aVariants[0], aVariants[1], aVariants[2], aVariants[3], aVariants[4]);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void* GetSkillIconCanvas(int nSkillId)
{
    void* pInfo = *reinterpret_cast<void**>(ADDR_SkillInfoInstance);
    if (pInfo == nullptr || nSkillId <= 0)
    {
        return nullptr;
    }

    __try
    {
        void* pEntry = _skill_get(pInfo, nSkillId);
        if (pEntry == nullptr)
        {
            return nullptr;
        }

        return *reinterpret_cast<void**>(reinterpret_cast<char*>(pEntry) + OFF_SKILLENTRY_Icon);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// Slot layers
// ---------------------------------------------------------------------------------------------

static void HideAllLayers()
{
    for (int nPet = 0; nPet < kPetCount; ++nPet)
    {
        for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
        {
            if (g_apLayers[nPet][nSlot] != nullptr)
            {
                SetLayerVisible(g_apLayers[nPet][nSlot], false);
            }
            g_anDrawn[nPet][nSlot] = 0;
        }
    }
}

// Positions every configured slot on the window and (re)paints the ones whose skill changed.
static void RefreshLayers()
{
    if (g_pWindow == nullptr)
    {
        return;
    }

    const int nAbsLeft = _wnd_abs_left(g_pWindow);
    const int nAbsTop = _wnd_abs_top(g_pWindow);
    const int nPet = *reinterpret_cast<int*>(reinterpret_cast<char*>(g_pWindow) + OFF_CUIPetEquip_Tab);

    if (nPet < 0 || nPet >= kPetCount)
    {
        return;
    }

    for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
    {
        const int nSkillId = g_anSlots[nPet][nSlot];
        void* pLayer = g_apLayers[nPet][nSlot];

        if (nSkillId == 0)
        {
            if (pLayer != nullptr)
            {
                SetLayerVisible(pLayer, false);
            }
            g_anDrawn[nPet][nSlot] = 0;
            continue;
        }

        if (pLayer == nullptr)
        {
            pLayer = CreateSlotLayer();
            g_apLayers[nPet][nSlot] = pLayer;
            g_anDrawn[nPet][nSlot] = 0;
            if (pLayer == nullptr)
            {
                LogLine("  layer create failed pet=%d slot=%d", nPet, nSlot);
                continue;
            }
        }

        if (g_anDrawn[nPet][nSlot] != nSkillId)
        {
            void* pCanvas = GetSkillIconCanvas(nSkillId);
            if (pCanvas == nullptr)
            {
                LogLine("  no icon canvas for skill=%d", nSkillId);
                continue;
            }

            if (AnimateIcon(pLayer, pCanvas))
            {
                g_anDrawn[nPet][nSlot] = nSkillId;
                LogLine("  painted pet=%d slot=%d skill=%d", nPet, nSlot, nSkillId);
            }
            else
            {
                LogLine("  animate failed pet=%d slot=%d skill=%d", nPet, nSlot, nSkillId);
                continue;
            }
        }

        MoveLayer(pLayer, nAbsLeft + kSlotRects[nSlot].nLeft, nAbsTop + kSlotRects[nSlot].nTop);
        SetLayerVisible(pLayer, true);
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

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

        g_anSlots[nPet][nSlot] = nSkillId;
        LogLine("  stored pet=%d slot=%d skill=%d", nPet, nSlot, nSkillId);

        g_pWindow = pObject;
        RefreshLayers();
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LogLine("  exception while reading the drop");
        return 0;
    }
}

// The window is repainted and dragged around while it is open; re-positioning on every mouse move
// keeps the layers on their cells without needing an update hook of our own.
static int __fastcall OnMouseMove_Hook(void* pThis, void* /*edx*/, int nX, int nY)
{
    int nResult = 0;
    if (g_origOnMouseMove != nullptr)
    {
        nResult = g_origOnMouseMove(pThis, nX, nY);
    }

    __try
    {
        if (pThis != nullptr && *reinterpret_cast<void**>(pThis) != nullptr)
        {
            if (g_pWindow != pThis)
            {
                g_pWindow = pThis;
            }

            RefreshLayers();
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return nResult;
}

// The pet equip window is created and destroyed on every toggle, so its layers must not outlive it.
static void __fastcall Destroy_Hook(void* pThis, void* /*edx*/)
{
    __try
    {
        if (g_pWindow != nullptr &&
            (pThis == g_pWindow || pThis == reinterpret_cast<char*>(g_pWindow) + 4))
        {
            HideAllLayers();
            g_pWindow = nullptr;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    if (g_origDestroy != nullptr)
    {
        g_origDestroy(pThis);
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

    if (g_origOnDropped == nullptr)
    {
        g_origOnDropped = reinterpret_cast<DraggableSkillOnDropped_t>(ADDR_DraggableSkill_OnDropped);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnDropped),
            reinterpret_cast<void*>(&OnDropped_Hook)))
        {
            g_origOnDropped = nullptr;
            std::cout << "pet auto-buff slots: OnDropped hook FAILED" << std::endl;
            return;
        }
    }

    if (g_origOnMouseMove == nullptr)
    {
        g_origOnMouseMove = reinterpret_cast<PetEquipOnMouseMove_t>(ADDR_CUIPetEquip_OnMouseMove);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnMouseMove),
            reinterpret_cast<void*>(&OnMouseMove_Hook)))
        {
            g_origOnMouseMove = nullptr;
            std::cout << "pet auto-buff slots: OnMouseMove hook FAILED" << std::endl;
        }
    }

    if (g_origDestroy == nullptr)
    {
        g_origDestroy = reinterpret_cast<CWndDestroy_t>(ADDR_CWnd_Destroy);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origDestroy),
            reinterpret_cast<void*>(&Destroy_Hook)))
        {
            g_origDestroy = nullptr;
        }
    }

    std::cout << "pet auto-buff slots hook created (CDraggableSkill::OnDropped)" << std::endl;
    LogLine("--- pet auto-buff slots armed (build 0x%08X) ---", ADDR_DraggableSkill_OnDropped);
}

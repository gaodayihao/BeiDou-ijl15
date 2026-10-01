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
// --- where a slot layer hangs ----------------------------------------------
//   CWnd::CreateWnd (0x009DE4D2) keeps the window's own Gr2D layer at *(CWnd + 0x18)
//   (CWnd::GetLayer 0x00426604 is literally `return *(this + 6)`), and 0x00800214 -- the window's
//   own mouse-down handler, drawing the drag ghost over a potion cell -- shows the shape a child
//   layer must take:
//       CreateLayer(gr2d, &layer, 0, 0, 0, 0, 0, empty, empty)
//       Animate(layer, &retbuf, canvas, empty x5)
//       Putcolor(layer, 0x80FFFFFF)                      vtable+224; CreateLayer leaves it 0
//       put_origin(layer, CWnd::GetLayer(this - 4))      hang it on the window's own layer
//       raw_RelMove(layer, x, y, empty, empty)           the same frame as its own mouse coords
//   So the slot layers hang on that layer and move in WINDOW-LOCAL pixels: no screen position is
//   needed anywhere, and the icons follow the window while it is dragged.
//
//   CWnd::GetAbsLeft (0x009E03C5) must NOT be used for this: it dereferences *(CWnd + 0x14), which
//   CreateWnd writes as `*(this + 5) = ++dword_BF1604` -- a window sequence number -- so Getx()
//   runs on a small integer. Every drop took an access violation there (petbuff.log).
//
// --- layer lifecycle -------------------------------------------------------
//   the window is created and destroyed on every toggle of the 宠物装备 button
//   (CUIEquip::TogglePetEquip 0x007FFA84 -> ctor sub_7FE299 / destroy), so the layers are
//   re-positioned on each CUIPetEquip::OnMouseMove (that is also what picks up a tab switch) and
//   released from CWnd::Destroy (0x009E00AF) when the window goes away. Release is not enough on
//   its own: put_origin holds the window's layer, so the origin is handed back to the Gr2D centre
//   first, the way BuffTimer::ClearLabelLayer does it -- otherwise the window's graphics outlive
//   the window.
//
// =================================================================================================

static const DWORD ADDR_DraggableSkill_OnDropped = 0x004FAA22;
static const DWORD ADDR_CUIPetEquip_OnMouseMove = 0x00800F7B;
static const DWORD ADDR_CWnd_Destroy = 0x009E00AF;
static const DWORD ADDR_CSkillInfo_GetSkill = 0x0075C755;
static const DWORD ADDR_SkillInfoInstance = 0x00BE78DC;
static const DWORD ADDR_IWzGr2DLayer_Animate = 0x00426BAB;
static const DWORD ADDR_IWzGr2D_CreateLayer = 0x00426C7E;
static const DWORD ADDR_IWzGr2D_GetCenter = 0x004374CB;
static const DWORD ADDR_IWzGr2DLayer_Putcolor = 0x0045144A;
static const DWORD ADDR_IWzGr2DLayer_GetZ = 0x0044337D;
static const DWORD ADDR_IWzGr2DLayer_GetWidth = 0x00440C00;
static const DWORD ADDR_IWzGr2DLayer_GetHeight = 0x00440C2A;
static const DWORD ADDR_IWzCanvas_Getcx = 0x0040F09B;
static const DWORD ADDR_IWzCanvas_Getcy = 0x0040F0C2;
static const DWORD ADDR_Gr2DInstance = 0x00BF14EC;
static const DWORD ADDR_EmptyVariant = 0x00BF6300;

static const DWORD VTBL_CUIPetEquip_Primary = 0x00B38A70; // *(void**)obj
static const DWORD VTBL_CUIPetEquip_Second = 0x00B38A24;  // *(void**)(obj + 4)

static const int OFF_DraggableSkill_SkillId = 6 * 4;    // *(this + 6)
static const int OFF_CUIPetEquip_Tab = 360 * 4;         // *(this + 360), 0..2
static const int OFF_SKILLENTRY_Icon = 0xAC;            // *(SKILLENTRY + 0xAC) = 32x32 canvas
static const int OFF_CWnd_Layer = 0x18;                 // *(CWnd + 6), the window's own Gr2D layer

static const int nVtbl_IWzVector2D__get_x = 32;
static const int nVtbl_IWzVector2D__get_y = 40;
static const int nVtbl_IWzVector2D__put_origin = 100;
static const int nVtbl_IWzVector2D__raw_RelMove = 144;
static const int nVtbl_IWzGr2DLayer__PutZ = 180;

// Visible / invisible alpha for a slot layer (Putcolor is ARGB).
static const unsigned long kLayerVisible = 0xFFFFFFFFu;
static const unsigned long kLayerHidden = 0x00000000u;

// The pet equip window's own layer is created at depth 10: its ctor sub_7FE299 calls
// CWnd::CreateWnd(nLeft, nTop, /*w*/ 0xB1 = 177, /*h*/ 181, /*z*/ 10, /*bScreenCoord*/ 1, 0, 1),
// and the layer measures 177x181 at runtime, which pins the argument positions.
//
// The slot layers sit ONE ABOVE that, in their own depth group. Larger depths draw in front
// (measured: depth 0 put the icon behind the window, depth 10 in front of it), and a same-depth
// layer only wins on creation order -- which the window takes back every time it is dragged:
// CWndMan::UpdateWindowPosition (0x009E03A6) removes and re-inserts the window's own layer, so the
// icon was buried by the window's canvas as soon as the window moved. A depth of its own cannot be
// overtaken that way. Reading the depth off the parent is not an option either:
// IWzGr2DLayer::GetZ (0x0044337D) answers 0xFFFFFFFF for the window's own layer, not the 10 that
// CreateWnd set.
static const int kSlotLayerZ = 11;

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

// The pet equip window the layers belong to, the window layer they hang on, and one layer per slot.
static void* g_pWindow = nullptr;
static void* g_pParentLayer = nullptr;
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
typedef int(__fastcall* LayerGetInt_t)(void* pLayer, void* edx);
typedef void (__fastcall* LayerPutColor_t)(void* pLayer, void* edx, unsigned long nColor);
// The client's property-get convention is COM-style: __stdcall long get_x(long* pOut) (QuestBulb).
typedef long(__stdcall* VectorGetLong_t)(void* pVector, long* pnOut);
typedef void* (__thiscall* LayerAnimate_t)(void* pLayer, void* pRetBuf, void* pCanvas,
    const void* pV1, const void* pV2, const void* pV3, const void* pV4, const void* pV5);
typedef void (__thiscall* CWndDestroy_t)(void* pWnd);
typedef int(__thiscall* PetEquipOnMouseMove_t)(void* pThis, int nX, int nY);
typedef void* (__thiscall* SkillInfoGetSkill_t)(void* pSkillInfo, int nSkillId);
typedef int(__thiscall* DraggableSkillOnDropped_t)(void* pThis, void* pFrom, void* pTo, int nX, int nY);

static auto _create_layer = reinterpret_cast<Gr2DCreateLayer_t>(ADDR_IWzGr2D_CreateLayer);
static auto _gr2d_get_center = reinterpret_cast<Gr2DGetCenter_t>(ADDR_IWzGr2D_GetCenter);
static auto _layer_put_color = reinterpret_cast<LayerPutColor_t>(ADDR_IWzGr2DLayer_Putcolor);
static auto _layer_get_z = reinterpret_cast<LayerGetInt_t>(ADDR_IWzGr2DLayer_GetZ);
static auto _layer_get_width = reinterpret_cast<LayerGetInt_t>(ADDR_IWzGr2DLayer_GetWidth);
static auto _layer_get_height = reinterpret_cast<LayerGetInt_t>(ADDR_IWzGr2DLayer_GetHeight);
static auto _canvas_get_cx = reinterpret_cast<LayerGetInt_t>(ADDR_IWzCanvas_Getcx);
static auto _canvas_get_cy = reinterpret_cast<LayerGetInt_t>(ADDR_IWzCanvas_Getcy);
static auto _layer_animate = reinterpret_cast<LayerAnimate_t>(ADDR_IWzGr2DLayer_Animate);
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

// IUnknown::AddRef answers with the resulting count, so an AddRef immediately followed by a Release
// reports the current reference count without changing it. Used to tell "my layer still holds the
// window's layer" apart from "the window's own release is the one that matters".
static unsigned ProbeRefCount(void* pUnknown)
{
    if (pUnknown == nullptr)
    {
        return 0;
    }

    __try
    {
        void** pVtbl = *reinterpret_cast<void***>(pUnknown);
        if (pVtbl != nullptr && !IsBadReadPtr(pVtbl, 12))
        {
            const unsigned nCount = reinterpret_cast<unsigned(__stdcall*)(void*)>(pVtbl[1])(pUnknown);
            reinterpret_cast<unsigned(__stdcall*)(void*)>(pVtbl[2])(pUnknown);
            return nCount - 1;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return 0;
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

// The layer the window draws itself into (CWnd::GetLayer 0x00426604: `return *(this + 6)`).
// Borrowed: the window owns it, so this side never AddRefs or Releases it.
static void* GetWindowLayer(void* pWindow)
{
    if (pWindow == nullptr)
    {
        return nullptr;
    }

    __try
    {
        void* pLayer = *reinterpret_cast<void**>(reinterpret_cast<char*>(pWindow) + OFF_CWnd_Layer);
        if (pLayer != nullptr && !IsBadReadPtr(pLayer, sizeof(void*)))
        {
            return pLayer;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return nullptr;
}

// Reports a structured exception through the log. Only ever used from an __except filter, so it
// must not throw and must not build anything that needs unwinding. A C++ exception surfaces here as
// code 0xE06D7363 with the faulting address inside the CRT, an access violation as 0xC0000005.
static int LogSehFilter(EXCEPTION_POINTERS* pInfo)
{
    if (pInfo != nullptr && pInfo->ExceptionRecord != nullptr)
    {
        const EXCEPTION_RECORD* pRecord = pInfo->ExceptionRecord;
        LogLine("  SEH code=0x%08X addr=0x%08X p0=0x%08X p1=0x%08X",
            static_cast<unsigned>(pRecord->ExceptionCode),
            static_cast<unsigned>(static_cast<DWORD_PTR>(reinterpret_cast<DWORD_PTR>(pRecord->ExceptionAddress))),
            pRecord->NumberParameters > 0 ? static_cast<unsigned>(pRecord->ExceptionInformation[0]) : 0u,
            pRecord->NumberParameters > 1 ? static_cast<unsigned>(pRecord->ExceptionInformation[1]) : 0u);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// CreateLayer's first variant is a VT_I4 canvas id, not an empty variant: the client builds it with
// sub_402FAB(&v, 0, 3) in both CWnd::CreateWnd and 0x00800214, and the second variant is a copy of
// the global empty variant. Passing two empty variants makes the w = h = 0 shape fail with
// E_INVALIDARG (0x80070057).
static void MakeIntVariant(void* pOut, long nValue)
{
    memset(pOut, 0, 16);
    *reinterpret_cast<unsigned short*>(pOut) = VT_I4;
    *reinterpret_cast<long*>(reinterpret_cast<char*>(pOut) + 8) = nValue;
}

static void GetVectorPos(void* pVector, long* pnX, long* pnY)
{
    *pnX = 0;
    *pnY = 0;
    if (pVector == nullptr)
    {
        return;
    }

    __try
    {
        void** pVtbl = *reinterpret_cast<void***>(pVector);
        if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__get_y + sizeof(void*)))
        {
            return;
        }
        reinterpret_cast<VectorGetLong_t>(pVtbl[nVtbl_IWzVector2D__get_x / sizeof(void*)])(pVector, pnX);
        reinterpret_cast<VectorGetLong_t>(pVtbl[nVtbl_IWzVector2D__get_y / sizeof(void*) ])(pVector, pnY);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// Everything that decides whether a layer can be seen at all: size (a layer without a canvas draws
// nothing), depth (behind the window's own canvas) and where it actually sits in its frame.
// GetAlpha is deliberately not read: its prototype is not the (this, edx) shape GetWidth/GetHeight
// use, and calling it that way faults inside it at 0x004143F4.
static void DumpLayer(const char* sTag, void* pLayer)
{
    if (pLayer == nullptr)
    {
        LogLine("  %s: null", sTag);
        return;
    }

    __try
    {
        long nX = 0;
        long nY = 0;
        GetVectorPos(pLayer, &nX, &nY);
        LogLine("  %s %p: w=%d h=%d z=0x%08X pos=(%d,%d)", sTag, pLayer,
            _layer_get_width(pLayer, nullptr), _layer_get_height(pLayer, nullptr),
            static_cast<unsigned>(_layer_get_z(pLayer, nullptr)),
            static_cast<int>(nX), static_cast<int>(nY));
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }
}

// Logs the binding only when it actually changes. Mouse moves drive this many times a second, so an
// unconditional line would drown the log; a change is exactly what "the icon vanished" looks like.
static void LogRefreshState(void* pParent, void* pLayer)
{
    static void* s_pParent = nullptr;
    static void* s_pLayer = nullptr;
    static long s_nParentX = 0;
    static long s_nParentY = 0;
    static long s_nLayerX = 0;
    static long s_nLayerY = 0;

    long nParentX = 0;
    long nParentY = 0;
    long nLayerX = 0;
    long nLayerY = 0;
    GetVectorPos(pParent, &nParentX, &nParentY);
    GetVectorPos(pLayer, &nLayerX, &nLayerY);

    if (pParent == s_pParent && pLayer == s_pLayer &&
        nParentX == s_nParentX && nParentY == s_nParentY &&
        nLayerX == s_nLayerX && nLayerY == s_nLayerY)
    {
        return;
    }

    s_pParent = pParent;
    s_pLayer = pLayer;
    s_nParentX = nParentX;
    s_nParentY = nParentY;
    s_nLayerX = nLayerX;
    s_nLayerY = nLayerY;

    __try
    {
        LogLine("  state: parent=%p(%d,%d) layer=%p(%d,%d) rel=(%d,%d) z=0x%08X",
            pParent, static_cast<int>(nParentX), static_cast<int>(nParentY),
            pLayer, static_cast<int>(nLayerX), static_cast<int>(nLayerY),
            static_cast<int>(nLayerX - nParentX), static_cast<int>(nLayerY - nParentY),
            static_cast<unsigned>(pLayer != nullptr ? _layer_get_z(pLayer, nullptr) : 0));
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }
}

static void DumpCanvas(const char* sTag, void* pCanvas)
{
    if (pCanvas == nullptr)
    {
        LogLine("  %s: null", sTag);
        return;
    }

    __try
    {
        LogLine("  %s %p: cx=%d cy=%d", sTag, pCanvas,
            _canvas_get_cx(pCanvas, nullptr), _canvas_get_cy(pCanvas, nullptr));
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }
}

// Runs CreateLayer once, reporting a _com_error with its HRESULT. Separate function from the
// __try/__except wrapper below on purpose: MSVC allows only one form of exception handling per
// function, and these wrappers throw _com_error while a bad pointer raises a structured exception.
static bool CreateLayerRaw(void* pGr2D, void** ppLayer, unsigned long nWidth, unsigned long nHeight,
    int nZ, void* pV1, void* pV2)
{
    try
    {
        _create_layer(pGr2D, nullptr, ppLayer, 0, 0, nWidth, nHeight, nZ, pV1, pV2);
        return true;
    }
    catch (const _com_error& e)
    {
        LogLine("  create: _com_error hr=0x%08X w=%u h=%u z=0x%08X",
            static_cast<unsigned>(e.Error()), nWidth, nHeight, static_cast<unsigned>(nZ));
        return false;
    }
    catch (...)
    {
        LogLine("  create: unknown C++ exception w=%u h=%u z=0x%08X", nWidth, nHeight, static_cast<unsigned>(nZ));
        return false;
    }
}

static bool CreateLayerAttempt(void* pGr2D, void** ppLayer, unsigned long nWidth, unsigned long nHeight,
    int nZ, void* pV1, void* pV2)
{
    __try
    {
        return CreateLayerRaw(pGr2D, ppLayer, nWidth, nHeight, nZ, pV1, pV2);
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
        return false;
    }
}

// One fresh layer hung on the window's own layer, so raw_RelMove takes WINDOW-LOCAL pixels and the
// engine carries the icon along when the window is dragged. Every step is traced (bVerbose) and
// guarded: a fault here must not escape into the drop handler, which would leave the drop
// unconsumed.
static void* CreateSlotLayer(void* pParentLayer, bool bVerbose)
{
    __try
    {
        void* pGr2D = *reinterpret_cast<void**>(ADDR_Gr2DInstance);
        if (pGr2D == nullptr || IsBadReadPtr(pGr2D, sizeof(void*)))
        {
            if (bVerbose) LogLine("  create: no Gr2D instance");
            return nullptr;
        }

        unsigned char aCanvasId[16];
        unsigned char aEmpty[16];
        MakeIntVariant(aCanvasId, 0);
        CopyEmptyVariant(aEmpty);

        if (bVerbose)
        {
            LogLine("  create: gr2d=%p gr2d_vtbl=%p parent=%p", pGr2D, *reinterpret_cast<void**>(pGr2D), pParentLayer);
            DumpLayer("  parent", pParentLayer);
        }

        // The client's own layers are created with width = height = 0 (CWnd::CreateWnd, 0x00800214);
        // BuffTimer's come out 32x32 at depth 0xC006156C. Try the client's shape, fall back to the
        // one that is proven to work in this plugin.
        void* pLayer = nullptr;
        if (!CreateLayerAttempt(pGr2D, &pLayer, 0, 0, kSlotLayerZ, aCanvasId, aEmpty))
        {
            LogLine("  create: retrying with BuffTimer's parameters");
            if (!CreateLayerAttempt(pGr2D, &pLayer, 32, 32, kSlotLayerZ, aCanvasId, aEmpty))
            {
                return nullptr;
            }
        }

        if (bVerbose)
        {
            LogLine("  create: layer=%p", pLayer);
            DumpLayer("  created", pLayer);
        }
        if (pLayer == nullptr)
        {
            return nullptr;
        }

        if (bVerbose) LogLine("  create: put_origin=%d", SetLayerOrigin(pLayer, pParentLayer) ? 1 : 0);
        if (bVerbose) LogLine("  create: putz=%d", LayerPutZ(pLayer, kSlotLayerZ) ? 1 : 0);

        // CreateLayer leaves the layer colour at zero and a zero-alpha layer is not drawn at all
        // (CUIToolTip::MakeLayer ends with exactly this call, 0x00800214 does it with 0x80FFFFFF).
        _layer_put_color(pLayer, nullptr, kLayerVisible);
        return pLayer;
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
        return nullptr;
    }
}

// Takes a slot layer off the screen for good. The order matters: put_origin holds the window's
// layer, so handing the origin back to the Gr2D centre is what releases it -- without that, the
// window's own Destroy cannot free its layer and its graphics stay on screen after it is gone
// (BuffTimer::ClearLabelLayer hit the same thing). Only then does dropping our reference free the
// slot layer itself.
static void DestroySlotLayer(void* pLayer)
{
    if (pLayer == nullptr)
    {
        return;
    }

    __try
    {
        void* pGr2D = *reinterpret_cast<void**>(ADDR_Gr2DInstance);
        void* pCenter = nullptr;
        if (pGr2D != nullptr && !IsBadReadPtr(pGr2D, sizeof(void*)))
        {
            _gr2d_get_center(pGr2D, nullptr, &pCenter);
        }
        if (pCenter != nullptr)
        {
            LogLine("  destroy %p: origin back to the centre=%d", pLayer, SetLayerOrigin(pLayer, pCenter) ? 1 : 0);
            ReleaseComPtr(pCenter); // ours; the layer kept its own
        }
        else
        {
            LogLine("  destroy %p: no Gr2D centre to hand the origin back to", pLayer);
        }
        _layer_put_color(pLayer, nullptr, kLayerHidden);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    ReleaseComPtr(pLayer);
}

// Hands the skill's own icon canvas to the layer: Animate(layer, retbuf, canvas, v1..v5).
// The five variants are the SKILL ICON recipe, not empty ones: the CTemporaryStatView entry tail
// (0x007B3176, reached from its nType == 2 branch) passes VT_I4 500, VT_I4 210, VT_I4 64, empty,
// empty. The all-empty shape belongs to the drag ghost at 0x00800214, and a layer animated that way
// stays blank -- which is exactly how this module first came out invisible.
static bool AnimateIcon(void* pLayer, void* pCanvas)
{
    if (pLayer == nullptr || pCanvas == nullptr)
    {
        return false;
    }

    unsigned char aRetBuf[16];
    memset(aRetBuf, 0, sizeof(aRetBuf));

    unsigned char aVariants[5][16];
    MakeIntVariant(aVariants[0], 500);
    MakeIntVariant(aVariants[1], 210);
    MakeIntVariant(aVariants[2], 64);
    CopyEmptyVariant(aVariants[3]);
    CopyEmptyVariant(aVariants[4]);

    __try
    {
        _layer_animate(pLayer, aRetBuf, pCanvas,
            aVariants[0], aVariants[1], aVariants[2], aVariants[3], aVariants[4]);
        return true;
    }
    __except (LogSehFilter(GetExceptionInformation()))
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

static void DestroyAllLayers()
{
    if (g_pParentLayer != nullptr)
    {
        LogLine("  teardown: window layer %p refs=%u", g_pParentLayer, ProbeRefCount(g_pParentLayer));
    }

    for (int nPet = 0; nPet < kPetCount; ++nPet)
    {
        for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
        {
            DestroySlotLayer(g_apLayers[nPet][nSlot]);
            g_apLayers[nPet][nSlot] = nullptr;
            g_anDrawn[nPet][nSlot] = 0;
        }
    }

    if (g_pParentLayer != nullptr)
    {
        LogLine("  teardown: window layer refs after=%u", ProbeRefCount(g_pParentLayer));
    }

    g_pParentLayer = nullptr;
}

// Puts every configured slot on the window and (re)paints the ones whose skill changed.
// bVerbose traces every step; it is only set on the drop path so mouse-move churn stays quiet.
static void RefreshLayers(bool bVerbose)
{
    if (g_pWindow == nullptr)
    {
        return;
    }

    void* pParentLayer = GetWindowLayer(g_pWindow);
    if (pParentLayer == nullptr)
    {
        return;
    }

    // The window rebuilds its layer if it is re-created behind our back; a layer we hung on the old
    // one would be orphaned, so start over rather than move a layer that no longer follows.
    if (g_pParentLayer != pParentLayer)
    {
        if (bVerbose)
        {
            LogLine("  window layer %p -> %p: rebuilding", g_pParentLayer, pParentLayer);
        }
        DestroyAllLayers();
        g_pParentLayer = pParentLayer;
    }

    int nPet = -1;
    __try
    {
        nPet = *reinterpret_cast<int*>(reinterpret_cast<char*>(g_pWindow) + OFF_CUIPetEquip_Tab);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }

    if (bVerbose)
    {
        LogLine("  refresh win=%p parent=%p tab=%d", g_pWindow, pParentLayer, nPet);
    }

    if (nPet < 0 || nPet >= kPetCount)
    {
        return;
    }

    for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
    {
        const int nSkillId = g_anSlots[nPet][nSlot];
        void* pLayer = g_apLayers[nPet][nSlot];

        if (bVerbose)
        {
            LogLine("  slot %d: skill=%d layer=%p drawn=%d", nSlot, nSkillId, pLayer, g_anDrawn[nPet][nSlot]);
        }

        if (nSkillId == 0)
        {
            if (pLayer != nullptr)
            {
                SetLayerVisible(pLayer, false);
            }
            g_anDrawn[nPet][nSlot] = 0;
            continue;
        }

        bool bCreated = false;
        if (pLayer == nullptr)
        {
            pLayer = CreateSlotLayer(pParentLayer, bVerbose);
            g_apLayers[nPet][nSlot] = pLayer;
            g_anDrawn[nPet][nSlot] = 0;
            if (pLayer == nullptr)
            {
                LogLine("  layer create failed pet=%d slot=%d", nPet, nSlot);
                continue;
            }

            bCreated = true;
        }

        if (g_anDrawn[nPet][nSlot] != nSkillId)
        {
            void* pCanvas = GetSkillIconCanvas(nSkillId);
            if (bVerbose)
            {
                LogLine("  icon canvas=%p for skill=%d", pCanvas, nSkillId);
                DumpCanvas("  icon", pCanvas);
            }
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

        if (bCreated)
        {
            if (bVerbose) LogLine("  slot %d created", nSlot);
        }

        // Re-bind the layer on EVERY refresh, the way BuffTimer::BindLabelLayer re-binds its labels
        // each tick. Moving the window re-hangs the window's own layer (CWndMan::UpdateWindowPosition
        // 0x009E03A6 -> RemoveWindow, plus the LT/origin juggling in the layout pass sub_7FFB17), and
        // whatever that invalidates is restored here.
        //
        // raw_RelMove is ABSOLUTE and, once the layer has its canvas, it sets the layer's left-top
        // directly: measured with a canvas attached, asking for y = 76 reported 76; issued before
        // Animate (a 0x0 layer with no canvas) the same call came back 32 short, because the engine
        // then treats the value as the canvas's bottom edge. Hence: move after Animate, ask for the
        // cell's own top. Repeated calls are idempotent, so re-issuing costs nothing.
        SetLayerOrigin(pLayer, pParentLayer);
        MoveLayer(pLayer, kSlotRects[nSlot].nLeft, kSlotRects[nSlot].nTop);

        // PutZ re-inserts the layer at the end of its depth group, so this is also what keeps the
        // icon above the window's own canvas after the window re-fronts itself.
        LayerPutZ(pLayer, kSlotLayerZ);
        SetLayerVisible(pLayer, true);

        LogRefreshState(pParentLayer, pLayer);

        if (bVerbose)
        {
            DumpLayer("  painted", pLayer);
        }
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
        RefreshLayers(true);

        // Return 0 even though the slot was taken: a non-zero result tells CWndMan::EndDragDrop
        // (0x009E37C2) that the target ACCEPTED the drop, and it then runs the drop-into-the-world
        // path -- `if (v31) sub_8D63EC(CUIStatusBar::ms_pInstance, x, y, dragCtx + 4)` -- which
        // leaves the UI unresponsive and unclosable for a skill. The original returns 0 whenever
        // the skill is not one it handles, so 0 is the ordinary "nobody took it" outcome.
        return 0;
    }
    __except (LogSehFilter(GetExceptionInformation()))
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
        // CUIPetEquip::OnMouseMove is called with the window's SECOND base (`obj + 4`), the same
        // pointer CWndMan hands out as a drop target -- the destroy log caught it: `this=2422787C
        // tracked=24227880`. The tab field, the layer field and CWnd::Destroy all live on the CWnd
        // base, so the pointer is normalised the same way the drop path does it. Skipping the
        // normalisation made every mouse move re-hang the icon on whatever sat at obj + 0x1C and
        // left CWnd::Destroy unable to recognise the window at all.
        void* pObject = nullptr;
        if (IsPetEquipWindow(pThis, &pObject))
        {
            g_pWindow = pObject;
            RefreshLayers(false);
        }
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }

    return nResult;
}

// The pet equip window is created and destroyed on every toggle, so its layers must not outlive it.
static void __fastcall Destroy_Hook(void* pThis, void* /*edx*/)
{
    __try
    {
        if (g_pWindow != nullptr)
        {
            LogLine("  window destroy: this=%p tracked=%p%s", pThis, g_pWindow,
                (pThis == g_pWindow || pThis == reinterpret_cast<char*>(g_pWindow) + 4) ? " (ours)" : "");
        }

        if (g_pWindow != nullptr &&
            (pThis == g_pWindow || pThis == reinterpret_cast<char*>(g_pWindow) + 4))
        {
            DestroyAllLayers();
            g_pWindow = nullptr;
        }
    }
    __except (LogSehFilter(GetExceptionInformation()))
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

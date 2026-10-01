#include "stdafx.h"
#include "PetSkillSlot.h"
#include "Memory.h"
#include "PetBuffWhitelist.h"
#include "PetBuffConfig.h"      // configuration changes are shipped to the server here
#include <stdio.h>
#include <stdarg.h>
#include <oleauto.h> // VARIANTARG (the WzGr2D / WzCanvas wrappers take Ztl_variant_t by value)
#include <comdef.h>

// ===== Reverse-engineering anchors (Angel.exe / BeiDou.exe, v83; bookmarks prefixed "PETBUFF:") ====
//
// --- the cells -------------------------------------------------------------
//   the window's rect table (0x00BE2260, interleaved x/y, 50 entries) registers, at y = 44, only
//   x = 13 (item pouch) and x = 46 (meso magnet). Cells #3/#4 (x = 79 and x = 112) are painted by
//   the window background but registered nowhere: sub_8011FA answers 0 there and a click falls
//   through. Those two are this feature's slots (32x32, pitch 33, verified in game).
//
// --- where the drop lands --------------------------------------------------
//   CDraggableSkill::OnDropped  0x004FAA22  (pFrom, pTo, x, y) -> int, __thiscall
//     the skill id sits at *(this + 6); the drop coordinates are WINDOW-LOCAL, the same frame the
//     rect table and sub_8011FA use.
//   CWndMan::EndDragDrop        0x009E37C2  calls the draggable's vtable+4 (OnDropped) directly --
//     the target window's HitTest is NOT consulted -- and treats a NON-ZERO result as "the target
//     accepted the drop", forwarding it to the status bar's drop handler
//     (`if (v31) sub_8D63EC(CUIStatusBar::ms_pInstance, x, y, dragCtx + 4)`, the drop-into-the-world
//     path). For a skill that leaves the whole UI unresponsive and unclosable, so this hook stores
//     the slot and returns 0.
//
// --- the window, and which pointer is which ---------------------------------
//   ctor sub_7FE299 writes *obj = off_B38A70 (primary vtable), obj[1] = off_B38A24, obj[2] =
//   off_B38A20, then CWnd::CreateWnd(id = 0xB1, w = 177, h = 181, z = 10). Drag targets and
//   CUIPetEquip::OnMouseMove (0x00800F7B) both receive `obj + 4`; CWnd::Destroy (0x009E00AF)
//   receives `obj`. The tab field (this+360), the layer field (this+0x18) and the tooltip
//   (this+108) all live on the CWnd base, so both hooks normalise with IsPetEquipWindow.
//
// --- drawing the icon: the window's own canvas, exactly like the potion icons ---
//   CUIPetEquip::Draw (0x00801474) is the window's draw override. It calls CWnd::Draw (the window
//   background) and then draws every registered cell into the window's canvas:
//       CWnd::GetCanvas(this, &canvas)                                  0x00425C4C
//       sub_5D6458(ctx, itemId, rect.x, rect.y + 32, iconCanvas, ...)  per cell
//       ... and the two auto-potion icons explicitly at (46, 43) and (112, 43)
//   (the +32 and the 43 are the cell's BOTTOM edge: those helpers position by the canvas's bottom
//   left, the same asymmetry IWzVector2D::raw_RelMove shows).
//
//   The primitive underneath is IWzCanvas vtable+128 -- draw a canvas at (x, y) -- used by
//   CWnd::Draw 0x009E0502 for the window background, where (x, y) is the window's own origin, i.e.
//   the LEFT-TOP. So the slot icons are drawn the same way, into the window's canvas, at the cell's
//   left-top.
//
//   This is deliberately the client's own mechanism rather than a layer of our own: the window's
//   tooltip is painted after the window's Draw, so it covers a cell icon for free, and the window
//   moving, closing or switching pet tabs repaints the canvas and carries the icons along with no
//   extra hooks. A separate Gr2D layer cannot reproduce that: the tooltip is not a layer of its own
//   (nothing in 0x8E49B5..0x8F6000 creates one), so any depth that keeps a layer above the window's
//   canvas keeps it above the tooltip too.
//
// --- the slot bottom: a green rounded rect, in the client's own paint calls ----------------------
//   CUIPetEquip::Draw washes a cell it cannot use with IWzCanvas vtable +140, "fill a rectangle":
//       (*(*canvas + 140))(canvas, x, y, 32, 32, 0x40FF0000)   at (46, 11) / (112, 11) / (tbl, tbl)
//   i.e. (left, top, width, height, 0xAARRGGBB) -- 0x40FF0000 is the semi-transparent RED of that
//   wash, which is what makes the argument order readable. CUIToolTip::MakeLayer uses the same call
//   for its whole background and for its 1x1 corner pixels, so a 1px-tall fill is a supported shape;
//   the rounded corners below are cut by drawing the rect one scanline at a time (never overlapping,
//   or a translucent colour would double-blend along the seams).
//
//   Text comes from IWzCanvas::DrawTextA (0x004277AD, vtable +0x98) with a font from
//   get_basic_font (0x0098A707, global cache spFontBasic[56]). Both that wrapper and the width
//   helper IWzFont (0x0042782E, vtable +0x1C) CONSUME the bstr they are handed -- each releases it on
//   the way out -- so measuring and drawing need one bstr each. A bstr is made with the client's own
//   _bstr_t(wchar_t const*) (0x00403382) into a 4-byte slot, exactly as CUIMessenger::DrawTextA and
//   CAvatarMegaphone::Draw do (the latter centres with `78 - (width >> 1)`, the same arithmetic).
//   Both colour arguments are left empty: the client never passes anything but an empty Ztl_variant_t
//   there (0x00BF6300), and the colour comes from the font itself.
//
// --- the skill tooltip -----------------------------------------------------
//   the window's embedded CUIToolTip sits at obj + 108 (its ctor: CUIToolTip::CUIToolTip(a1 + 27)).
//   CTemporaryStatView::ShowToolTip 0x007B2FD5 shows a buff icon's tooltip with exactly:
//       ClearToolTip(0x008E6E23)
//       SetToolTip_Skill(tooltip, x, y, CSkillInfo::GetSkill(id), 0)     0x008F25D0
//   with x/y in SCREEN pixels (the tooltip is positioned absolutely), which is why this module asks
//   CWndMan::GetCursorPos (0x009E311B) instead of passing the window-local mouse position.
//
// =================================================================================================

static const DWORD ADDR_DraggableSkill_OnDropped = 0x004FAA22;
static const DWORD ADDR_CUIPetEquip_OnMouseButton = 0x00800214;
static const DWORD ADDR_CUIPetEquip_Draw = 0x00801474;
static const DWORD ADDR_CUIPetEquip_OnMouseMove = 0x00800F7B;
static const DWORD ADDR_CUIPetEquip_HitTest = 0x008011FA;
static const DWORD ADDR_CWnd_GetCanvas = 0x00425C4C;
static const DWORD ADDR_CWnd_InvalidateRect = 0x009E04C9;
static const DWORD ADDR_CSkillInfo_GetSkill = 0x0075C755;
static const DWORD ADDR_SkillInfoInstance = 0x00BE78DC;
static const DWORD ADDR_CWndMan_Instance = 0x00BEC20C;
static const DWORD ADDR_CWndMan_GetCursorPos = 0x009E311B;
static const DWORD ADDR_CInputSystem_SetCursorState = 0x0059A6D9;
static const DWORD ADDR_CInputSystem_Instance = 0x00BEC33C;
static const DWORD ADDR_CUIToolTip_ClearToolTip = 0x008E6E23;
static const DWORD ADDR_CUIToolTip_SetToolTip_Skill = 0x008F25D0;
static const DWORD ADDR_CWvsContext_OnLeaveGame = 0x00A041FF;
static const DWORD ADDR_EmptyVariant = 0x00BF6300;

// Picking a slot up again uses the client's own draggable, so the drag ghost, the cursor and the
// drop handling are all its. CDraggableSkill's vtable is 0x00B39810 (OnDropped sits at +4, the slot
// EndDragDrop calls), and CUIMacroSys::OnMouseButton (0x008B9488) builds one exactly like this:
//   ZAllocEx<ZAllocAnonSelector>::Alloc(0x2C)         0x00403065 on the static at 0x00BF0B00
//   CDraggable base ctor(sourceWindow)                0x006FFDA3
//   [+0x18] = skill id, [+0x1C] = 1                    OnDropped reads exactly these two
//   [0x00]  = the vtable
//   CWndMan::BeginDragDrop(source, draggable)         0x009E353D, two arguments
static const DWORD ADDR_CDraggableSkill_Vtbl = 0x00B39810;
static const DWORD ADDR_CDraggable_BaseCtor = 0x006FFDA3;
static const DWORD ADDR_ZAllocEx_Alloc = 0x00403065;
static const DWORD ADDR_ZAllocEx_s_alloc = 0x00BF0B00;
static const DWORD ADDR_CWndMan_BeginDragDrop = 0x009E353D;
static const DWORD ADDR_IWzGr2D_CreateLayer = 0x00426C7E;
static const DWORD ADDR_IWzGr2D_GetCenter = 0x004374CB;
static const DWORD ADDR_IWzGr2DLayer_Animate = 0x00426BAB;
static const DWORD ADDR_IWzGr2DLayer_Putcolor = 0x0045144A;
static const DWORD ADDR_IWzGr2DLayer_GetWidth = 0x00440C00;
static const DWORD ADDR_IWzGr2DLayer_GetHeight = 0x00440C2A;
static const DWORD ADDR_IWzGr2DLayer_GetZ = 0x0044337D;
static const DWORD ADDR_Gr2DInstance = 0x00BF14EC;
static const int nVtbl_IWzVector2D__put_origin = 100;
static const int nVtbl_IWzVector2D__raw_RelMove = 144;

// The drag ghost is drawn at the cursor, semi-transparent -- the alpha the client's own item ghost
// uses (Putcolor(0x80FFFFFF) in 0x00800214).
static const unsigned long kGhostColor = 0x80FFFFFFu;
static const int nGhostOffsetX = -16;
static const int nGhostOffsetY = -16;

// The depth the client pushes its drag ghost to (sub_61738F(layer, 0x7FFFFFFD) inside BeginDragDrop).
static const int nGhostLayerZ = 0x7FFFFFFD;
static const int nCDraggableSkill_Size = 0x2C;
static const int OFF_CDraggableSkill_SkillId = 0x18;
static const int OFF_CDraggableSkill_Validated = 0x1C;

static const unsigned int nMsgLButtonDown = 513; // WM_LBUTTONDOWN

// The cursor state the window's own OnMouseMove sets over a cell that holds something -- the grab
// hand. Its own default is 0, which is what it leaves over our cells (it knows nothing about them).
static const long nCursorGrab = 5;

static const DWORD VTBL_CUIPetEquip_Primary = 0x00B38A70; // *(void**)obj
static const DWORD VTBL_CUIPetEquip_Second = 0x00B38A24;  // *(void**)(obj + 4)

static const int OFF_DraggableSkill_SkillId = 6 * 4;    // *(this + 6)
static const int OFF_CUIPetEquip_Tab = 360 * 4;         // *(this + 360), 0..2
static const int OFF_CUIPetEquip_ToolTip = 108;         // the window's embedded CUIToolTip
static const int OFF_SKILLENTRY_Icon = 0xAC;            // *(SKILLENTRY + 0xAC) = 32x32 canvas

// IWzCanvas vtable+128 = "draw this canvas at (x, y)", the call CWnd::Draw makes for a background.
static const int nVtbl_IWzCanvas__DrawCanvas = 128;

// IWzCanvas vtable+140 = "fill (x, y, w, h) with 0xAARRGGBB" -- CUIPetEquip::Draw's red wash and
// CUIToolTip::MakeLayer's background frame (see the header block).
static const int nVtbl_IWzCanvas__DrawRect = 140;

static const DWORD ADDR_IWzCanvas_DrawTextA = 0x004277AD;  // IWzCanvas::DrawTextA, vtable +0x98
static const DWORD ADDR_IWzFont_GetTextWidth = 0x0042782E; // IWzFont width, vtable +0x1C
static const DWORD ADDR_get_basic_font = 0x0098A707;       // get_basic_font(FONT_TYPE)
static const DWORD ADDR_bstr_t_CtorW = 0x00403382;         // _bstr_t::_bstr_t(wchar_t const*)
static const int nFontTypeCount = 56;                      // get_basic_font's switch covers 0..55

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

// The bottom each of those two cells gets: a green rounded rect, plus one label over the pair. The
// alpha and the "tint the cell" idea mirror the client's own red wash (0x40FF0000) at the same rects.
static const unsigned long kSlotBackColor = 0x8000C000u;
static const int nSlotRadius = 6;

// Corner insets for a 6px radius: scanline i of the top edge is pulled in by
// radius - round(sqrt(radius^2 - dy^2)), dy = radius - 1 - i. The bottom edge mirrors it.
static const int kSlotRadiusInset[nSlotRadius] = { 3, 2, 1, 0, 0, 0 };

// The label, spelled in universal character names so the source file's encoding cannot change it.
static const wchar_t kWszSlotLabel[] = L"\u81EA\u52A8\u6280\u80FD";
static const int nSlotLabelOffsetY = 9;      // label top, from the cell top: 12px text centres in 32
static const int nSlotLabelMaxWidth = 200;   // sanity bound on the measured width (see DrawSlotLabel)

static const int kPetCount = 3;
static const int kSlotCount = 2;

// The remembered configuration. Client-side only, lost when the client closes.
static int g_anSlots[kPetCount][kSlotCount] = {};

// The slot the drag in flight was started from, so a drop that lands anywhere else takes the skill
// out of it. Reset on every mouse-down and after every drop.
static int g_nDragFromPet = -1;
static int g_nDragFromSlot = -1;
static void* g_pDragFromWindow = nullptr;

bool PetSkillSlot::bEnabled = true;
bool PetSkillSlot::bDebug = true;
int PetSkillSlot::nLabelFont = 0;

typedef void* (__thiscall* CWndGetCanvas_t)(void* pWnd, void* pRetBuf);
typedef void (__thiscall* CWndInvalidateRect_t)(void* pWnd, const void* pRect);
typedef long(__stdcall* CanvasDrawCanvas_t)(void* pCanvas, long nX, long nY, void* pSrcCanvas,
    VARIANTARG vAttr);
typedef long(__stdcall* CanvasDrawRect_t)(void* pCanvas, long nX, long nY, long nWidth, long nHeight,
    unsigned long nColor);
// The text argument of the two wrappers below is a bstr SLOT (a Ztl_bstr_t: one dword holding the
// client's refcounted string object), passed as its address; both consume it. Declaring it as void*
// keeps our temp a bare 4-byte slot instead of a C++ object the compiler would copy and destroy.
typedef long(__thiscall* CanvasDrawTextA_t)(void* pCanvas, long nX, long nY, void* pBstrSlot,
    void* pFont, const VARIANTARG* pColor, const VARIANTARG* pTabOrg);
typedef long(__thiscall* FontTextWidth_t)(void* pFont, void* pBstrSlot, const VARIANTARG* pVar);
typedef void* (__cdecl* GetBasicFont_t)(void* pRetSlot, int nFontType);
typedef void* (__thiscall* BstrCtorW_t)(void* pThis, const wchar_t* pszText);
typedef void (__thiscall* CWndManGetCursorPos_t)(void* pWndMan, POINT* pOut, int nFlag);
typedef void (__thiscall* CInputSystemSetCursorState_t)(void* pInputSystem, long nState);
typedef int(__fastcall* CWvsContextOnLeaveGame_t)(void* pThis, void* edx);
typedef void (__thiscall* ToolTipClear_t)(void* pToolTip);
typedef void (__fastcall* ToolTipSetSkill_t)(void* pToolTip, void* edx, int nX, int nY,
    void* pSkillEntry, int nFlag);
typedef int(__fastcall* PetEquipHitTest_t)(void* pWindow, void* edx, int nX, int nY);
typedef int(__thiscall* PetEquipDraw_t)(void* pThis, const void* pRect);
typedef void (__thiscall* PetEquipOnMouseButton_t)(void* pThis, unsigned int nMsg, unsigned int nParam,
    long nX, long nY);
typedef void* (__thiscall* ZAllocExAlloc_t)(void* pThis, unsigned int nSize);
typedef void (__thiscall* CDraggableBaseCtor_t)(void* pThis, void* pSourceWindow);
typedef void (__thiscall* CWndManBeginDragDrop_t)(void* pWndMan, void* pSource, void* pDraggable);
typedef void* (__fastcall* Gr2DCreateLayer_t)(void* pGr2D, void* edx, void** ppLayer, int nX, int nY,
    unsigned long nWidth, unsigned long nHeight, int nZ, const void* pV1, const void* pV2);
typedef void* (__fastcall* Gr2DGetCenter_t)(void* pGr2D, void* edx, void** ppCenter);
typedef long(__stdcall* VectorPutOrigin_t)(void* pVector, VARIANTARG vOrigin);
typedef long(__stdcall* VectorRawRelMove_t)(void* pVector, long nX, long nY, VARIANTARG v1, VARIANTARG v2);
typedef void (__fastcall* LayerPutColor_t)(void* pLayer, void* edx, unsigned long nColor);
typedef void* (__thiscall* LayerAnimate_t)(void* pLayer, void* pRetBuf, void* pCanvas,
    const void* pV1, const void* pV2, const void* pV3, const void* pV4, const void* pV5);
typedef int(__fastcall* LayerGetInt_t)(void* pLayer, void* edx);
typedef int(__thiscall* PetEquipOnMouseMove_t)(void* pThis, int nX, int nY);
typedef void* (__thiscall* SkillInfoGetSkill_t)(void* pSkillInfo, int nSkillId);
typedef int(__thiscall* DraggableSkillOnDropped_t)(void* pThis, void* pFrom, void* pTo, int nX, int nY);

static auto _wnd_get_canvas = reinterpret_cast<CWndGetCanvas_t>(ADDR_CWnd_GetCanvas);
static auto _wnd_invalidate = reinterpret_cast<CWndInvalidateRect_t>(ADDR_CWnd_InvalidateRect);
static auto _wndman_get_cursor_pos = reinterpret_cast<CWndManGetCursorPos_t>(ADDR_CWndMan_GetCursorPos);
static auto _input_set_cursor_state = reinterpret_cast<CInputSystemSetCursorState_t>(ADDR_CInputSystem_SetCursorState);
static auto _tooltip_clear = reinterpret_cast<ToolTipClear_t>(ADDR_CUIToolTip_ClearToolTip);
static auto _tooltip_set_skill = reinterpret_cast<ToolTipSetSkill_t>(ADDR_CUIToolTip_SetToolTip_Skill);
static auto _hit_test = reinterpret_cast<PetEquipHitTest_t>(ADDR_CUIPetEquip_HitTest);
static auto _skill_get = reinterpret_cast<SkillInfoGetSkill_t>(ADDR_CSkillInfo_GetSkill);
static auto _create_layer = reinterpret_cast<Gr2DCreateLayer_t>(ADDR_IWzGr2D_CreateLayer);
static auto _gr2d_get_center = reinterpret_cast<Gr2DGetCenter_t>(ADDR_IWzGr2D_GetCenter);
static auto _layer_animate = reinterpret_cast<LayerAnimate_t>(ADDR_IWzGr2DLayer_Animate);
static auto _layer_put_color = reinterpret_cast<LayerPutColor_t>(ADDR_IWzGr2DLayer_Putcolor);
static auto _layer_get_width = reinterpret_cast<LayerGetInt_t>(ADDR_IWzGr2DLayer_GetWidth);
static auto _layer_get_height = reinterpret_cast<LayerGetInt_t>(ADDR_IWzGr2DLayer_GetHeight);
static auto _layer_get_z = reinterpret_cast<LayerGetInt_t>(ADDR_IWzGr2DLayer_GetZ);

static DraggableSkillOnDropped_t g_origOnDropped = nullptr;
static PetEquipDraw_t g_origDraw = nullptr;
static PetEquipOnMouseMove_t g_origOnMouseMove = nullptr;
static PetEquipOnMouseButton_t g_origOnMouseButton = nullptr;
static CWvsContextOnLeaveGame_t g_origOnLeaveGame = nullptr;

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
            reinterpret_cast<unsigned long(__stdcall*)(void*)>(pVtbl[2])(pUnknown); // IUnknown::Release
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// True when pTo is the pet equip window; *ppObject receives the CWnd base the offsets are relative
// to. Drag targets and CUIPetEquip::OnMouseMove hand out `obj + 4`; CWnd::Destroy hands out `obj`.
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

        if (pVtbl == reinterpret_cast<void*>(VTBL_CUIPetEquip_Primary))
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

// CSkillInfo::GetSkill(id), the lookup both the icon and the tooltip go through.
static void* GetSkillEntry(int nSkillId)
{
    void* pInfo = *reinterpret_cast<void**>(ADDR_SkillInfoInstance);
    if (pInfo == nullptr || nSkillId <= 0)
    {
        return nullptr;
    }

    __try
    {
        return _skill_get(pInfo, nSkillId);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// The skill's own 32x32 icon canvas, the same field the client's buff row paints
// (CTemporaryStatView's entry ctor takes the nType == 2 branch straight to *(SKILLENTRY + 0xAC)).
static void* GetSkillIconCanvas(int nSkillId)
{
    void* pEntry = GetSkillEntry(nSkillId);
    if (pEntry == nullptr)
    {
        return nullptr;
    }

    __try
    {
        return *reinterpret_cast<void**>(reinterpret_cast<char*>(pEntry) + OFF_SKILLENTRY_Icon);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// The slot whose cell contains (nX, nY), or -1. Window-local pixels, the frame the drop coordinates
// and the window's own rect table use.
static int FindSlotAt(int nX, int nY)
{
    for (int i = 0; i < kSlotCount; ++i)
    {
        const SlotRect& r = kSlotRects[i];
        if (nX >= r.nLeft && nX <= r.nRight && nY >= r.nTop && nY <= r.nBottom)
        {
            return i;
        }
    }

    return -1;
}

static int GetTab(void* pWindow)
{
    int nPet = -1;
    __try
    {
        nPet = *reinterpret_cast<int*>(reinterpret_cast<char*>(pWindow) + OFF_CUIPetEquip_Tab);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return nPet;
}

// ---------------------------------------------------------------------------------------------
// Picking a slot up: the client's own CDraggableSkill, so the ghost, the cursor and the drop
// handling all come from the client
// ---------------------------------------------------------------------------------------------

// put_origin hands the layer its own reference to the new origin; raw_RelMove is absolute and, once
// the layer has a canvas, sets the canvas's BOTTOM-left (measured: asking for y = 44 reported 12 on
// a canvas-less layer, 76 for 76 on one that had its canvas).
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
        reinterpret_cast<VectorPutOrigin_t>(pVtbl[nVtbl_IWzVector2D__put_origin / sizeof(void*)])(pLayer, vOrigin);
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

    // Both variants must be spelled out: calling it with x/y alone leaves 32 bytes of arguments
    // missing and the callee pops its own count, which corrupts the stack (QuestBulb.cpp).
    VARIANTARG v1;
    VARIANTARG v2;
    memcpy(&v1, reinterpret_cast<const void*>(ADDR_EmptyVariant), sizeof(v1));
    memcpy(&v2, reinterpret_cast<const void*>(ADDR_EmptyVariant), sizeof(v2));

    __try
    {
        reinterpret_cast<VectorRawRelMove_t>(pVtbl[nVtbl_IWzVector2D__raw_RelMove / sizeof(void*)])(pLayer, nX, nY, v1, v2);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

static void ForgetDragSource()
{
    g_nDragFromPet = -1;
    g_nDragFromSlot = -1;
    g_pDragFromWindow = nullptr;
}

// Takes the skill out of the slot a drag started from, and repaints that window. Called when the
// drop lands anywhere other than the cell it came from -- which is how the client's own equipment
// slots are emptied.
static void ClearDragSourceSlot()
{
    const int nPet = g_nDragFromPet;
    const int nSlot = g_nDragFromSlot;
    void* pWindow = g_pDragFromWindow;
    ForgetDragSource();

    if (nPet < 0 || nPet >= kPetCount || nSlot < 0 || nSlot >= kSlotCount || pWindow == nullptr)
    {
        return;
    }

    g_anSlots[nPet][nSlot] = 0;
    LogLine("  removed pet=%d slot=%d", nPet, nSlot);
    PetBuffConfig_Send();                   // whole-configuration save; see PetBuffConfig.h

    __try
    {
        _wnd_invalidate(pWindow, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// The drag ghost: a layer holding the skill's icon canvas. BeginDragDrop gives it its origin and
// pushes it to the front itself (put_origin on CWndMan's org window, then sub_61738F with
// 0x7FFFFFFD), so the draggable only has to hand the layer over at +0xC.
static void* CreateGhostLayer(void* pIconCanvas, void* pWindow, int nLocalX, int nLocalY)
{
    if (pIconCanvas == nullptr)
    {
        return nullptr;
    }

    void* pGr2D = *reinterpret_cast<void**>(ADDR_Gr2DInstance);
    if (pGr2D == nullptr || IsBadReadPtr(pGr2D, sizeof(void*)))
    {
        return nullptr;
    }

    // CreateLayer's first variant is a VT_I4 canvas id, the second a copy of the empty variant.
    VARIANTARG vCanvasId;
    memset(&vCanvasId, 0, sizeof(vCanvasId));
    vCanvasId.vt = VT_I4;
    vCanvasId.lVal = 0;

    unsigned char aEmpty[16];
    memcpy(aEmpty, reinterpret_cast<const void*>(ADDR_EmptyVariant), 16);

    void* pLayer = nullptr;
    _create_layer(pGr2D, nullptr, &pLayer, 0, 0, 0, 0, nGhostLayerZ, &vCanvasId, aEmpty);
    if (pLayer == nullptr)
    {
        return nullptr;
    }

    // Animate's five variants are the skill-icon recipe (0x007B3176's tail), not the drag ghost's
    // all-empty shape -- that one belongs to a static item image and leaves a canvas blank.
    VARIANTARG aAttr[5];
    memset(aAttr, 0, sizeof(aAttr));
    aAttr[0].vt = VT_I4;
    aAttr[0].lVal = 500;
    aAttr[1].vt = VT_I4;
    aAttr[1].lVal = 210;
    aAttr[2].vt = VT_I4;
    aAttr[2].lVal = 64;
    memcpy(&aAttr[3], aEmpty, sizeof(VARIANTARG));
    memcpy(&aAttr[4], aEmpty, sizeof(VARIANTARG));

    unsigned char aRetBuf[16];
    memset(aRetBuf, 0, sizeof(aRetBuf));
    _layer_animate(pLayer, aRetBuf, pIconCanvas, &aAttr[0], &aAttr[1], &aAttr[2], &aAttr[3], &aAttr[4]);

    // CreateLayer leaves the layer colour at 0, which is fully transparent.
    _layer_put_color(pLayer, nullptr, kGhostColor);

    // Hang it on the window's own layer and put it under the cursor IN WINDOW-LOCAL PIXELS.
    // BeginDragDrop then re-origins the ghost onto the UI root window and converts its position with
    // `Getx(ghost) - Getx(rootWindow)`, i.e. it carries a ghost that was positioned in the window's
    // frame over to the screen frame. Skipping this step leaves the ghost at the layer's (0,0) and
    // the per-frame follow only tracks the mouse DELTA, so it stays a fixed distance away.
    void* pWindowLayer = *reinterpret_cast<void**>(reinterpret_cast<char*>(pWindow) + 0x18);
    if (pWindowLayer != nullptr)
    {
        SetLayerOrigin(pLayer, pWindowLayer);
        MoveLayer(pLayer, nLocalX + nGhostOffsetX, nLocalY + nGhostOffsetY);
    }

    LogLine("  ghost layer: w=%d h=%d z=0x%08X",
        _layer_get_width(pLayer, nullptr), _layer_get_height(pLayer, nullptr),
        static_cast<unsigned>(_layer_get_z(pLayer, nullptr)));
    return pLayer;
}

// Hands the client a CDraggableSkill carrying this slot's skill and starts a drag with it.
// pSource is the window pointer the client itself uses for a drag (`obj + 4`, the one OnDropped and
// EndDragDrop see); pWindow is the CWnd base, which is what CWnd::InvalidateRect and the tab field
// need. The draggable's +0xC is the ghost layer, NOT the window -- BeginDragDrop treats that field
// as an IWzShape2D and calls put_origin / raw_RelMove on it.
static bool StartSlotDrag(void* pSource, void* pWindow, int nPet, int nSlot, int nLocalX, int nLocalY)
{
    const int nSkillId = g_anSlots[nPet][nSlot];
    if (nSkillId == 0)
    {
        return false;
    }

    void* pGhost = nullptr;

    __try
    {
        pGhost = CreateGhostLayer(GetSkillIconCanvas(nSkillId), pWindow, nLocalX, nLocalY);
        LogLine("  drag: ghost=%p", pGhost);
        if (pGhost == nullptr)
        {
            return false;
        }

        void* pDraggable = reinterpret_cast<ZAllocExAlloc_t>(ADDR_ZAllocEx_Alloc)(
            reinterpret_cast<void*>(ADDR_ZAllocEx_s_alloc), nCDraggableSkill_Size);
        LogLine("  drag: alloc=%p", pDraggable);
        if (pDraggable == nullptr)
        {
            ReleaseComPtr(pGhost);
            return false;
        }

        memset(pDraggable, 0, nCDraggableSkill_Size);
        reinterpret_cast<CDraggableBaseCtor_t>(ADDR_CDraggable_BaseCtor)(pDraggable, pGhost);
        LogLine("  drag: base ctor done, p=%p", pDraggable);

        *reinterpret_cast<int*>(reinterpret_cast<char*>(pDraggable) + OFF_CDraggableSkill_SkillId) = nSkillId;
        // 1 skips OnDropped's "is this skill learned?" gate, which is the check the client's own
        // drags from the skill window satisfy differently (they drag a skill the player has).
        *reinterpret_cast<int*>(reinterpret_cast<char*>(pDraggable) + OFF_CDraggableSkill_Validated) = 1;
        *reinterpret_cast<void**>(pDraggable) = reinterpret_cast<void*>(ADDR_CDraggableSkill_Vtbl);
        LogLine("  drag: fields set");

        // The base ctor took its own reference on the ghost; ours is handed back here, leaving the
        // draggable the only owner.
        ReleaseComPtr(pGhost);
        pGhost = nullptr;

        reinterpret_cast<CWndManBeginDragDrop_t>(ADDR_CWndMan_BeginDragDrop)(
            *reinterpret_cast<void**>(ADDR_CWndMan_Instance), pSource, pDraggable);
        LogLine("  drag: begun");
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
        if (pGhost != nullptr)
        {
            ReleaseComPtr(pGhost);
        }
        return false;
    }

    g_nDragFromPet = nPet;
    g_nDragFromSlot = nSlot;
    // The base, not the interface: this is handed to CWnd::InvalidateRect when the drop removes the
    // skill, and the interface pointer leaves the window un-repainted (the icon then stays on screen
    // until the window is recreated).
    g_pDragFromWindow = pWindow;
    LogLine("  drag start pet=%d slot=%d skill=%d", nPet, nSlot, nSkillId);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Drawing: the window's own canvas, the way CUIPetEquip::Draw paints its cells
// ---------------------------------------------------------------------------------------------

// One cell's bottom: a rounded rect, drawn as one 1px scanline per row so that two fills never
// overlap (a translucent colour would double-blend where they did).
static void DrawRoundedRect(void* pCanvas, void** pVtbl, const SlotRect& rect, unsigned long nColor)
{
    auto drawRect = reinterpret_cast<CanvasDrawRect_t>(pVtbl[nVtbl_IWzCanvas__DrawRect / sizeof(void*)]);
    const int nWidth = rect.nRight - rect.nLeft;
    const int nHeight = rect.nBottom - rect.nTop;

    for (int i = 0; i < nHeight; ++i)
    {
        int nInset = 0;
        if (i < nSlotRadius)
        {
            nInset = kSlotRadiusInset[i];
        }
        else if (i >= nHeight - nSlotRadius)
        {
            nInset = kSlotRadiusInset[nHeight - 1 - i];
        }

        drawRect(pCanvas, rect.nLeft + nInset, rect.nTop + i, nWidth - 2 * nInset, 1, nColor);
    }
}

// The label, centred over the pair of cells. Its font is whichever FONT_TYPE the configuration
// names; get_basic_font caches, but hands out a counted reference that has to be released.
static void DrawSlotLabelRaw(void* pCanvas, void* pFont)
{
    VARIANTARG vEmpty;
    memset(&vEmpty, 0, sizeof(vEmpty));
    memcpy(&vEmpty, reinterpret_cast<const void*>(ADDR_EmptyVariant), sizeof(vEmpty));

    // Measuring and drawing each consume their bstr, so each gets its own. The slot is one pointer
    // wide: that is all a Ztl_bstr_t holds, and the client's own callers keep it in a 4-byte local.
    void* aProbe[1] = {};
    reinterpret_cast<BstrCtorW_t>(ADDR_bstr_t_CtorW)(aProbe, kWszSlotLabel);
    long nWidth = reinterpret_cast<FontTextWidth_t>(ADDR_IWzFont_GetTextWidth)(pFont, aProbe, &vEmpty);
    if (nWidth < 0 || nWidth > nSlotLabelMaxWidth)
    {
        LogLine("  draw: label width %d out of range, drawing it from the centre", nWidth);
        nWidth = 0;
    }

    void* aText[1] = {};
    reinterpret_cast<BstrCtorW_t>(ADDR_bstr_t_CtorW)(aText, kWszSlotLabel);

    const int nCenter = (kSlotRects[0].nLeft + kSlotRects[kSlotCount - 1].nRight) / 2;
    reinterpret_cast<CanvasDrawTextA_t>(ADDR_IWzCanvas_DrawTextA)(pCanvas,
        nCenter - nWidth / 2, kSlotRects[0].nTop + nSlotLabelOffsetY, aText, pFont, &vEmpty, &vEmpty);
}

// The drawing half, kept apart from the SEH guard because SEH (__try/__except) and C++ EH
// (try/catch) cannot share a function. Both are needed: the client's Gr2D wrappers raise _com_error,
// and an exception escaping a hook unwinds into the client's frames, which have no handler for it --
// that ends the process through abort()/__fastfail, with no crash dump at all (an access violation at
// least gets caught here and logged). BuffTimer documents the same split.
static void DrawSlotsRaw(void* pWindow, int nPet)
{
    try
    {
        unsigned char aCanvasPtr[16];
        memset(aCanvasPtr, 0, sizeof(aCanvasPtr));
        _wnd_get_canvas(pWindow, aCanvasPtr); // com_ptr<IWzCanvas> by value

        void* pCanvas = *reinterpret_cast<void**>(aCanvasPtr);
        if (pCanvas == nullptr)
        {
            return;
        }

        void** pVtbl = *reinterpret_cast<void***>(pCanvas);
        if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzCanvas__DrawRect + sizeof(void*)))
        {
            ReleaseComPtr(pCanvas);
            return;
        }

        for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
        {
            DrawRoundedRect(pCanvas, pVtbl, kSlotRects[nSlot], kSlotBackColor);
        }

        int nFontType = PetSkillSlot::nLabelFont;
        if (nFontType < 0 || nFontType >= nFontTypeCount)
        {
            LogLine("  draw: FONT_TYPE=%d out of range, using 0", PetSkillSlot::nLabelFont);
            nFontType = 0;
        }

        void* aFontPtr[1] = {};
        reinterpret_cast<GetBasicFont_t>(ADDR_get_basic_font)(aFontPtr, nFontType);
        void* pFont = aFontPtr[0];
        if (pFont != nullptr)
        {
            DrawSlotLabelRaw(pCanvas, pFont);
            ReleaseComPtr(pFont);
        }
        else
        {
            LogLine("  draw: no font for FONT_TYPE=%d, label skipped", nFontType);
        }

        // DrawCanvas takes its attribute as a Ztl_variant_t BY VALUE, so it has to be a real
        // VARIANTARG rather than the byte buffer the by-reference wrappers use.
        VARIANTARG vAttr;
        memset(&vAttr, 0, sizeof(vAttr));
        memcpy(&vAttr, reinterpret_cast<const void*>(ADDR_EmptyVariant), sizeof(vAttr));

        for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
        {
            const int nSkillId = g_anSlots[nPet][nSlot];
            if (nSkillId == 0)
            {
                continue;
            }

            void* pIcon = GetSkillIconCanvas(nSkillId);
            if (pIcon == nullptr)
            {
                LogLine("  draw: no icon canvas for skill=%d", nSkillId);
                continue;
            }

            reinterpret_cast<CanvasDrawCanvas_t>(pVtbl[nVtbl_IWzCanvas__DrawCanvas / sizeof(void*)])(
                pCanvas, kSlotRects[nSlot].nLeft, kSlotRects[nSlot].nTop, pIcon, vAttr);
        }

        ReleaseComPtr(pCanvas);
    }
    catch (...)
    {
        LogLine("  draw: the canvas call raised, skipped");
    }
}

// Paints the two cells into the pet equip window's canvas: the bottom and the label always (they are
// the empty-slot affordance), then whatever icon the current tab holds on top of them. Runs after the
// window's own Draw, i.e. after its background and its own cell icons and before the tooltip is
// painted over the window.
static void DrawSlots(void* pWindow)
{
    if (pWindow == nullptr)
    {
        return;
    }

    const int nPet = GetTab(pWindow);
    if (nPet < 0 || nPet >= kPetCount)
    {
        return;
    }

    __try
    {
        DrawSlotsRaw(pWindow, nPet);
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

// The window's draw override: let it paint the background and its own cells, then add ours. Every
// repaint -- opening the window, switching pet tabs, moving it, a tooltip coming and going -- comes
// back through here, so the icons are never stale and never need a layer of their own.
static int __fastcall Draw_Hook(void* pThis, void* /*edx*/, const void* pRect)
{
    int nResult = 0;
    if (g_origDraw != nullptr)
    {
        nResult = g_origDraw(pThis, pRect);
    }

    __try
    {
        void* pObject = nullptr;
        if (IsPetEquipWindow(pThis, &pObject))
        {
            DrawSlots(pObject);
        }
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }

    return nResult;
}

static int __fastcall OnDropped_Hook(void* pThis, void* /*edx*/, void* pFrom, void* pTo, int nX, int nY)
{
    int nResult = 0;
    if (g_origOnDropped != nullptr)
    {
        nResult = g_origOnDropped(pThis, pFrom, pTo, nX, nY);
    }

    // The client already consumed the drop (quickslot, macro, skill window, ...). If the drag came
    // out of one of our slots the skill has moved there, so that slot is empty now.
    if (nResult != 0)
    {
        ClearDragSourceSlot();
        return nResult;
    }

    void* pObject = nullptr;
    if (!IsPetEquipWindow(pTo, &pObject))
    {
        // Dropped somewhere other than the slot cells: taking a skill out of a slot is done by
        // dragging it out, exactly like emptying an equipment slot.
        ClearDragSourceSlot();
        return 0;
    }

    __try
    {
        const int nSkillId = *reinterpret_cast<int*>(reinterpret_cast<char*>(pThis) + OFF_DraggableSkill_SkillId);
        const int nPet = GetTab(pObject);

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

        // The cell the cursor is over, if any. A release anywhere else inside the window counts as
        // taking the skill out, the same as releasing outside it -- there is no snapping back to the
        // nearest cell, which used to put the skill back when the drop landed on empty window space.
        const int nSlot = FindSlotAt(nX, nY);
        if (nSlot < 0)
        {
            LogLine("  dropped off the cells: removing");
            ClearDragSourceSlot();
            return 0;
        }

        // A skill may only occupy one of the six cells. Placing it again moves it: the later
        // placement wins and the cell that held it is cleared, across pet tabs as well.
        for (int nOtherPet = 0; nOtherPet < kPetCount; ++nOtherPet)
        {
            for (int nOtherSlot = 0; nOtherSlot < kSlotCount; ++nOtherSlot)
            {
                if ((nOtherPet != nPet || nOtherSlot != nSlot) &&
                    g_anSlots[nOtherPet][nOtherSlot] == nSkillId)
                {
                    g_anSlots[nOtherPet][nOtherSlot] = 0;
                    LogLine("  moved out of pet=%d slot=%d", nOtherPet, nOtherSlot);
                }
            }
        }

        g_anSlots[nPet][nSlot] = nSkillId;
        LogLine("  stored pet=%d slot=%d skill=%d", nPet, nSlot, nSkillId);
        PetBuffConfig_Send();               // whole-configuration save; see PetBuffConfig.h

        // Dragging a slot onto another slot is the same case as dropping the same skill again: the
        // dedup pass above has already emptied the cell it came from, so only the bookkeeping is
        // left to drop.
        ForgetDragSource();

        // The drop itself does not repaint the window, so without this the icon only appears on the
        // next repaint something else happens to trigger (hovering a cell and getting its tooltip,
        // for instance). The client marks its own windows dirty the same way -- CWnd::CreateWnd ends
        // with CWnd::InvalidateRect(this, 0).
        _wnd_invalidate(pObject, nullptr);

        // Return 0 even though the slot was taken: a non-zero result tells CWndMan::EndDragDrop that
        // the target ACCEPTED the drop, and it then runs the drop-into-the-world path, which leaves
        // the UI unresponsive and unclosable for a skill. The original returns 0 whenever the skill
        // is not one it handles, so 0 is the ordinary "nobody took it" outcome.
        return 0;
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
        LogLine("  exception while reading the drop");
        ForgetDragSource();
        return 0;
    }
}

// Pressing a slot picks its skill up with the client's own draggable. The window's own handler runs
// first and handles its registered cells (the potion cells start their own drags there), so this
// only fires for the two cells the client knows nothing about.
static void __fastcall OnMouseButton_Hook(void* pThis, void* /*edx*/, unsigned int nMsg,
    unsigned int nParam, long nX, long nY)
{
    if (g_origOnMouseButton != nullptr)
    {
        g_origOnMouseButton(pThis, nMsg, nParam, nX, nY);
    }

    __try
    {
        void* pObject = nullptr;
        if (!IsPetEquipWindow(pThis, &pObject))
        {
            return;
        }

        if (nMsg != nMsgLButtonDown)
        {
            return;
        }

        // A fresh press: any earlier drag never completed, so drop its bookkeeping.
        ForgetDragSource();

        const int nPet = GetTab(pObject);
        const int nSlot = FindSlotAt(nX, nY);

        // Logged for every press so a drag that never starts can be told apart from a press that
        // never reaches this window (a tooltip sitting under the cursor does exactly that).
        LogLine("  press msg=%u x=%d y=%d tab=%d slot=%d", nMsg, nX, nY, nPet, nSlot);

        if (nPet < 0 || nPet >= kPetCount || nSlot < 0)
        {
            return;
        }

        StartSlotDrag(pThis, pObject, nPet, nSlot, nX, nY);
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }
}

// The slot cells carry no id in the window's own hit test (it answers 0 there), so their tooltip is
// ours to show -- the same two calls, in the same order, as the client's own buff icons. Screen
// pixels: the tooltip positions itself absolutely.
static void ShowSlotToolTip(void* pObject, int nX, int nY, bool bOnClientCell)
{
    const int nPet = GetTab(pObject);
    const int nSlot = FindSlotAt(nX, nY);

    void* pEntry = nullptr;
    if (nPet >= 0 && nPet < kPetCount && nSlot >= 0)
    {
        const int nSkillId = g_anSlots[nPet][nSlot];
        if (nSkillId != 0)
        {
            pEntry = GetSkillEntry(nSkillId);
        }
    }

    __try
    {
        void* pToolTip = reinterpret_cast<char*>(pObject) + OFF_CUIPetEquip_ToolTip;
        if (pEntry == nullptr)
        {
            // The window's own OnMouseMove has already run: only clear when it had nothing to show
            // either, so its tooltip is left alone.
            if (!bOnClientCell)
            {
                _tooltip_clear(pToolTip);
            }
            return;
        }

        POINT ptCursor;
        ptCursor.x = 0;
        ptCursor.y = 0;
        _wndman_get_cursor_pos(*reinterpret_cast<void**>(ADDR_CWndMan_Instance), &ptCursor, 0);

        // The window's own OnMouseMove has just set the cursor back to its default for a cell it
        // does not know, so the grab hand has to be re-applied here -- same state value it uses.
        _input_set_cursor_state(*reinterpret_cast<void**>(ADDR_CInputSystem_Instance), nCursorGrab);

        _tooltip_clear(pToolTip);
        // The client's own cell tooltips sit 20px below the cell's top edge (`window.y + cell.y + 20`
        // in CUIPetEquip::OnMouseMove), which also keeps the tooltip out from under the cursor --
        // with it centred on the cursor the press landed on the tooltip window instead of the cell.
        _tooltip_set_skill(pToolTip, nullptr, ptCursor.x, ptCursor.y + 20, pEntry, 0);
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }
}

static int __fastcall OnMouseMove_Hook(void* pThis, void* /*edx*/, int nX, int nY)
{
    int nResult = 0;
    if (g_origOnMouseMove != nullptr)
    {
        nResult = g_origOnMouseMove(pThis, nX, nY);
    }

    __try
    {
        void* pObject = nullptr;
        if (IsPetEquipWindow(pThis, &pObject))
        {
            // Non-zero means the window is showing one of its own cell tooltips; that one is not ours
            // to clear.
            bool bOnClientCell = false;
            __try
            {
                bOnClientCell = _hit_test(pObject, nullptr, nX, nY) != 0;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }

            ShowSlotToolTip(pObject, nX, nY, bOnClientCell);
        }
    }
    __except (LogSehFilter(GetExceptionInformation()))
    {
    }

    return nResult;
}

// Leaving the game is also what a character switch does, and the client keeps running for the next
// character, so the configuration has to go with it: the slots belong to the character that set
// them. Same hook point BuffTimer uses to drop its buff labels on the way out.
static int __fastcall OnLeaveGame_Hook(void* pThis, void* edx)
{
    for (int nPet = 0; nPet < kPetCount; ++nPet)
    {
        for (int nSlot = 0; nSlot < kSlotCount; ++nSlot)
        {
            g_anSlots[nPet][nSlot] = 0;
        }
    }

    ForgetDragSource();
    LogLine("  left the game: slots cleared");

    if (g_origOnLeaveGame != nullptr)
    {
        return g_origOnLeaveGame(pThis, edx);
    }

    return 0;
}

int PetSkillSlot::GetSkill(int nPet, int nSlot)
{
    if (nPet < 0 || nPet >= kPetCount || nSlot < 0 || nSlot >= kSlotCount)
    {
        return 0;
    }

    return g_anSlots[nPet][nSlot];
}

void PetSkillSlot::SetSkill(int nPet, int nSlot, int nSkillId)
{
    if (nPet < 0 || nPet >= kPetCount || nSlot < 0 || nSlot >= kSlotCount)
    {
        return;
    }

    g_anSlots[nPet][nSlot] = nSkillId;      // server-side values; the caller does not re-send them
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

    if (g_origDraw == nullptr)
    {
        g_origDraw = reinterpret_cast<PetEquipDraw_t>(ADDR_CUIPetEquip_Draw);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origDraw),
            reinterpret_cast<void*>(&Draw_Hook)))
        {
            g_origDraw = nullptr;
            std::cout << "pet auto-buff slots: Draw hook FAILED" << std::endl;
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

    if (g_origOnMouseButton == nullptr)
    {
        g_origOnMouseButton = reinterpret_cast<PetEquipOnMouseButton_t>(ADDR_CUIPetEquip_OnMouseButton);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnMouseButton),
            reinterpret_cast<void*>(&OnMouseButton_Hook)))
        {
            g_origOnMouseButton = nullptr;
            std::cout << "pet auto-buff slots: OnMouseButton hook FAILED" << std::endl;
        }
    }

    if (g_origOnLeaveGame == nullptr)
    {
        g_origOnLeaveGame = reinterpret_cast<CWvsContextOnLeaveGame_t>(ADDR_CWvsContext_OnLeaveGame);
        if (!Memory::SetHook(true, reinterpret_cast<void**>(&g_origOnLeaveGame),
            reinterpret_cast<void*>(&OnLeaveGame_Hook)))
        {
            g_origOnLeaveGame = nullptr;
            std::cout << "pet auto-buff slots: OnLeaveGame hook FAILED" << std::endl;
        }
    }

    std::cout << "pet auto-buff slots hook created (CDraggableSkill::OnDropped)" << std::endl;
    LogLine("--- pet auto-buff slots armed (build 0x%08X) ---", ADDR_DraggableSkill_OnDropped);
}

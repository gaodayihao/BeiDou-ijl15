#include "stdafx.h"
#include "BuffTimer.h"
#include <comutil.h> // _bstr_t/_com_ptr_t; comdef.h carries the _com_error body the hooks throw
#include <comdef.h>  // every Gr2D wrapper reports a failed HRESULT by throwing a _com_error
#include <oleauto.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ===== Reverse-engineering anchors (Angel.exe / BeiDou.exe, v83) =====
//
// --- where the label goes -------------------------------------------------
// The buff row at the top right of the HUD is CTemporaryStatView, a member of CWvsContext at
// +0x2EA8. Each 32x32 icon is a TEMPORARY_STAT entry inside a ZList<ZRef<TEMPORARY_STAT>>:
//
//   ZList base                          view+0x04   size view+0x0C | head view+0x10 | tail view+0x14
//   ZList::FindIndex                    0x007B4D1D  (ZList*, unsigned int) -> node, *(node+4) = entry
//   CTemporaryStatView::AdjustPosition   0x007B2BB0  places entry+0x28 (icon) and entry+0x2C
//                                                    (overlay) on exactly the same pixel
//   CTemporaryStatView::SetTemporary     0x007B24D5  adds one entry (entry ctor 0x007B3176)
//   CTemporaryStatView::ShowToolTip      0x007B2E58  reads the same entry fields this file does
//
//   entry layout: +0x1C nType (1 = item buff, 2 = skill buff, 3/4 = family/guild)
//                 +0x20 nID   (negative for item buffs), +0x24 tooltip string
//                 +0x28 icon IWzGr2DLayer, +0x2C overlay IWzGr2DLayer
//
// entry+0x2C is the client's own "digit over the icon" layer: a plain buff never draws into it
// (0x007B44F4, the skill-cooldown digit, only starts once the value it is fed changes), so it is
// an empty, client-positioned, client-owned surface sitting under the icon. 0x007B44F4 also shows
// the client's own way of replacing the number - it drops the dirty canvas with
// IWzGr2DLayer::RemoveCanvas before handing the layer a new canvas, which is what stops a shorter
// number from leaving the tail of a longer one behind. This file does the same, with the text
// calls BossHP already proved out.
//
// --- where the remaining time comes from ---------------------------------
// CTemporaryStatView does not keep the buff duration, so it is read off the GIVE_BUFF packet while
// CWvsContext::OnTemporaryStatSet (0x00A202BE) still has it unread:
//
//   CInPacket::DecodeBuffer(mask, 16)     0x00432257  the 128-bit stat mask
//   per set bit: Decode2 value            0x0042470C
//                Decode4 buffid           0x00406629
//                Decode4 bufflength       0x00406629
//
// byte for byte what PacketCreator.giveBuff writes on this server (mask = two longs, then
// short value / int buffid / int bufflength per stat) and what the client's own decoder sub_781D0E
// reads - it takes timeGetTime() once and stores now+bufflength as that stat's expiry, so the
// duration is in milliseconds on both sides (StatEffect scales skill `time` seconds and item times
// up to ms). The cursor at CInPacket+0x14 is saved and put back, so the client parses the packet
// exactly as if this hook were not there.
//
// --- colour ---------------------------------------------------------------
// get_basic_font (0x0098A707) has 56 FONT_TYPE slots; the call that builds each one carries a literal
// ARGB colour (and size/typeface), so the slot, not the draw call, picks the colour. Slots this
// feature defaults to: 7 = 0xFF28C99B green, 3 = 0xFFFFFF20 yellow, 1 = 0xFF000000 black. Other
// greens/yellows in the same table: 13 = 0xFF336600, 52 = 0xFF629A00, 9 = 0xFFFF9900 (orange),
// 27 = 0xFFFFFF20. Slots 43..49 are the only 9px ones (Tahoma) but come in white/black/red/cream.

const DWORD dwCWvsContext__OnTemporaryStatSet = 0x00A202BE;
const DWORD dwCWvsContext__Update = 0x00A03350;
const DWORD dwCInPacket__DecodeBuffer = 0x00432257;
const DWORD dwCInPacket__Decode2 = 0x0042470C;
const DWORD dwCInPacket__Decode4 = 0x00406629;
const DWORD dwZList__FindIndex = 0x007B4D1D;
const DWORD dwGetBasicFont = 0x0098A707;
// The chain get_basic_font uses to build a single spFontBasic slot, reused here because that fixed
// table cannot give the label what it wants: its slots are 9/11/12/15 px, the one 15 px slot is
// white, and its only green is the dark 0xFF28C99B (see GetLabelFont).
const DWORD dwStringPool__GetInstance = 0x0079E805;
const DWORD dwStringPool__GetStringW = 0x00406276;
const DWORD dwStringPool__GetBSTR = 0x00406292;
const DWORD dwPcCreateObject__IWzFont = 0x00463670;
const DWORD dwIWzFont__Init = 0x0046341A;
const DWORD dwZXStringW__Release = 0x004033A7;
// The two StringPool entries get_basic_font feeds the factory and the font itself.
const unsigned int nFontClassNameId = 1410;
const unsigned int nFontFaceId = 5527;
const DWORD dwBstrCtor = 0x00406301; // _bstr_t::_bstr_t(char const*)
const DWORD dwIWzFont__CalcTextWidth = 0x0042782E;
const DWORD dwIWzCanvas__DrawTextA = 0x004277AD;
// IWzCanvas::Create(int cx, int cy, const Ztl_variant_t&, const Ztl_variant_t&) - re-allocates the
// canvas blank. The client calls it on canvases it keeps in members (CField_LimitedView::Init at
// 0x0055BC6C and sub_537FDA at 0x005384AA both pass 316x316), so it is the client's own way of
// wiping a canvas it is about to draw into again.
const DWORD dwIWzCanvas__Create = 0x0048ECA7;
const DWORD dwIWzGr2DLayer__GetCanvas = 0x00425D2E;
// IWzGr2DLayer::RemoveCanvas(VARIANT) - not used: it drops the layer's canvas, and a layer whose
// canvas was swapped out that way stops rendering the label altogether. Kept on record because it is
// the call the client's own buff-icon countdown (0x007B44F4) reaches for.
const DWORD dwIWzGr2DLayer__RemoveCanvas = 0x004143FB;
const DWORD dwIWzGr2DLayer__GetWidth = 0x00440C00;
const DWORD dwIWzGr2DLayer__GetHeight = 0x00440C2A;
// IWzGr2D::GetCenter() - the vector every CTemporaryStatView layer takes as its origin (the entry
// ctor at 0x007B3176 calls put_origin with it right after CreateLayer).
const DWORD dwIWzGr2D__GetCenter = 0x004374CB;
const DWORD dwIWzGr2D__CreateLayer = 0x00426C7E;
const DWORD dwIWzGr2DLayer__GetZ = 0x0044337D;
const DWORD dwIWzGr2DLayer__GetAlpha = 0x004143C6;
const DWORD dwIWzGr2DLayer__Putcolor = 0x0045144A;
const DWORD dwIWzCanvas__Getcx = 0x0040F09B;
const DWORD dwIWzCanvas__Getcy = 0x0040F0C2;

// dword_BF14EC - the IWzGr2D every UI layer in the client is built from (CUIToolTip::MakeLayer and
// the CTemporaryStatView entry ctor both take it from here).
const DWORD dwGr2DInstance = 0x00BF14EC;

// IWzVector2D vtable slots, confirmed in this exe and already used by QuestBulb.cpp.
const int nVtbl_IWzVector2D__get_x = 32;
const int nVtbl_IWzVector2D__get_y = 40;
const int nVtbl_IWzVector2D__put_origin = 100;
const int nVtbl_IWzVector2D__raw_RelMove = 144;

// IWzGr2DLayer::PutZ; Putcolor sits at +224 right after it, which pins the slot.
const int nVtbl_IWzGr2DLayer__PutZ = 180;

// The label layer must be a normal-sized layer: CTemporaryStatView's entry ctor creates its own
// layers with width = height = 0 (CreateLayer(0, 0, 0, 0, 0xC006156C, ...)), and a zero-sized layer
// has no canvas of its own - GetCanvas hands every such layer the same shared canvas, which is why
// one label used to show up on all buff icons at once.
const int nLabelLayerSize = 32;
const long nLabelLayerZ = static_cast<long>(0xC006156C); // the depth the icon layers themselves use

// The client's global empty Ztl_variant_t, the 5th/6th argument of DrawTextA. File-local: BossHP.cpp
// has a pointer of its own to the same address and its one carries external linkage.
static const void* g_pEmptyVariant = reinterpret_cast<const void*>(0x00BF6300);

const int nCWvsContext__TemporaryStatView = 0x2EA8;
const int nView__ListBase = 0x04;
const int nView__ListSize = 0x0C;
const int nTempStat__Type = 0x1C;
const int nTempStat__Id = 0x20;
const int nTempStat__Icon = 0x28;
const int nTempStat__Overlay = 0x2C;
const int nCInPacket__Data = 0x08;
const int nCInPacket__Cursor = 0x14;

const unsigned int nHourMs = 3600000;
const unsigned int nMinuteMs = 60000;

const int nTempStatType__Item = 1;
const int nTempStatType__Skill = 2;

// A 32x32 icon holds at most a couple of dozen buffs; anything past that is a mis-read, not a buff.
const int nMaxTracked = 96;
const int nMaxIcons = 96;

// The FONT_TYPE slots this feature defaults to are 12px, which is what the bounds clamp assumes.
const int nAssumedGlyphHeight = 12;

// White glyphs with a black outline: the colour lives in the font slot, so the outline is the same
// string drawn in the black slot at the 8 neighbouring pixels.
static const int aTextOutlineOffset[8][2] = {
	{ -1, -1 }, { 0, -1 }, { 1, -1 },
	{ -1,  0 },            { 1,  0 },
	{ -1,  1 }, { 0,  1 }, { 1,  1 },
};

struct TRACKED_BUFF
{
	unsigned int dwId;
	unsigned int dwExpire;
};

// One label layer per buff icon. The layer is ours (created with a real size so it owns a canvas),
// its origin is the icon layer, and its position is (0,0), so the text lands on the icon itself.
struct LABEL_LAYER
{
	void* pLayer;
	void* pEntry;
	char sText[8];
	bool bSeen;
};

static TRACKED_BUFF aTracked[nMaxTracked];
static int nTrackedNext = 0;
static LABEL_LAYER aLabels[nMaxIcons];
static unsigned int dwLastTick = 0;
static unsigned int dwCaptureCount = 0;

// Every icon is logged until this runs out, so one short session is enough to diagnose a mismatch.
static int nIconLogBudget = 300;

bool BuffTimer::bEnabled = true;
int BuffTimer::nMinuteFont = 7;
int BuffTimer::nSecondFont = 3;
int BuffTimer::nOutlineFont = 1;
int BuffTimer::nFontSize = 14;
int BuffTimer::nMaxMinutes = 10;
int BuffTimer::nMinuteColor = 0x5AFFAA;
int BuffTimer::nSecondColor = 0xFFFF20;
int BuffTimer::nOutlineColor = 0x000000;
int BuffTimer::nOffsetX = 4;
int BuffTimer::nOffsetY = 17;
bool BuffTimer::bDebug = false;

typedef void* (__fastcall* LayerGetCanvas_t)(void* pLayer, void* edx, void** ppCanvas, const void* pVariant);
typedef int (__fastcall* LayerGetSize_t)(void* pLayer, void* edx);
typedef void* (__fastcall* Gr2DCreateLayer_t)(void* pGr2D, void* edx, void** ppLayer, int nX, int nY, unsigned long nWidth, unsigned long nHeight, int nZ, const void* pV1, const void* pV2);
typedef void* (__fastcall* Gr2DGetCenter_t)(void* pGr2D, void* edx, void** ppCenter);
typedef long(__stdcall* VectorPutOrigin_t)(void* pVector, VARIANTARG vOrigin);
typedef long(__stdcall* VectorRawRelMove_t)(void* pVector, long nX, long nY, VARIANTARG vAttr1, VARIANTARG vAttr2);
typedef long(__stdcall* VectorGetLong_t)(void* pVector, long* pnOut);
typedef long(__stdcall* LayerPutZ_t)(void* pLayer, int nZ);
typedef int (__fastcall* LayerGetZ_t)(void* pLayer, void* edx);
typedef void (__fastcall* LayerPutColor_t)(void* pLayer, void* edx, unsigned long nColor);
typedef int (__fastcall* CanvasGetSize_t)(void* pCanvas, void* edx);
// IWzCanvas::Create(cx, cy, VARIANT, VARIANT): the two variants travel as pointers to the callee's
// own copies, matching the tlh wrapper the client itself calls (0x0048ECA7).
typedef long(__fastcall* CanvasCreate_t)(void* pCanvas, void* edx, int nCx, int nCy, const void* pV1, const void* pV2);
typedef void* (__fastcall* ZListFindIndex_t)(void* pList, void* edx, unsigned int nIndex);
typedef void* (__fastcall* BstrCtor_t)(void* pBstr, void* edx, const char* sText);
typedef int (__fastcall* FontCalcTextWidth_t)(void* pFont, void* edx, void* pBstrData, const void* pVariant);
typedef unsigned int (__fastcall* CanvasDrawTextA_t)(void* pCanvas, void* edx, int nLeft, int nTop, void* pBstrData, void* pFont, const void* pV1, const void* pV2);
typedef void (__fastcall* PacketDecodeBuffer_t)(void* pPacket, void* edx, void* pOut, unsigned int nSize);
typedef unsigned short (__fastcall* PacketDecode2_t)(void* pPacket, void* edx);
typedef unsigned int (__fastcall* PacketDecode4_t)(void* pPacket, void* edx);
typedef void* (__cdecl* GetBasicFont_t)(void** pOut, int nType);
typedef void* (__cdecl* StringPoolGetInstance_t)(void);
typedef void* (__fastcall* StringPoolGetString_t)(void* pPool, void* edx, void** ppOut, unsigned int nId);
typedef int (__cdecl* PcCreateFont_t)(const void* pName, void** ppOut, int pUnkOuter);
typedef long(__fastcall* FontInit_t)(void* pFont, void* edx, void* pBstr, int nSize, unsigned int nColor, const void* pVariant);
typedef void (__cdecl* ZXStringWRelease_t)(void* pData);

static auto _layer_get_canvas = reinterpret_cast<LayerGetCanvas_t>(dwIWzGr2DLayer__GetCanvas);
static auto _layer_get_width = reinterpret_cast<LayerGetSize_t>(dwIWzGr2DLayer__GetWidth);
static auto _layer_get_height = reinterpret_cast<LayerGetSize_t>(dwIWzGr2DLayer__GetHeight);
static auto _create_layer = reinterpret_cast<Gr2DCreateLayer_t>(dwIWzGr2D__CreateLayer);
static auto _gr2d_get_center = reinterpret_cast<Gr2DGetCenter_t>(dwIWzGr2D__GetCenter);
static auto _layer_get_z = reinterpret_cast<LayerGetZ_t>(dwIWzGr2DLayer__GetZ);
static auto _layer_put_color = reinterpret_cast<LayerPutColor_t>(dwIWzGr2DLayer__Putcolor);
static auto _canvas_get_cx = reinterpret_cast<CanvasGetSize_t>(dwIWzCanvas__Getcx);
static auto _canvas_get_cy = reinterpret_cast<CanvasGetSize_t>(dwIWzCanvas__Getcy);
static auto _zlist_find_index = reinterpret_cast<ZListFindIndex_t>(dwZList__FindIndex);
static auto _bstr_ctor = reinterpret_cast<BstrCtor_t>(dwBstrCtor);
static auto _font_calc_text_width = reinterpret_cast<FontCalcTextWidth_t>(dwIWzFont__CalcTextWidth);
static auto _canvas_draw_text = reinterpret_cast<CanvasDrawTextA_t>(dwIWzCanvas__DrawTextA);
static auto _canvas_create = reinterpret_cast<CanvasCreate_t>(dwIWzCanvas__Create);
static auto _packet_decode_buffer = reinterpret_cast<PacketDecodeBuffer_t>(dwCInPacket__DecodeBuffer);
static auto _packet_decode2 = reinterpret_cast<PacketDecode2_t>(dwCInPacket__Decode2);
static auto _packet_decode4 = reinterpret_cast<PacketDecode4_t>(dwCInPacket__Decode4);
static auto _stringpool_get_instance = reinterpret_cast<StringPoolGetInstance_t>(dwStringPool__GetInstance);
static auto _stringpool_get_string_w = reinterpret_cast<StringPoolGetString_t>(dwStringPool__GetStringW);
static auto _stringpool_get_bstr = reinterpret_cast<StringPoolGetString_t>(dwStringPool__GetBSTR);
static auto _pc_create_font = reinterpret_cast<PcCreateFont_t>(dwPcCreateObject__IWzFont);
static auto _font_init = reinterpret_cast<FontInit_t>(dwIWzFont__Init);
static auto _zxstring_w_release = reinterpret_cast<ZXStringWRelease_t>(dwZXStringW__Release);

// Diagnostics only, appended to buff_timer.log. Callers decide when a line is worth writing; the
// first few labels always leave a trail so a fresh test needs no config change to be diagnosable.
void BuffTimer::Log(const char* sFormat, ...) {
	char sLine[512];
	va_list args;
	va_start(args, sFormat);
	_vsnprintf_s(sLine, sizeof(sLine), _TRUNCATE, sFormat, args);
	va_end(args);
	FILE* pFile = nullptr;
	if (fopen_s(&pFile, "buff_timer.log", "a") == 0 && pFile != nullptr) {
		fprintf(pFile, "%s\n", sLine);
		fclose(pFile);
	}
}

// The client's text APIs take a Ztl_bstr_t by value: what travels on the stack is the wrapper's
// inner Data_t pointer, and the callee destroys that reference. Every call therefore gets its own
// freshly built instance and nothing may be released on this side. (Same shape as BossHP.)
static void* MakeBstrData(char* pBstrSlot, const char* sText) {
	_bstr_ctor(pBstrSlot, nullptr, sText);
	return *reinterpret_cast<void**>(pBstrSlot);
}

static int MeasureTextWidth(void* pFont, const char* sText) {
	char aBstr[8] = { 0 };
	void* pData = MakeBstrData(aBstr, sText);
	if (pData == nullptr) return 0;
	return _font_calc_text_width(pFont, nullptr, pData, g_pEmptyVariant);
}

// Returns the width IWzCanvas::DrawTextA reported, so a silent no-op can be told from a real draw.
static unsigned int DrawTextOnePass(void* pCanvas, int nX, int nY, const char* sText, void* pFont) {
	char aBstr[8] = { 0 };
	void* pData = MakeBstrData(aBstr, sText);
	if (pData == nullptr) return 0;
	return _canvas_draw_text(pCanvas, nullptr, nX, nY, pData, pFont, g_pEmptyVariant, g_pEmptyVariant);
}

static void ReleaseComPtr(void* pCanvas) {
	if (pCanvas != nullptr) {
		void** pVtbl = *reinterpret_cast<void***>(pCanvas);
		reinterpret_cast<unsigned long(__stdcall*)(void*)>(pVtbl[2])(pCanvas); // IUnknown::Release
	}
}

void* BuffTimer::GetFont(int nType) {
	if (nType < 0) return nullptr;

	// get_basic_font caches the slot and keeps its own reference, so the pointer stays valid.
	static void* apFonts[64] = { nullptr };
	if (nType >= static_cast<int>(sizeof(apFonts) / sizeof(apFonts[0]))) return nullptr;
	if (apFonts[nType] != nullptr) return apFonts[nType];

	static auto _get_basic_font = reinterpret_cast<GetBasicFont_t>(dwGetBasicFont);
	void* pFont = nullptr;
	_get_basic_font(&pFont, nType);
	apFonts[nType] = pFont;
	return pFont;
}

// Builds one font exactly the way get_basic_font builds a spFontBasic slot, so the label can have a
// size and a colour that fixed table does not offer. The order is the table's: the StringPool name
// goes to the object factory (PcCreateObject::IWzFont), and then the face, point size and ARGB
// colour go to the font itself (IWzFont::Init, raw vtbl+12). Both StringPool ids are the ones the
// table uses, so the face is the one the whole HUD uses. nullptr on any failure - the caller falls
// back to a table slot, and the label keeps working.
void* BuffTimer::CreateFont(int nSize, int nRgb) {
	const unsigned int nColor = 0xFF000000u | (static_cast<unsigned int>(nRgb) & 0xFFFFFFu);
	try {
		void* pPool = _stringpool_get_instance();
		if (pPool == nullptr) return nullptr;

		// ZXString<unsigned short> is a single pointer, so the hidden return slot is one dword.
		void* pClassName = nullptr;
		_stringpool_get_string_w(pPool, nullptr, &pClassName, nFontClassNameId);

		void* pFont = nullptr;
		_pc_create_font(pClassName, &pFont, 0);
		if (pClassName != nullptr) {
			// the string's data pointer sits behind its 12-byte header, which is what _Release wants
			_zxstring_w_release(reinterpret_cast<char*>(pClassName) - 12);
		}
		if (pFont == nullptr) {
			Log("[font] size=%d rgb=0x%06X: no font object", nSize, nRgb & 0xFFFFFF);
			return nullptr;
		}

		// Ztl_bstr_t is a single pointer too, and IWzFont::Init releases the one it is handed, so the
		// slot must not be released here - the same shape as every text call in this file.
		void* pFace = nullptr;
		_stringpool_get_bstr(pPool, nullptr, &pFace, nFontFaceId);

		_font_init(pFont, nullptr, pFace, nSize, nColor, g_pEmptyVariant);

		Log("[font] built size=%d rgb=0x%06X -> %p", nSize, nRgb & 0xFFFFFF, pFont);
		return pFont;
	}
	catch (_com_error& e) {
		Log("[font] size=%d rgb=0x%06X failed hr=0x%08X", nSize, nRgb & 0xFFFFFF,
			static_cast<unsigned int>(e.Error()));
	}
	catch (...) {
		Log("[font] size=%d rgb=0x%06X failed", nSize, nRgb & 0xFFFFFF);
	}
	return nullptr;
}

// The label's three fonts, built once each on first use. Falls back to the spFontBasic slot the
// config names when the font cannot be built, so a failure costs the look, not the feature.
void* BuffTimer::GetLabelFont(int nRole) {
	static void* apFonts[kFontRoleCount] = { nullptr };
	static bool abTried[kFontRoleCount] = { false };
	if (nRole < 0 || nRole >= kFontRoleCount) return nullptr;
	if (abTried[nRole]) return apFonts[nRole];
	abTried[nRole] = true;

	int nSlot = nOutlineFont;
	int nRgb = nOutlineColor;
	if (nRole == kFontMinute) {
		nSlot = nMinuteFont;
		nRgb = nMinuteColor;
	}
	else if (nRole == kFontSecond) {
		nSlot = nSecondFont;
		nRgb = nSecondColor;
	}

	apFonts[nRole] = (nFontSize > 0) ? CreateFont(nFontSize, nRgb) : nullptr;
	if (apFonts[nRole] == nullptr) {
		apFonts[nRole] = GetFont(nSlot);
		if (nFontSize > 0) {
			Log("[font] role %d: custom font unavailable, using slot %d -> %p", nRole, nSlot, apFonts[nRole]);
		}
	}
	return apFonts[nRole];
}

// ----- remaining time -----------------------------------------------------

void BuffTimer::RememberBuff(unsigned int dwId, unsigned int dwDurationMs) {
	if (dwId == 0) return;
	if (dwDurationMs == 0) { // no timer to show at all
		ForgetBuff(dwId);
		return;
	}

	for (int i = 0; i < nMaxTracked; i++) {
		if (aTracked[i].dwId == dwId) {
			aTracked[i].dwExpire = GetTickCount() + dwDurationMs;
			return;
		}
	}

	aTracked[nTrackedNext].dwId = dwId;
	aTracked[nTrackedNext].dwExpire = GetTickCount() + dwDurationMs;
	nTrackedNext = (nTrackedNext + 1) % nMaxTracked;
}

void BuffTimer::ForgetBuff(unsigned int dwId) {
	for (int i = 0; i < nMaxTracked; i++) {
		if (aTracked[i].dwId == dwId) aTracked[i].dwId = 0;
	}
}

int BuffTimer::FindRemainingMs(unsigned int dwId) {
	if (dwId == 0) return 0; // 0 marks a free slot, so it must never be looked up
	unsigned int dwNow = GetTickCount();
	for (int i = 0; i < nMaxTracked; i++) {
		if (aTracked[i].dwId != dwId) continue;
		int nRemaining = static_cast<int>(aTracked[i].dwExpire - dwNow); // wrap-safe
		if (nRemaining <= 0) {
			aTracked[i].dwId = 0; // the server's reset packet carries only a stat mask, so an entry
			return 0;             // is dropped here instead: the icon outlives it either way
		}
		return nRemaining;
	}
	return 0;
}

// Separated from CaptureDurations because SEH (__try/__except) and C++ EH (try/catch) cannot share
// one function: the first guards against a bad pointer, the second against the ZException the
// client's own Decode* raise on a short read. Neither may reach the client's top-level handler.
void BuffTimer::ParseDurations(void* pPacket, int* pnTriplets) {
	try {
		unsigned char aMask[16];
		memset(aMask, 0, sizeof(aMask));
		_packet_decode_buffer(pPacket, nullptr, aMask, sizeof(aMask));

		// Diagnostics: the first few packets are logged whole, because a wrong field order shows up
		// here as values that do not match the skill/item the buff came from. The raw bytes go in
		// too, so the real layout can be read straight off the log even if it is not (2, 4, 4).
		static int nPktCount = 0;
		const bool bLogPkt = bDebug || (++nPktCount <= 20);
		if (bLogPkt) {
			const char* pData = *reinterpret_cast<const char**>(reinterpret_cast<char*>(pPacket) + nCInPacket__Data);
			if (pData != nullptr) {
				unsigned int nCursor = *reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(pPacket) + nCInPacket__Cursor);
				if (!IsBadReadPtr(pData + nCursor, 48)) {
					char sHex[3 * 48 + 1];
					for (int k = 0; k < 48; k++) {
						sprintf_s(sHex + 3 * k, 4, "%02X ", static_cast<unsigned char>(pData[nCursor + k]));
					}
					Log("[pkt %d] raw@%u: %s", nPktCount, nCursor, sHex);
				}
			}
			Log("[pkt %d] mask=%08X %08X %08X %08X", nPktCount,
				*reinterpret_cast<unsigned int*>(aMask + 0),
				*reinterpret_cast<unsigned int*>(aMask + 4),
				*reinterpret_cast<unsigned int*>(aMask + 8),
				*reinterpret_cast<unsigned int*>(aMask + 12));
		}

		// One triplet per set bit; the bits are consumed in the same order the client reads them,
		// but the order does not matter here - id and duration are read together, so every triplet
		// is self-consistent whichever bit it belongs to.
		for (int i = 0; i < 128; i++) {
			if ((aMask[i >> 3] & (1 << (i & 7))) == 0) continue;
			unsigned int dwValue = _packet_decode2(pPacket, nullptr);
			unsigned int dwId = _packet_decode4(pPacket, nullptr);
			unsigned int dwDuration = _packet_decode4(pPacket, nullptr);
			// An item buff travels with a negated source id (StatEffect.applyBuffEffect sends
			// `skill ? sourceid : -sourceid`) while the icon carries the positive one, so both sides
			// have to be compared as magnitudes.
			if ((dwId & 0x80000000u) != 0) dwId = 0u - dwId;
			if (bLogPkt) Log("[pkt %d]   bit=%d value=%u id=%u duration=%u", nPktCount, i, dwValue, dwId, dwDuration);
			RememberBuff(dwId, dwDuration);
			(*pnTriplets)++;
		}
	}
	catch (...) {
		*pnTriplets = -1;
	}
}

// Reads the per-stat triplets straight out of GIVE_BUFF while the client has not consumed them yet.
void BuffTimer::CaptureDurations(void* pPacket) {
	if (pPacket == nullptr || IsBadReadPtr(pPacket, nCInPacket__Cursor + sizeof(int))) return;

	unsigned int dwSavedCursor = *reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(pPacket) + nCInPacket__Cursor);
	int nTriplets = 0;
	__try {
		ParseDurations(pPacket, &nTriplets);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		nTriplets = -1;
	}

	// hand the packet back exactly as it was found, the client parses it next
	*reinterpret_cast<unsigned int*>(reinterpret_cast<char*>(pPacket) + nCInPacket__Cursor) = dwSavedCursor;

	if (bDebug && (++dwCaptureCount <= 5 || (dwCaptureCount % 100) == 0)) {
		Log("[%u] capture triplets=%d cursor=%u", dwCaptureCount, nTriplets, dwSavedCursor);
	}
}

// ----- drawing ------------------------------------------------------------

// ----- label layers -------------------------------------------------------
//
// The overlay layer the client keeps on every buff icon (entry+0x2C) cannot carry the label: the
// entry ctor builds it with width = height = 0, and a zero-sized layer owns no canvas - GetCanvas
// answers with one shared canvas for all of them, which is exactly the "every buff icon shows the
// same number" bug this replaced. So each icon gets a label layer of its own, created the way
// CUIToolTip::MakeLayer creates its layer: a real size, then GetCanvas gives it a real canvas.

void* BuffTimer::CreateLabelLayer() {
	void* pGr2D = *reinterpret_cast<void**>(dwGr2DInstance);
	if (pGr2D == nullptr || IsBadReadPtr(pGr2D, sizeof(void*))) return nullptr;

	// CreateLayer takes both variants by const reference; hand it its own copy of the client's empty
	// variant rather than the global itself.
	unsigned char aEmpty1[16];
	unsigned char aEmpty2[16];
	memcpy(aEmpty1, g_pEmptyVariant, sizeof(aEmpty1));
	memcpy(aEmpty2, g_pEmptyVariant, sizeof(aEmpty2));

	void* pLayer = nullptr;
	try {
		_create_layer(pGr2D, nullptr, &pLayer, 0, 0, nLabelLayerSize, nLabelLayerSize, nLabelLayerZ, aEmpty1, aEmpty2);
	}
	catch (...) {
		Log("[label] CreateLayer failed");
		return nullptr;
	}
	Log("[label] CreateLayer -> %p", pLayer);
	return pLayer;
}

// Runs one binding step and survives its failure. Every Gr2D wrapper here raises a _com_error on a
// failed HRESULT, and letting that escape would leave the layer half-bound with no way to tell which
// call went wrong, so each step reports itself and the binding carries on.
#define BIND_STEP(sName, ...) \
	do { \
		try { \
			Log("[label] bind step: %s", sName); \
			__VA_ARGS__; \
		} \
		catch (...) { \
			Log("[label] bind step %s threw", sName); \
		} \
	} while (0)

// Reads a layer's position as the layer itself resolves it. get_x/get_y answer in the frame the
// layer ends up drawn in, not in the frame its own origin defines, which is why a layer hung under
// another one reports the parent's position and not (0,0).
bool BuffTimer::ReadLayerPos(void* pLayer, long* pnX, long* pnY) {
	void** pVtbl = *reinterpret_cast<void***>(pLayer);
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__get_y + sizeof(void*))) return false;
	try {
		reinterpret_cast<VectorGetLong_t>(pVtbl[nVtbl_IWzVector2D__get_x / sizeof(void*)])(pLayer, pnX);
		reinterpret_cast<VectorGetLong_t>(pVtbl[nVtbl_IWzVector2D__get_y / sizeof(void*)])(pLayer, pnY);
		return true;
	}
	catch (...) {
		return false;
	}
}

// Points a layer's coordinate frame at another vector or layer. put_origin is a property put: it
// hands the layer its own reference to the new origin, so whatever reference the caller used (an
// AddRef of its own, or the one GetCenter returned) stays the caller's to release. Setting a new
// origin also drops the reference to the previous one, which is what ClearLabelLayer relies on.
bool BuffTimer::SetLayerOrigin(void* pLayer, void* pOrigin) {
	void** pVtbl = *reinterpret_cast<void***>(pLayer);
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__put_origin + sizeof(void*))) return false;

	VARIANTARG vOrigin;
	memset(&vOrigin, 0, sizeof(vOrigin));
	vOrigin.vt = VT_UNKNOWN;
	vOrigin.punkVal = reinterpret_cast<IUnknown*>(pOrigin);
	try {
		reinterpret_cast<VectorPutOrigin_t>(pVtbl[nVtbl_IWzVector2D__put_origin / sizeof(void*)])
			(pLayer, vOrigin);
		return true;
	}
	catch (...) {
		Log("[label] layer %p would not take origin %p", pLayer, pOrigin);
		return false;
	}
}

// Hangs the label on the icon: origin = the icon layer, offset (0,0), depth = the icon's. The icon
// layer is only ever *used* here, never kept: no AddRef on this side, because put_origin already
// holds the reference it needs, and the client frees a buff icon by releasing its entry and nothing
// else (CTemporaryStatView::ResetTemporary 0x007B2679 -> sub_7B4BD1; there is no RemoveLayer
// anywhere in the exe). One reference too many and the expired icon stays on screen on top of the
// icons the row has already shifted into its slot. ClearLabelLayer hands the origin back.
void BuffTimer::BindLabelLayer(void* pLayer, void* pIconLayer) {
	void** pVtbl = *reinterpret_cast<void***>(pLayer);
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__raw_RelMove + sizeof(void*))) return;

	BIND_STEP("put_origin", SetLayerOrigin(pLayer, pIconLayer));

	// raw_RelMove needs both VARIANTs spelled out: calling it with x/y alone leaves 32 bytes of
	// garbage on the stack and the callee reads the empty slot as a pointer (see QuestBulb.cpp).
	BIND_STEP("raw_RelMove", [&] {
		VARIANTARG vAttr1;
		VARIANTARG vAttr2;
		memset(&vAttr1, 0, sizeof(vAttr1));
		memset(&vAttr2, 0, sizeof(vAttr2));
		reinterpret_cast<VectorRawRelMove_t>(pVtbl[nVtbl_IWzVector2D__raw_RelMove / sizeof(void*)])
			(pLayer, 0, 0, vAttr1, vAttr2);
	}());

	// CreateLayer's fifth argument is not necessarily the depth (CWnd::CreateWnd sets the depth with
	// PutZ afterwards), so match the icon layer explicitly: same depth, created later, drawn later.
	BIND_STEP("PutZ", reinterpret_cast<LayerPutZ_t>(pVtbl[nVtbl_IWzGr2DLayer__PutZ / sizeof(void*)])
		(pLayer, _layer_get_z(pIconLayer, nullptr)));

	// CUIToolTip::MakeLayer ends with exactly this call, and it is the difference between a layer that
	// renders and one that does not: CreateLayer leaves the layer colour at zero, so without it the
	// label would be drawn onto an invisible layer.
	BIND_STEP("Putcolor", _layer_put_color(pLayer, nullptr, 0xFFFFFFFFu));

	long lIconX = 0, lIconY = 0, lLayerX = 0, lLayerY = 0;
	ReadLayerPos(pIconLayer, &lIconX, &lIconY);
	ReadLayerPos(pLayer, &lLayerX, &lLayerY);
	Log("[label] bind layer=%p icon=%p icon xy=(%d,%d) z=%d | layer xy=(%d,%d) z=%d",
		pLayer, pIconLayer, lIconX, lIconY, _layer_get_z(pIconLayer, nullptr),
		lLayerX, lLayerY, _layer_get_z(pLayer, nullptr));
}

#undef BIND_STEP

// Wipes the layer's canvas so the next digit does not sit on top of the previous one. The canvas is
// the drawing surface, not a buffer to clear, and the client never rubs text out: every path that
// redraws a layer either covers the whole canvas again (CField::ShowMobHPTag) or re-allocates it with
// IWzCanvas::Create and draws from scratch (CField_LimitedView::Init). The layer keeps the same
// canvas object either way, which matters - swapping the object out is what makes a layer stop
// showing anything at all.
bool BuffTimer::EraseCanvas(void* pLayer, bool bLog) {
	try {
		void* pCanvas = nullptr;
		_layer_get_canvas(pLayer, nullptr, &pCanvas, g_pEmptyVariant);
		if (pCanvas == nullptr) return false;

		const int nWidth = _layer_get_width(pLayer, nullptr);
		const int nHeight = _layer_get_height(pLayer, nullptr);
		const long lResult = _canvas_create(pCanvas, nullptr, nWidth, nHeight, g_pEmptyVariant, g_pEmptyVariant);
		if (bLog || lResult < 0) {
			Log("[draw] canvas %p recreated %dx%d hr=0x%08X", pCanvas, nWidth, nHeight,
				static_cast<unsigned int>(lResult));
		}
		ReleaseComPtr(pCanvas);
		return lResult >= 0;
	}
	catch (_com_error& e) {
		Log("[draw] canvas Create on layer %p failed hr=0x%08X", pLayer, static_cast<unsigned int>(e.Error()));
	}
	catch (...) {
		Log("[draw] canvas Create on layer %p failed", pLayer);
	}
	return false;
}

// Blanks and hides the layer, and hands its origin back to the Gr2D centre - the origin the client
// gives the icon layers themselves (entry ctor 0x007B3176). Until that origin stops pointing at the
// icon layer, this label holds a reference to it, the client's own release cannot free it, and an
// expired icon stays drawn over the icons that the row has shifted up. The layer itself stays around
// to be bound to whatever icon shows up next.
void BuffTimer::ClearLabelLayer(void* pLayer) {
	if (pLayer == nullptr) return;
	EraseCanvas(pLayer, false);
	try {
		void* pGr2D = *reinterpret_cast<void**>(dwGr2DInstance);
		void* pCenter = nullptr;
		_gr2d_get_center(pGr2D, nullptr, &pCenter);
		if (pCenter != nullptr) {
			SetLayerOrigin(pLayer, pCenter);
			ReleaseComPtr(pCenter); // ours; the layer kept its own
		}
		_layer_put_color(pLayer, nullptr, 0); // Putcolor is ARGB: alpha 0 keeps it off screen
	}
	catch (...) {
	}
}

// One attempt at putting sText on the layer's canvas. bErase wipes that canvas first. nRole picks the
// font (minutes, seconds or the black outline), see GetLabelFont.
bool BuffTimer::DrawLabel(void* pLayer, const char* sText, int nRole, bool bErase, bool bLog) {
	try {
		// Nothing on the canvas yet only for the first label of a freshly bound layer; once a digit is
		// there, the canvas has to be wiped before the new one is drawn, or a shorter number leaves
		// the tail of the longer one behind. A wipe that fails must not fall through to the draw: the
		// stale number is left alone until the next redraw instead of being overdrawn.
		if (bErase && !EraseCanvas(pLayer, bLog)) {
			Log("[draw] could not wipe layer %p, leaving the number as it is", pLayer);
			return false;
		}

		void* pCanvas = nullptr;
		_layer_get_canvas(pLayer, nullptr, &pCanvas, g_pEmptyVariant);
		if (bLog) Log("[draw] GetCanvas layer=%p canvas=%p erase=%d text=%s", pLayer, pCanvas, bErase ? 1 : 0, sText);
		if (pCanvas == nullptr) return false; // nothing to draw on; the label is skipped, not forced

		void* pFont = GetLabelFont(nRole);
		void* pOutline = GetLabelFont(kFontOutline);
		if (bLog) Log("[draw] fonts role=%d font=%p outline=%p", nRole, pFont, pOutline);
		if (pFont == nullptr) {
			Log("[draw] no font for role %d, text=%s", nRole, sText);
			ReleaseComPtr(pCanvas);
			return false;
		}

		const int nLayerWidth = _layer_get_width(pLayer, nullptr);
		const int nLayerHeight = _layer_get_height(pLayer, nullptr);

		// Bottom left inside the icon, and never wider or taller than the icon canvas.
		int nX = nOffsetX;
		const int nWidth = MeasureTextWidth(pFont, sText);
		if (nLayerWidth > 0 && nX + nWidth > nLayerWidth) nX = nLayerWidth - nWidth;
		if (nX < 0) nX = 0;

		int nY = nOffsetY;
		if (nLayerHeight > nFontSize) {
			if (nY > nLayerHeight - nFontSize) nY = nLayerHeight - nFontSize;
		}
		else {
			nY = 0;
		}
		if (nY < 0) nY = 0;

		if (bLog) Log("[draw] layer=%dx%d x=%d y=%d w=%d canvas=%dx%d", nLayerWidth, nLayerHeight, nX, nY, nWidth,
			_canvas_get_cx(pCanvas, nullptr), _canvas_get_cy(pCanvas, nullptr));

		if (pOutline != nullptr) {
			for (int i = 0; i < 8; i++) {
				DrawTextOnePass(pCanvas, nX + aTextOutlineOffset[i][0], nY + aTextOutlineOffset[i][1], sText, pOutline);
			}
		}
		unsigned int nDrawn = DrawTextOnePass(pCanvas, nX, nY, sText, pFont);
		ReleaseComPtr(pCanvas);

		if (bLog) Log("[draw] ok text=%s role=%d erase=%d drawn=%u layer=%dx%d x=%d y=%d w=%d",
			sText, nRole, bErase ? 1 : 0, nDrawn, nLayerWidth, nLayerHeight, nX, nY, nWidth);
		return true;
	}
	catch (...) {
		// Never let a failed COM call reach the client's top-level handler: skip the label instead.
		Log("[draw] COM call failed (erase=%d text=%s layer=%p)", bErase ? 1 : 0, sText, pLayer);
		return false;
	}
}

void BuffTimer::Tick(void* pWvsContext) {
	if (!bEnabled || pWvsContext == nullptr) return;

	unsigned int dwNow = GetTickCount();
	if (dwNow - dwLastTick < 100) return; // the label only moves once a second
	dwLastTick = dwNow;

	char* pView = reinterpret_cast<char*>(pWvsContext) + nCWvsContext__TemporaryStatView;
	if (IsBadReadPtr(pView, nView__ListSize + sizeof(int))) return;

	int nCount = *reinterpret_cast<int*>(pView + nView__ListSize);
	for (int i = 0; i < nMaxIcons; i++) aLabels[i].bSeen = false;
	if (nCount > nMaxIcons) nCount = nMaxIcons;

	void* pList = pView + nView__ListBase;
	for (int i = 0; i < nCount; i++) {
		void* pNode = _zlist_find_index(pList, nullptr, static_cast<unsigned int>(i));
		if (pNode == nullptr || IsBadReadPtr(pNode, 8)) continue;

		int* pEntry = *reinterpret_cast<int**>(reinterpret_cast<char*>(pNode) + 4);
		if (pEntry == nullptr) continue;
		if (IsBadReadPtr(pEntry, nTempStat__Overlay + sizeof(void*))) continue;

		int nType = *reinterpret_cast<int*>(reinterpret_cast<char*>(pEntry) + nTempStat__Type);
		if (nType != nTempStatType__Item && nType != nTempStatType__Skill) continue; // family/guild rows

		// The entry id of an item buff is the negated item id (CWvsContext::OnTemporaryStatSet
		// negates it before SetTemporary), while the packet carries the positive one.
		int nId = *reinterpret_cast<int*>(reinterpret_cast<char*>(pEntry) + nTempStat__Id);
		unsigned int dwKey = (nId < 0) ? static_cast<unsigned int>(-nId) : static_cast<unsigned int>(nId);
		if (dwKey == 0) continue; // no id: nothing to look up

		int nRemaining = FindRemainingMs(dwKey);

		// Diagnostics: what each icon is paired with. A label that never moves shows up here as a
		// remaining value that keeps getting refreshed, or as a key that does not belong to the icon.
		if (nIconLogBudget > 0) {
			nIconLogBudget--;
			Log("[icon %d] ptr=%p type=%d id=%d key=%u remaining=%d", i, pEntry, nType, nId, dwKey, nRemaining);
		}

		if (nRemaining <= 0) continue;

		// Long buffs get no number at all: a countdown that starts in the forties is noise, and the
		// official client does not show one either. nMaxMinutes <= 0 lifts the cap back to an hour.
		const unsigned int nCapMs = (nMaxMinutes > 0)
			? static_cast<unsigned int>(nMaxMinutes) * nMinuteMs
			: nHourMs;
		if (static_cast<unsigned int>(nRemaining) >= nCapMs) continue;

		char sText[8];
		int nRole;
		if (static_cast<unsigned int>(nRemaining) >= nMinuteMs) {
			// Whole minutes, rounded up: 59:30 left reads 60. How far up this goes is set by
			// nMaxMinutes (10 by default, so the largest label is 10).
			unsigned int nMinutes = (static_cast<unsigned int>(nRemaining) + nMinuteMs - 1) / nMinuteMs;
			sprintf_s(sText, "%u", nMinutes);
			nRole = kFontMinute;
		}
		else {
			unsigned int nSeconds = (static_cast<unsigned int>(nRemaining) + 999) / 1000;
			sprintf_s(sText, "%u", nSeconds);
			nRole = kFontSecond;
		}

		int nSlot = -1;
		for (int j = 0; j < nMaxIcons; j++) {
			if (aLabels[j].pEntry == pEntry) { nSlot = j; break; }
		}
		if (nSlot < 0) {
			for (int j = 0; j < nMaxIcons; j++) {
				if (aLabels[j].pEntry == nullptr && !aLabels[j].bSeen) { nSlot = j; break; }
			}
			if (nSlot >= 0) {
				void* pIconLayer = *reinterpret_cast<void**>(reinterpret_cast<char*>(pEntry) + nTempStat__Icon);
				void* pLayer = aLabels[nSlot].pLayer;
				if (pLayer == nullptr) pLayer = CreateLabelLayer();
				if (pLayer == nullptr || pIconLayer == nullptr || IsBadReadPtr(pIconLayer, sizeof(void*))) {
					aLabels[nSlot].bSeen = true; // do not let another icon in this tick take the slot
					continue;
				}
				BindLabelLayer(pLayer, pIconLayer);
				aLabels[nSlot].pLayer = pLayer;
				aLabels[nSlot].pEntry = pEntry;
				aLabels[nSlot].sText[0] = 0; // nothing was drawn on this layer for this icon yet
			}
		}
		if (nSlot < 0) continue;

		aLabels[nSlot].bSeen = true;
		if (strcmp(aLabels[nSlot].sText, sText) != 0) {
			static int nDrawCount = 0;
			const bool bLog = bDebug || (++nDrawCount <= 60);
			// Nothing on the canvas yet only for the first label of a freshly bound layer.
			const bool bErase = (aLabels[nSlot].sText[0] != 0);
			if (DrawLabel(aLabels[nSlot].pLayer, sText, nRole, bErase, bLog)) {
				strncpy_s(aLabels[nSlot].sText, sText, _TRUNCATE);
			}
		}
	}

	// Icons that vanished this tick must not keep a cached label: the same address may come back.
	for (int i = 0; i < nMaxIcons; i++) {
		if (!aLabels[i].bSeen && aLabels[i].pEntry != nullptr) {
			// The buff is gone: blank the canvas so the number cannot outlive its icon, and keep the
			// layer around to be bound to whatever icon shows up next.
			ClearLabelLayer(aLabels[i].pLayer);
			aLabels[i].pEntry = nullptr;
			aLabels[i].sText[0] = 0;
		}
	}
}

// ----- hooks --------------------------------------------------------------

static void(__fastcall* _WvsContext__OnTemporaryStatSet)(void* pThis, void* edx, void* pPacket) =
	reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(dwCWvsContext__OnTemporaryStatSet);
static void(__fastcall* _WvsContext__Update)(void* pThis, void* edx) =
	reinterpret_cast<void(__fastcall*)(void*, void*)>(dwCWvsContext__Update);

static void __fastcall WvsContext__OnTemporaryStatSet_Hook(void* pThis, void* edx, void* pPacket) {
	BuffTimer::CaptureDurations(pPacket); // the triplet it needs is still unread in the packet
	_WvsContext__OnTemporaryStatSet(pThis, edx, pPacket);
}

static void __fastcall WvsContext__Update_Hook(void* pThis, void* edx) {
	_WvsContext__Update(pThis, edx);
	BuffTimer::Tick(pThis); // the client has just updated the buff row, so it is safe to draw into it
}

void BuffTimer::HookTemporaryStatSet() {
	Memory::SetHook(true, reinterpret_cast<void**>(&_WvsContext__OnTemporaryStatSet), WvsContext__OnTemporaryStatSet_Hook);
}

void BuffTimer::HookWvsContextUpdate() {
	Memory::SetHook(true, reinterpret_cast<void**>(&_WvsContext__Update), WvsContext__Update_Hook);
}

void BuffTimer::Hook() {
	HookTemporaryStatSet();
	HookWvsContextUpdate();
}

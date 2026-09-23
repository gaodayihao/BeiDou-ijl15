#include "stdafx.h"
#include "BossHP.h"
#include <stdarg.h>

const DWORD dw_TSingleton_CUIMiniMap___ms_pInstance = 0x00BED788;
const DWORD dwCField__ShowMobHpTag = 0x005336CA;
const DWORD dwCField__Init = 0x00528DBC; // CField::CField
const DWORD dwCField__Dispose = 0x00529035; // CField::~CField
const DWORD dwCUIToolTip__SetToolTip_String = 0x008E6E7D;
const DWORD dwCUIToolTip__ClearToolTip = 0x008E6E23;
const DWORD dwCUIToolTip__DisposeToolTip = 0x008E6BA3; // CUIToolTip::~CUIToolTip
const DWORD dwCUIToolTip__CreateToolTip = 0x008E49B5; // CUIToolTip::CUIToolTip
const DWORD dwCUserLocal__Update = 0x0094A144;
const DWORD dwCMobTemplate__GetMobTemplate = 0x0067CD28; // CMobTemplate::GetMobTemplate(dwTemplateID)
const DWORD dwGetBasicFont = 0x0098A707; // get_basic_font(FONT_TYPE), __cdecl(out com_ptr<IWzFont>*, int)
const DWORD dwBstrCtor = 0x00406301; // _bstr_t::_bstr_t(char const*) (same helper AutoTypes.h calls _bstr_ctor)
const DWORD dwIWzFont__CalcTextWidth = 0x0042782E; // (font, &bstr, &variant) -> text width in px
const DWORD dwIWzCanvas__DrawTextA = 0x004277AD; // (x, y, bstr, font, &vAlpha, &vTabOrg)
const DWORD dwIWzGr2DLayer__GetCanvas = 0x00425D2E; // (layer, &canvas, &variant)
const DWORD dwIWzShape2D__GetWidth = 0x0040B920; // (canvas/layer) -> width
const void* pEmptyVariant = reinterpret_cast<const void*>(0x00BF6300); // client's global empty Ztl_variant_t (pvargSrc)

// CField stores the MobGage layer created by CField::ShowMobHPTag at +0x1E4; drawing into that
// layer's own canvas puts the text inside the bar (transparent around it, removed with the bar).
const int nCField__MobHpTagLayer = 0x1E4;

// UI/UIWindow.img/MobGage geometry (wz): the gage layer spans x = [minimap width, 800], the
// 35x37 backgrnd on its left holds the 25x25 boss icon, the gage strip inside is ~19px tall.
const int nBossGageIconWidth = 35;

// get_basic_font slot 0 = 12px white; any unusable setting (negative type, or a slot that the
// client refuses to build) falls back here instead of dropping the text.
const int nDefaultTextFontType = 0;

// White glyphs with a black outline: the client's font slots carry the colour, so the outline is
// the same string drawn in a black slot around the white one (see bossHpTextOutlineFont).
static const int aTextOutlineOffset[8][2] = {
	{ -1, -1 }, { 0, -1 }, { 1, -1 },
	{ -1,  0 },            { 1,  0 },
	{ -1,  1 }, { 0,  1 }, { 1,  1 },
};

char BossHP::aBossHpUIToolTip[1304];
double BossHP::dBossHpPercentage = 0;

bool BossHP::bShowPercent = true;
bool BossHP::bShowText = true;
bool BossHP::bShowTextName = true;
int BossHP::nTextFontType = 0;       // get_basic_font(0) = 12px white
int BossHP::nTextOutlineFont = 1;    // get_basic_font(1) = 12px black
bool BossHP::bTextAlignRight = false; // default: left aligned at the bar's left end (right of the boss icon)
int BossHP::nTextX = 35;             // == the boss icon cap width, i.e. where the gage itself starts
int BossHP::nTextMargin = 14;
int BossHP::nTextY = 24;             // below the ~19px gage strip, still inside the 37px tall bar
bool BossHP::bTextDebug = true;      // bossHpTextDebug: append diagnostics to boss_hp_text.log

// Temporary diagnostics for the bar text (config: bossHpTextDebug).
static void BossTextLog(const char* sFormat, ...) {
	if (!BossHP::bTextDebug) return;
	char sLine[512];
	va_list args;
	va_start(args, sFormat);
	_vsnprintf_s(sLine, sizeof(sLine), _TRUNCATE, sFormat, args);
	va_end(args);
	FILE* pFile = nullptr;
	if (fopen_s(&pFile, "boss_hp_text.log", "a") == 0 && pFile != nullptr) {
		fprintf(pFile, "%s\n", sLine);
		fclose(pFile);
	}
}

static char sBossName[128] = { 0 };
static unsigned int dwBossNameMobId = 0;
static bool bBossNameLoaded = false;
static bool bBossHpPercentShown = false;
static void* pTextFont = nullptr;
static void* pTextOutlineFont = nullptr;

typedef void* (__fastcall* BstrCtor_t)(void* pBstr, void* edx, const char* sText);
typedef int (__fastcall* FontCalcTextWidth_t)(void* pFont, void* edx, void* pBstrData, const void* pVariant);
typedef unsigned int (__fastcall* CanvasDrawTextA_t)(void* pCanvas, void* edx, int nLeft, int nTop, void* pBstrData, void* pFont, const void* pV1, const void* pV2);
typedef void* (__fastcall* LayerGetCanvas_t)(void* pLayer, void* edx, void** ppCanvas, const void* pVariant);
typedef int (__fastcall* ShapeGetWidth_t)(void* pShape, void* edx);

static auto _bstr_ctor = reinterpret_cast<BstrCtor_t>(dwBstrCtor);
static auto _font_calc_text_width = reinterpret_cast<FontCalcTextWidth_t>(dwIWzFont__CalcTextWidth);
static auto _canvas_draw_text = reinterpret_cast<CanvasDrawTextA_t>(dwIWzCanvas__DrawTextA);
static auto _layer_get_canvas = reinterpret_cast<LayerGetCanvas_t>(dwIWzGr2DLayer__GetCanvas);
static auto _shape_get_width = reinterpret_cast<ShapeGetWidth_t>(dwIWzShape2D__GetWidth);

// The client's text APIs take a Ztl_bstr_t by value: what actually travels on the stack is the
// wrapper's inner Data_t pointer (see IWzCanvas::DrawTextA / sub_42782E, which both end with
// `mov ecx,[arg]; call _bstr_t::Data_t::Release`), and the callee destroys that reference. So every
// call gets its own freshly built instance and nothing may be released on this side.
static void* MakeBstrData(char* pBstrSlot, const char* sText) {
	_bstr_ctor(pBstrSlot, nullptr, sText);
	return *reinterpret_cast<void**>(pBstrSlot);
}

static int MeasureTextWidth(void* pFont, const char* sText) {
	char aBstr[8] = { 0 };
	void* pData = MakeBstrData(aBstr, sText);
	if (pData == nullptr) return 0;
	return _font_calc_text_width(pFont, nullptr, pData, pEmptyVariant);
}

static void DrawTextOnePass(void* pCanvas, int nX, int nY, const char* sText, void* pFont) {
	char aBstr[8] = { 0 };
	void* pData = MakeBstrData(aBstr, sText);
	if (pData == nullptr) return;
	_canvas_draw_text(pCanvas, nullptr, nX, nY, pData, pFont, pEmptyVariant, pEmptyVariant);
}

static void ReleaseCanvas(void* pCanvas) {
	if (pCanvas != nullptr) {
		void** pVtbl = *reinterpret_cast<void***>(pCanvas);
		reinterpret_cast<unsigned long(__stdcall*)(void*)>(pVtbl[2])(pCanvas); // IUnknown::Release
	}
}

// Reads a NUL-terminated ANSI string that came from the client's own tables, refusing obviously
// unusable pointers instead of letting strncpy_s walk into unmapped memory.
static bool CopyClientString(char* sOut, size_t nOutSize, const char* sSource) {
	if (sOut == nullptr || nOutSize == 0) return false;
	sOut[0] = 0;
	if (sSource == nullptr || IsBadReadPtr(sSource, 1)) return false;
	strncpy_s(sOut, nOutSize, sSource, _TRUNCATE);
	return true;
}


void BossHP::Hook() { // main method
	HookInternal();
}

void BossHP::HookInternal() {
	HookUpdate();
	HookShowMobHPTag();
	BossHP::HookDisposeField();
	HookInitField();
}

void BossHP::HookUpdate() {
	typedef void(__fastcall* UserLocal__Update_type)(void* pThis, void* edx);
	static auto _UserLocal__Update = reinterpret_cast<UserLocal__Update_type>(dwCUserLocal__Update);

	UserLocal__Update_type Hook = [](void* pThis, void* edx) -> void
	{
		_UserLocal__Update(pThis, edx);
		DrawBossHpNumberIfNeed();
	};

	Memory::SetHook(true, reinterpret_cast<void**>(&_UserLocal__Update), Hook);
}

void BossHP::HookShowMobHPTag() {
	typedef void(__fastcall* Field__ShowMobHPTag_type)(void* pThis, void* edx, unsigned int dwMobID, int nColor, int nBgColor, int nHP, int nMaxHP);
	static auto _Field__ShowMobHPTag = reinterpret_cast<Field__ShowMobHPTag_type>(dwCField__ShowMobHpTag);

	Field__ShowMobHPTag_type Hook = [](void* pThis, void* edx, unsigned int dwMobID, int nColor, int nBgColor, int nHP, int nMaxHP) -> void
	{
		_Field__ShowMobHPTag(pThis, edx, dwMobID, nColor, nBgColor, nHP, nMaxHP);
		DrawBossHpNumber(nHP, nMaxHP);
		DrawBossHpBarText(pThis, dwMobID, nHP, nMaxHP); // the gage layer/canvas was just (re)created
	};
	Memory::SetHook(true, reinterpret_cast<void**>(&_Field__ShowMobHPTag), Hook);
}

void BossHP::HookInitField() {
	typedef void(__fastcall* Field__Init_Type)(void* pThis, void* edx);
	static auto _Field__Init = reinterpret_cast<Field__Init_Type>(dwCField__Init);

	Field__Init_Type Hook = [](void* pThis, void* edx) -> void
	{
		if (dBossHpPercentage > 0) {
			BossHP::DisposeBossHpNumber();
		}
		BossHP::DisposeToolTip((int)&aBossHpUIToolTip);
		BossHP::CreateToolTip((int)&aBossHpUIToolTip);
		BossHP::DisposeBossHpBarText();
		_Field__Init(pThis, edx);
	};
	Memory::SetHook(true, reinterpret_cast<void**>(&_Field__Init), Hook);
}

void BossHP::HookDisposeField() {
	typedef void(__fastcall* Field__Dispose_Type)(void* pThis, void* edx);
	static auto _Field__Dispose = reinterpret_cast<Field__Dispose_Type>(dwCField__Dispose);

	Field__Dispose_Type Hook = [](void* pThis, void* edx) -> void
	{
		DisposeBossHpNumber();
		DisposeBossHpBarText();
		_Field__Dispose(pThis, edx);
	};
	Memory::SetHook(true, reinterpret_cast<void**>(&_Field__Dispose), Hook);
}

void BossHP::DrawBossHpNumberIfNeed() {
	if (bShowPercent && dBossHpPercentage > 0) {
		char sToolTip[20];
		sprintf_s(sToolTip, "%.2f%%", dBossHpPercentage);
		BossHP::SetToolTip_String((int)&aBossHpUIToolTip, GetMiniMapWidth(), 37, sToolTip);
		bBossHpPercentShown = true;
	}
	else if (bBossHpPercentShown) { // config turned it off, or the bar is gone
		bBossHpPercentShown = false;
		BossHP::ClearToolTip((int)&aBossHpUIToolTip);
	}
}

void BossHP::DrawBossHpNumber(int nHP, int nMaxHP) {
	if (nHP > 0) {
		dBossHpPercentage = static_cast<double>(nHP) / nMaxHP * 100.0;
	}
	else {
		dBossHpPercentage = 0;
		BossHP::ClearToolTip((int)&aBossHpUIToolTip);
	}
}

void BossHP::DisposeBossHpNumber() {
	dBossHpPercentage = 0;
	bBossHpPercentShown = false;
	BossHP::ClearToolTip((int)&aBossHpUIToolTip);
}

// ----- "[Boss name] hp" inside the boss gage (UI/UIWindow.img/MobGage) -----
//
// The text goes straight into the canvas of the gage layer the client creates in
// CField::ShowMobHPTag (CField+0x1E4): that canvas is transparent except for the bar itself, is
// recreated on every bar update (so nothing can ghost) and dies with the bar. A CUIToolTip cannot be
// used here - it paints a translucent black box and imposes its own layout.
//
// Font slots come from get_basic_font; the slot carries the colour, so "white with a black outline"
// is the white slot drawn on top of the black slot at the 8 neighbouring pixels.

void BossHP::FormatThousands(int nValue, char* sOut, size_t nOutSize) {
	// the client's ZXString::Format has no thousands separator, so group the digits here
	char sDigits[16];
	sprintf_s(sDigits, "%u", nValue < 0 ? (unsigned int)(-nValue) : (unsigned int)nValue);
	size_t nLen = strlen(sDigits);
	size_t nOut = 0;
	if (nValue < 0 && nOut + 1 < nOutSize) sOut[nOut++] = '-';
	for (size_t i = 0; i < nLen && nOut + 1 < nOutSize; i++) {
		if (i > 0 && ((nLen - i) % 3) == 0) sOut[nOut++] = ',';
		if (nOut + 1 >= nOutSize) break;
		sOut[nOut++] = sDigits[i];
	}
	sOut[nOut] = 0;
}

const char* BossHP::GetBossName(unsigned int dwMobID) {
	// The client itself caches the localized name as the ZXString<char> at CMobTemplate+0x30 when it
	// builds the gage (CField::ShowMobHPTag), so read the same string instead of re-reading String.wz:
	// it is already in the codepage the client's text drawing expects.
	if (dwMobID == 0) return "";

	if (!bBossNameLoaded || dwBossNameMobId != dwMobID) {
		sBossName[0] = 0;
		typedef void* (__cdecl* GetMobTemplate_t)(unsigned int dwTemplateID);
		static auto _GetMobTemplate = reinterpret_cast<GetMobTemplate_t>(dwCMobTemplate__GetMobTemplate);
		void* pTemplate = _GetMobTemplate(dwMobID);
		if (pTemplate != nullptr) {
			if (!IsBadReadPtr(reinterpret_cast<char*>(pTemplate) + 0x30, sizeof(void*))) {
				const char* sName = *reinterpret_cast<const char**>(reinterpret_cast<char*>(pTemplate) + 0x30);
				CopyClientString(sBossName, sizeof(sBossName), sName);
			}
		}
		dwBossNameMobId = dwMobID;
		bBossNameLoaded = true;
	}
	return sBossName;
}

// Diagnostics only: the game's name string is in the client's own codepage, so print its raw bytes
// (the log file itself is read back by editors that assume UTF-8).
static void FormatNameHex(const char* sName, char* sOut, size_t nOutSize) {
	size_t nOut = 0;
	for (size_t i = 0; sName[i] != 0 && i < 8 && nOut + 3 < nOutSize; i++) {
		sprintf_s(sOut + nOut, nOutSize - nOut, "%02X ", static_cast<unsigned char>(sName[i]));
		nOut += 3;
	}
	sOut[nOut] = 0;
}

void* BossHP::GetFont(int nType) {
	if (nType < 0) return nullptr;

	typedef void* (__cdecl* GetBasicFont_t)(void** pOut, int nType);
	static auto _get_basic_font = reinterpret_cast<GetBasicFont_t>(dwGetBasicFont);
	void* pFont = nullptr;
	_get_basic_font(&pFont, nType);
	return pFont; // the slot is cached by the client and keeps its own reference
}

void BossHP::DisposeBossHpBarText() {
	// Nothing to clear: the text lives in the client's gage canvas, which is dropped with the bar.
	// Only the per-mob cache has to go.
	dwBossNameMobId = 0;
	bBossNameLoaded = false;
	sBossName[0] = 0;
}

void BossHP::DrawBossHpBarText(void* pCField, unsigned int dwMobID, int nHP, int nMaxHP) {
	static int nDrawCount = 0;
	bool bLog = bTextDebug && (++nDrawCount <= 5 || (nDrawCount % 200) == 0);

	if (!bShowText) {
		if (bLog) BossTextLog("[%d] skip: bossHpText=false", nDrawCount);
		return;
	}
	if (pCField == nullptr) return;
	if (dwMobID == 0 || nHP <= 0 || nMaxHP <= 0) {
		if (bLog) BossTextLog("[%d] skip: mobID=%u hp=%d maxHp=%d", nDrawCount, dwMobID, nHP, nMaxHP);
		return; // the client dropped the gage
	}

	void* pLayer = *reinterpret_cast<void**>(reinterpret_cast<char*>(pCField) + nCField__MobHpTagLayer);
	if (pLayer == nullptr) {
		if (bLog) BossTextLog("[%d] skip: layer(pCField+0x%X) = null, mobID=%u", nDrawCount, nCField__MobHpTagLayer, dwMobID);
		return;
	}

	void* pCanvas = nullptr;
	_layer_get_canvas(pLayer, nullptr, &pCanvas, pEmptyVariant);
	if (pCanvas == nullptr) {
		if (bLog) BossTextLog("[%d] skip: GetCanvas(%p) = null, mobID=%u", nDrawCount, pLayer, dwMobID);
		return;
	}

	// A negative bossHpTextFont (kept from an older config where -1 meant "tooltip font") or a slot
	// the client cannot build falls back to the default white slot instead of hiding the text.
	int nFillType = (nTextFontType < 0) ? nDefaultTextFontType : nTextFontType;
	void* pFont = (pTextFont != nullptr) ? pTextFont : (pTextFont = GetFont(nFillType));
	if (pFont == nullptr && nFillType != nDefaultTextFontType) {
		nFillType = nDefaultTextFontType;
		pFont = pTextFont = GetFont(nFillType);
	}
	void* pOutline = (nTextOutlineFont < 0) ? nullptr
		: ((pTextOutlineFont != nullptr) ? pTextOutlineFont : (pTextOutlineFont = GetFont(nTextOutlineFont)));
	if (pFont == nullptr) {
		if (bLog) BossTextLog("[%d] skip: font(type=%d) = null", nDrawCount, nFillType);
		ReleaseCanvas(pCanvas);
		return;
	}

	char sHp[32];
	FormatThousands(nHP, sHp, sizeof(sHp));
	char sText[192];
	const char* sName = bShowTextName ? GetBossName(dwMobID) : "";
	if (sName[0] != 0) sprintf_s(sText, "[%s] %s", sName, sHp);
	else sprintf_s(sText, "%s", sHp);

	int nBarWidth = _shape_get_width(pCanvas, nullptr); // == UI width - minimap width
	int nWidth = MeasureTextWidth(pFont, sText);

	// Placement: default is the bar's left end (just right of the 35px boss icon cap) on the band
	// below the ~19px gage strip; bossHpTextAlign=1 keeps the old right aligned form instead.
	int nX;
	if (bTextAlignRight) {
		int nLeft = nBossGageIconWidth + 2;
		if (bShowTextName && nWidth > (nBarWidth - nTextMargin - nLeft)) { // too wide: drop the name
			sprintf_s(sText, "%s", sHp);
			nWidth = MeasureTextWidth(pFont, sText);
		}
		nX = nBarWidth - nTextMargin - nWidth;
		if (nX < nLeft) nX = nLeft;
	}
	else {
		nX = (nTextX < 0) ? 0 : nTextX;
		if (bShowTextName && nWidth > (nBarWidth - nX - nTextMargin)) { // too wide: drop the name
			sprintf_s(sText, "%s", sHp);
			nWidth = MeasureTextWidth(pFont, sText);
		}
		if (nX + nWidth > nBarWidth) nX = (nBarWidth > nWidth) ? (nBarWidth - nWidth) : 0;
	}

	// Each pass hands the client its own bstr instance (the callee releases it).
	if (pOutline != nullptr) {
		for (int i = 0; i < 8; i++) {
			DrawTextOnePass(pCanvas, nX + aTextOutlineOffset[i][0], nTextY + aTextOutlineOffset[i][1], sText, pOutline);
		}
	}
	DrawTextOnePass(pCanvas, nX, nTextY, sText, pFont);

	if (bLog) {
		char sNameHex[32];
		FormatNameHex(sName, sNameHex, sizeof(sNameHex));
		BossTextLog("[%d] draw mobID=%u hp=%d maxHp=%d nameBytes=%s fontType=%d outlineType=%d layer=%p canvas=%p font=%p outline=%p barWidth=%d textWidth=%d x=%d y=%d",
			nDrawCount, dwMobID, nHP, nMaxHP, sNameHex, nFillType, nTextOutlineFont, pLayer, pCanvas, pFont, pOutline, nBarWidth, nWidth, nX, nTextY);
	}
	ReleaseCanvas(pCanvas);
}

// it's ToolTip region

typedef void(__fastcall* UIToolTip__SetToolTip_String_Type)(int pThis, void* edx, int x, int y, const char* sToolTip);
static auto _UIToolTip__SetToolTip_String = reinterpret_cast<UIToolTip__SetToolTip_String_Type>(dwCUIToolTip__SetToolTip_String);

void BossHP::SetToolTip_String(int instance, int x, int y, const char* sToolTip) {
	_UIToolTip__SetToolTip_String(instance, 0, x, y, sToolTip);
}

typedef void(__fastcall* UIToolTip__ClearToolTip_Type)(int pThis, void* edx);
static auto _UIToolTip__ClearToolTip = reinterpret_cast<UIToolTip__ClearToolTip_Type>(dwCUIToolTip__ClearToolTip);

void BossHP::ClearToolTip(int instance) {
	_UIToolTip__ClearToolTip(instance, 0);
}

typedef void(__fastcall* UIToolTip__DisposeToolTip_Type)(int pThis, void* edx);
static auto _UIToolTip__DisposeToolTip = reinterpret_cast<UIToolTip__DisposeToolTip_Type>(dwCUIToolTip__DisposeToolTip);

void BossHP::DisposeToolTip(int instance)
{
	_UIToolTip__DisposeToolTip(instance, 0);
}

typedef void(__fastcall* UIToolTip__CreateToolTip_Type)(int pThis, void* edx);
static auto _UIToolTip__CreateToolTip = reinterpret_cast<UIToolTip__CreateToolTip_Type>(dwCUIToolTip__CreateToolTip);

void BossHP::CreateToolTip(int instance)
{
	_UIToolTip__CreateToolTip(instance, 0);
}

int ReadInt(const DWORD dwAddress) {
	int nResult = -1;
	DWORD dwOldProtect;
	VirtualProtect((void*)dwAddress, sizeof(int), PAGE_EXECUTE_READ, &dwOldProtect);
	nResult = *reinterpret_cast<unsigned int*>(dwAddress);
	VirtualProtect((void*)dwAddress, sizeof(int), dwOldProtect, &dwOldProtect);
	return nResult;
}

int BossHP::GetMiniMapWidth() {
	return ReadInt(ReadInt(dw_TSingleton_CUIMiniMap___ms_pInstance) + 0x24); // 
}
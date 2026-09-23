#include "stdafx.h"
#include "BossHP.h"

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
const DWORD dwBstrDataRelease = 0x00402EA5; // _bstr_t::Data_t::Release()
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
int BossHP::nTextMargin = 14;
int BossHP::nTextY = 7;

static char sBossName[128] = { 0 };
static unsigned int dwBossNameMobId = 0;
static bool bBossNameLoaded = false;
static bool bBossHpPercentShown = false;
static void* pTextFont = nullptr;
static void* pTextOutlineFont = nullptr;

typedef void* (__fastcall* BstrCtor_t)(void* pBstr, void* edx, const char* sText);
typedef unsigned long (__fastcall* BstrDataRelease_t)(void* pData, void* edx);
typedef int (__fastcall* FontCalcTextWidth_t)(void* pFont, void* edx, void** ppBstr, const void* pVariant);
typedef unsigned int (__fastcall* CanvasDrawTextA_t)(void* pCanvas, void* edx, int nLeft, int nTop, void** ppBstr, void* pFont, const void* pV1, const void* pV2);
typedef void* (__fastcall* LayerGetCanvas_t)(void* pLayer, void* edx, void** ppCanvas, const void* pVariant);
typedef int (__fastcall* ShapeGetWidth_t)(void* pShape, void* edx);

static auto _bstr_ctor = reinterpret_cast<BstrCtor_t>(dwBstrCtor);
static auto _bstr_data_release = reinterpret_cast<BstrDataRelease_t>(dwBstrDataRelease);
static auto _font_calc_text_width = reinterpret_cast<FontCalcTextWidth_t>(dwIWzFont__CalcTextWidth);
static auto _canvas_draw_text = reinterpret_cast<CanvasDrawTextA_t>(dwIWzCanvas__DrawTextA);
static auto _layer_get_canvas = reinterpret_cast<LayerGetCanvas_t>(dwIWzGr2DLayer__GetCanvas);
static auto _shape_get_width = reinterpret_cast<ShapeGetWidth_t>(dwIWzShape2D__GetWidth);

// Builds the client's _bstr_t in place; the drawing API only reads it, the measuring helper
// consumes (releases) it, so callers must not release a slot that was handed to the measurer.
static bool MakeBstr(char* pBstrSlot, const char* sText) {
	_bstr_ctor(pBstrSlot, nullptr, sText);
	return *reinterpret_cast<void**>(pBstrSlot) != nullptr;
}

static void ReleaseBstr(char* pBstrSlot) {
	void* pData = *reinterpret_cast<void**>(pBstrSlot);
	if (pData != nullptr) {
		_bstr_data_release(pData, nullptr);
		*reinterpret_cast<void**>(pBstrSlot) = nullptr;
	}
}

static void ReleaseCanvas(void* pCanvas) {
	if (pCanvas != nullptr) {
		void** pVtbl = *reinterpret_cast<void***>(pCanvas);
		reinterpret_cast<unsigned long(__stdcall*)(void*)>(pVtbl[2])(pCanvas); // IUnknown::Release
	}
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
			const char* sName = *reinterpret_cast<const char**>(reinterpret_cast<char*>(pTemplate) + 0x30);
			if (sName != nullptr) strncpy_s(sBossName, sizeof(sBossName), sName, _TRUNCATE);
		}
		dwBossNameMobId = dwMobID;
		bBossNameLoaded = true;
	}
	return sBossName;
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
	if (!bShowText || pCField == nullptr) return;
	if (dwMobID == 0 || nHP <= 0 || nMaxHP <= 0) return; // the client dropped the gage

	void* pLayer = *reinterpret_cast<void**>(reinterpret_cast<char*>(pCField) + nCField__MobHpTagLayer);
	if (pLayer == nullptr) return;

	void* pCanvas = nullptr;
	_layer_get_canvas(pLayer, nullptr, &pCanvas, pEmptyVariant);
	if (pCanvas == nullptr) return;

	void* pFont = (pTextFont != nullptr) ? pTextFont : (pTextFont = GetFont(nTextFontType));
	void* pOutline = (nTextOutlineFont < 0) ? nullptr
		: ((pTextOutlineFont != nullptr) ? pTextOutlineFont : (pTextOutlineFont = GetFont(nTextOutlineFont)));
	if (pFont == nullptr) {
		ReleaseCanvas(pCanvas);
		return;
	}

	char sHp[32];
	FormatThousands(nHP, sHp, sizeof(sHp));
	char sText[192];
	const char* sName = bShowTextName ? GetBossName(dwMobID) : "";
	if (sName[0] != 0) sprintf_s(sText, "[%s] %s", sName, sHp);
	else sprintf_s(sText, "%s", sHp);

	int nBarWidth = _shape_get_width(pCanvas, nullptr); // == 800 - minimap width
	int nLeft = nBossGageIconWidth + 2;                 // never overlap the boss icon on the left
	char aMeasure[8] = { 0 };
	if (MakeBstr(aMeasure, sText)) {
		// Same measuring call the client uses to right align its own numbers (the callee releases
		// the bstr it was handed).
		int nWidth = _font_calc_text_width(pFont, nullptr, reinterpret_cast<void**>(aMeasure), pEmptyVariant);
		if (bShowTextName && nWidth > (nBarWidth - nTextMargin - nLeft)) { // too wide: drop the name
			sprintf_s(sText, "%s", sHp);
			char aMeasureHp[8] = { 0 };
			if (MakeBstr(aMeasureHp, sText)) {
				nWidth = _font_calc_text_width(pFont, nullptr, reinterpret_cast<void**>(aMeasureHp), pEmptyVariant);
			}
		}

		int nX = nBarWidth - nTextMargin - nWidth;
		if (nX < nLeft) nX = nLeft;

		// DrawTextA only reads the bstr, so one instance covers every pass.
		char aDraw[8] = { 0 };
		if (MakeBstr(aDraw, sText)) {
			if (pOutline != nullptr) {
				for (int i = 0; i < 8; i++) {
					_canvas_draw_text(pCanvas, nullptr, nX + aTextOutlineOffset[i][0], nTextY + aTextOutlineOffset[i][1],
						reinterpret_cast<void**>(aDraw), pOutline, pEmptyVariant, pEmptyVariant);
				}
			}
			_canvas_draw_text(pCanvas, nullptr, nX, nTextY, reinterpret_cast<void**>(aDraw), pFont, pEmptyVariant, pEmptyVariant);
			ReleaseBstr(aDraw);
		}
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
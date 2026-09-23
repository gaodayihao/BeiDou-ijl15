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

// CUIToolTip layout used by the bar text (verified against CUIToolTip::SetBasicInfo /
// SetToolTip_String / MakeLayer in the client):
//   +0x00C = layer width, set by SetToolTip_String to (text width + 8)
//   +0x440 = the font SetToolTip_String draws with; replace it to change size/colour
const int nCUIToolTip__LayerWidth = 0x00C;
const int nCUIToolTip__Font = 0x440;

// UI/UIWindow.img/MobGage geometry (wz): the gage layer spans x = [minimap width, 800], the
// 35x37 backgrnd on its left holds the 25x25 boss icon, the gage strip inside is ~19px tall.
const int nBossGageRight = 800;
const int nBossGageIconWidth = 35;

char BossHP::aBossHpUIToolTip[1304];
double BossHP::dBossHpPercentage = 0;

bool BossHP::bShowPercent = true;
bool BossHP::bShowText = true;
bool BossHP::bShowTextName = true;
int BossHP::nTextFontType = -1;
int BossHP::nTextMargin = 12;
int BossHP::nTextY = 5;

char BossHP::aBossHpTextUIToolTip[1304];
unsigned int BossHP::dwBossMobId = 0;
int BossHP::nBossHp = 0;
int BossHP::nBossMaxHp = 0;

static char sBossName[128] = { 0 };
static unsigned int dwBossNameMobId = 0;
static bool bBossNameLoaded = false;
static char sBossHpTextLast[192] = { 0 };
static int nBossHpTextLastX = 0;
static bool bBossHpTextShown = false;
static bool bBossHpPercentShown = false;

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
		DrawBossHpBarTextIfNeed();
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
		DrawBossHpBarText(dwMobID, nHP, nMaxHP);
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
		BossHP::DisposeToolTip((int)&aBossHpTextUIToolTip);
		BossHP::CreateToolTip((int)&aBossHpTextUIToolTip);
		BossHP::ApplyTextFont((int)&aBossHpTextUIToolTip);
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

// ----- "[Boss name] hp" label inside the boss gage (UI/UIWindow.img/MobGage) -----
//
// The label reuses the client's CUIToolTip like the percentage above does: it renders the text into
// an own layer (with the client's own font/encoding handling, so Chinese names stay correct) which is
// what makes a font change possible - SetToolTip_String always draws with the font stored at
// CUIToolTip+0x440, so ApplyTextFont swaps exactly that slot. Right alignment needs the text width,
// which SetToolTip_String publishes as (layer width - 8) at CUIToolTip+0x0C.

static int ReadToolTipLayerWidth(int nInstance) {
	return *reinterpret_cast<int*>(nInstance + nCUIToolTip__LayerWidth);
}

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

void BossHP::ApplyTextFont(int instance) {
	if (instance == 0 || nTextFontType < 0) return; // -1 keeps the tooltip's own font

	void* pFont = nullptr;
	typedef void* (__cdecl* GetBasicFont_t)(void** pOut, int nType);
	static auto _get_basic_font = reinterpret_cast<GetBasicFont_t>(dwGetBasicFont);
	_get_basic_font(&pFont, nTextFontType);
	if (pFont != nullptr) {
		// Hand the reference returned by get_basic_font over to the tooltip slot: SetToolTip_String
		// AddRef/Releases that slot around every draw, so the slot owns it from here on.
		*reinterpret_cast<void**>(instance + nCUIToolTip__Font) = pFont;
	}
}

void BossHP::DrawBossHpBarText(unsigned int dwMobID, int nHP, int nMaxHP) {
	dwBossMobId = dwMobID;
	nBossHp = nHP;
	nBossMaxHp = nMaxHP;
	if (nHP <= 0 || nMaxHP <= 0) DisposeBossHpBarText(); // the client drops the gage with it
}

void BossHP::DisposeBossHpBarText() {
	if (bBossHpTextShown) { // only touch a tooltip that has actually been drawn into
		bBossHpTextShown = false;
		BossHP::ClearToolTip((int)&aBossHpTextUIToolTip);
	}
	dwBossMobId = 0;
	nBossHp = 0;
	nBossMaxHp = 0;
	dwBossNameMobId = 0;
	bBossNameLoaded = false;
	sBossName[0] = 0;
	sBossHpTextLast[0] = 0;
	nBossHpTextLastX = 0;
}

void BossHP::DrawBossHpBarTextIfNeed() {
	if (!bShowText) {
		DisposeBossHpBarText();
		return;
	}
	if (dwBossMobId == 0 || nBossHp <= 0) return;

	char sText[192];
	const char* sName = bShowTextName ? GetBossName(dwBossMobId) : "";
	char sHp[32];
	FormatThousands(nBossHp, sHp, sizeof(sHp));
	if (sName[0] != 0) sprintf_s(sText, "[%s] %s", sName, sHp);
	else sprintf_s(sText, "%s", sHp);

	int nInstance = (int)&aBossHpTextUIToolTip;
	int nLeft = GetMiniMapWidth() + nBossGageIconWidth + 4;           // do not overlap the boss icon
	int nAvail = nBossGageRight - nTextMargin - nLeft;

	if (strcmp(sText, sBossHpTextLast) != 0) {
		// Width is unknown until the client has laid the string out once, so measure with a throwaway
		// call (the tooltip layer is replaced, not stacked) and place it right after. Measuring at the
		// previous x keeps that intermediate layer next to its final spot.
		int nMeasureX = (nBossHpTextLastX > nLeft) ? nBossHpTextLastX : nLeft;
		BossHP::SetToolTip_String(nInstance, nMeasureX, nTextY, sText);
		int nWidth = ReadToolTipLayerWidth(nInstance) - 8;
		if (bShowTextName && nWidth > nAvail) { // too wide: keep the number, drop the name
			BossHP::SetToolTip_String(nInstance, nLeft, nTextY, sHp);
			nWidth = ReadToolTipLayerWidth(nInstance) - 8;
			strcpy_s(sText, sHp);
		}
		int nX = nBossGageRight - nTextMargin - 4 - nWidth; // CUIToolTip draws the string at (4, 1)
		if (nX < nLeft) nX = nLeft;
		BossHP::SetToolTip_String(nInstance, nX, nTextY, sText);
		nBossHpTextLastX = nX;
		strcpy_s(sBossHpTextLast, sText);
	}
	else {
		BossHP::SetToolTip_String(nInstance, nBossHpTextLastX, nTextY, sBossHpTextLast);
	}
	bBossHpTextShown = true;
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
#pragma once

class BuffTimer
{
public:
	static void Hook();

	// config.ini [optional]
	static bool bEnabled;       // buffTimer           : the whole feature
	static int nMinuteFont;     // buffTimerMinuteFont : FONT_TYPE slot for the >=1min tier (green)
	static int nSecondFont;     // buffTimerSecondFont : FONT_TYPE slot for the <1min tier (yellow)
	static int nOutlineFont;    // buffTimerOutlineFont: FONT_TYPE slot for the 8 black outline passes
	static int nFontSize;       // buffTimerSize       : point size of the fonts the label builds
	static int nMaxMinutes;     // buffTimerMaxMinutes : no number at all from this much time left up
	static int nMinuteColor;    // buffTimerMinuteColor: RGB of the >=1min tier
	static int nSecondColor;    // buffTimerSecondColor: RGB of the <1min tier
	static int nOutlineColor;   // buffTimerOutlineColor
	static int nOffsetX;        // buffTimerX          : text anchor inside the icon canvas
	static int nOffsetY;        // buffTimerY

	// Called from the file-scope __fastcall thunks that Memory::SetHook installs.
	static void CaptureDurations(void* pPacket);
	static void Tick(void* pWvsContext);

private:
	// The three fonts the label draws with. The table's slots are not enough on their own (see
	// GetLabelFont), so each role gets a font built with the requested size and colour.
	enum { kFontMinute = 0, kFontSecond = 1, kFontOutline = 2, kFontRoleCount = 3 };

	static void ParseDurations(void* pPacket);
	static void HookTemporaryStatSet();
	static void HookWvsContextUpdate();
	//
	static void RememberBuff(unsigned int dwId, unsigned int dwDurationMs);
	static void ForgetBuff(unsigned int dwId);
	static int FindRemainingMs(unsigned int dwId);
	//
	// One label layer per buff icon. A layer's canvas cannot be cleared, so the layer is replaced
	// whenever the number changes - see the rebuild in Tick.
	static bool DrawLabel(void* pLayer, const char* sText, int nRole);
	static void* CreateLabelLayer();
	static void BindLabelLayer(void* pLayer, void* pIconLayer);
	static bool SetLayerOrigin(void* pLayer, void* pOrigin);
	static bool SetLayerZ(void* pLayer, int nZ);
	static void ClearLabelLayer(void* pLayer);
	static void* GetFont(int nType);
	static void* GetLabelFont(int nRole);
	static void* CreateFont(int nSize, int nRgb);
};

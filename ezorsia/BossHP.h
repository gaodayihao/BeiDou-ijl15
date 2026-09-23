#pragma once

class BossHP
{
public:
	static void Hook();

	// config.ini [optional]
	static bool bShowPercent;      // bossHpPercent         : percentage label under the boss icon (original feature)
	static bool bShowText;         // bossHpText            : "[name] hp" drawn inside the bar, right aligned
	static bool bShowTextName;     // bossHpTextShowName
	static int  nTextFontType;     // bossHpTextFont        : client font slot for the glyphs (0 = 12px white)
	static int  nTextOutlineFont;  // bossHpTextOutlineFont : client font slot for the outline (1 = 12px black, -1 = none)
	static int  nTextMargin;       // bossHpTextMargin      : px between the text and the bar's right edge
	static int  nTextY;            // bossHpTextY           : text top inside the bar (bar is 37px tall)
	static bool bTextDebug;        // bossHpTextDebug       : append diagnostics to boss_hp_text.log
private:
	static char aBossHpUIToolTip[1304];
	static double dBossHpPercentage;
	//
	static void HookInternal();
	static void HookUpdate();
	static void HookShowMobHPTag();
	static void HookDisposeField();
	static void HookInitField();
	//
	static void SetToolTip_String(int instance, int x, int y, const char* sToolTip);
	static void ClearToolTip(int instance);
	static void DisposeToolTip(int instance);
	static void CreateToolTip(int instance);
	//
	static void DrawBossHpNumberIfNeed();
	static void DrawBossHpNumber(int nHP, int nMaxHP);
	static void DisposeBossHpNumber();
	//
	static int GetMiniMapWidth();

	// ----- boss HP bar text: drawn straight into the client's own MobGage canvas -----
	static void DrawBossHpBarText(void* pCField, unsigned int dwMobID, int nHP, int nMaxHP);
	static void DisposeBossHpBarText();
	static void FormatThousands(int nValue, char* sOut, size_t nOutSize);
	static const char* GetBossName(unsigned int dwMobID);
	static void* GetFont(int nType);

};

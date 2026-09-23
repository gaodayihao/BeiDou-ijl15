#pragma once

class BossHP
{
public:
	static void Hook();

	// config.ini [optional]
	static bool bShowPercent;     // bossHpPercent    : percentage label under the boss icon (original feature)
	static bool bShowText;        // bossHpText       : "[name] hp" label inside the bar, right aligned
	static bool bShowTextName;    // bossHpTextShowName
	static int  nTextFontType;    // bossHpTextFont   : -1 = tooltip default font, 0..55 = client font slot
	static int  nTextMargin;      // bossHpTextMargin : px between the label and the bar's right edge
	static int  nTextY;           // bossHpTextY      : label top inside the bar (bar is 37px tall)
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

	// ----- boss HP bar text ("[name] 12,345,678" inside UI/UIWindow.img/MobGage) -----
	static char aBossHpTextUIToolTip[1304];
	static unsigned int dwBossMobId;
	static int nBossHp;
	static int nBossMaxHp;
	//
	static void DrawBossHpBarTextIfNeed();
	static void DrawBossHpBarText(unsigned int dwMobID, int nHP, int nMaxHP);
	static void DisposeBossHpBarText();
	static void ApplyTextFont(int instance);
	static void FormatThousands(int nValue, char* sOut, size_t nOutSize);
	static const char* GetBossName(unsigned int dwMobID);

};

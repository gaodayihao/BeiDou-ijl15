#pragma once

class BossHP
{
public:
	static void Hook();

	// config.ini [optional]
	static bool bShowPercent;   // bossHpPercent      : percentage label under the boss icon (original feature)
	static bool bShowText;      // bossHpText         : hp inside the bar + name below it, both on the gage canvas
	static bool bShowTextName;  // bossHpTextShowName : false = hp only
	static bool bTextDebug;     // not a config key: mirrors [debug] debug= and drives boss_hp_text.log
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

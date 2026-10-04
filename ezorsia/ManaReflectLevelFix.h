#pragma once

// Client-side fix for the Mana Reflection reflect rate (2221002 / 2121002 / 2321002).
//
// The reflect branch of CUserLocal::SetDamaged (0x009581A9, the block at 0x00958D50-0x00958E12) looks the
// WZ level table up through a HARD-CODED skill id - it pushes 0x205D2A (2121002, the F/P Arch Mage's Mana
// Reflection) into CSkillInfo::GetSkill - and takes the level from the local player's skill slot at
// CWvsContext+0x2134+0x60C. For an I/L Arch Mage (222) or a Bishop (232) that slot yields the wrong level,
// so the client computes the reflected amount with the wrong `x`: measured level 1 -> x = 55 while the
// character really is level 10 -> x = 100, i.e. the client drew 624 where the server sent 1135.
//
// Fix: hook SKILLENTRY::GetLevelData (0x00760F23); whenever it is asked for a level row of that hard-coded
// F/P entry, re-query the level of whichever Mana Reflection the character actually has
// (2121002/2221002/2321002) through the client's own CSkillInfo::GetSkillLevel and pass that level down
// instead. Both the chance (`prop`, +0xF4 of the level data) and the rate (`x`, +0x13C) read the same row,
// so the hook fixes the roll and the amount together. Nothing else in the client is touched.
//
// Enable with [debug] manaReflectFix=true in config.ini (default on). No log file is written.
void Hook_ManaReflectLevelFix(bool enable);

#pragma once

// BUG-023 fix: the "new character" equipment-name list window paints the name of the item selected
// in each of the 9 equipment slots. The getter it uses (sub_61828B at 0x0061828B) reads CLogin's
// per-slot candidate list for the active gender (CLogin+0x244 male / +0x268 female), adds 4 to that
// data pointer and hands the result to ZXString<char>::operator= as the source string. When a slot
// list is empty the client clears both lists on every step change, and the window outlives the
// new-character step the pointer is NULL, so "NULL + 4" is dereferenced: fault reading address
// 0x00000004 (exception 0xC0000005 at 0x004181D8, reached from CWndMan::RedrawInvalidatedWindows
// while the client was exiting).
//
// The data side already treats an empty list as normal (sub_5FCF8A and CLogin::ShiftNewCharEquip
// both test the pointer before using it), only this getter does not. This module adds the missing
// check and leaves every other path, including the non-NULL case, to the client.
//
// The three window Draw overrides that paint the list (0x00618EB4 / 0x00618026 / 0x0061A3A4) all
// call this one getter, so a single guard covers every caller.
void Hook_NewCharEquipNullGuard(bool enable);

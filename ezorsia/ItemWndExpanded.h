#pragma once

// Item window (UI/UIWindow.img/Item): the expanded "every tab at once" layout.
//
// The client knows both layouts - the window has an expand/collapse button - but it starts from the
// narrow window with a scrollbar and drops the player's choice on the way out, so the expanded view
// never comes back on its own. These two switches fix the two halves separately:
//
//   Hook_ItemWndExpanded      - what a character with no stored preference gets (the default the
//                               client falls back to). The client's own default is the narrow window.
//   Hook_ItemWndRememberChoice - whether the choice made with the button survives a session. The
//                               client writes it, but erases the whole per-character option key when
//                               it saves, so today it does not.
//
// Both are independent: leaving the default alone and only remembering gives stock behaviour plus a
// working button, and taking the default without remembering gives "always expanded" with the button
// still usable inside the session. See ItemWndExpanded.cpp for the addresses and
// docs/客户端逆向-背包默认展平.md for the reverse-engineering record.
void Hook_ItemWndExpanded(bool enable);
void Hook_ItemWndRememberChoice(bool enable);

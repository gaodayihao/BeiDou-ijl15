#pragma once

// Mob vacuum - Ctrl+0 in game toggles it.
//
// While it is on, every non-boss mob the client knows about is moved to the point the local
// player stood on at the moment the key was pressed; the point does NOT follow the player, and
// the mobs stay where they are once the key is pressed again.
//
// Why a local write is enough in v83: the CLIENT drives mob movement, not the server. The client
// that controls a mob reports its own position through MOVE_LIFE (0xBC) and the server validates
// only the ACTION (skill/attack ids) - never the coordinates - while its attack-range check is a
// log-only DAMAGE_HACK alert. The reported positions follow the write, so the server's copy of
// the mob stays in sync.
//
// What is touched (see MobVac.cpp for the addresses and the evidence behind them):
//   * CMob::Update            - after the client's own update the mob is put back on the stored
//                               point (current position + its previous-position copy), so nothing
//                               of the mob's own AI/vector-controller motion survives the frame;
//   * a thread this module owns - polls Ctrl+0, snapshots the player position, and drops the state
//                               when the character, the field or life state changes. It only reads
//                               globals; the client's update chain stays untouched (BossHP already
//                               detours CUserLocal::Update, and chaining onto it is avoidable);
//   * CUserLocal::OnSetDead   - drops the state on death; a change of the current field
//                               (get_field's payload) drops it on map change / relogin.
//
// Bosses are deliberately excluded (CMobTemplate+0x208, the flag that makes the client skip its
// per-mob HP tag loop and use the big gage instead).
//
// Enable with [optional] mobVac=true in config.ini (default off).
// With [debug] debug=true the module logs to the console DllMain allocates: the install result, the
// toggle, and (once a second while the vacuum runs) one mob's live/previous/wanted position.
namespace MobVac { extern bool bDebug; }

void Hook_MobVac(bool enable);

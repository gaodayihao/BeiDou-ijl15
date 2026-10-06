#pragma once

// Mob vacuum - Ctrl+0 in game toggles it.
//
// While it is on, every non-boss mob the client knows about is held inside a leash around an anchor
// that starts where the player stood when the vacuum was switched on. A mob that walks out of the
// leash is put back - onto the leash edge, not onto the anchor, so the correction is the few pixels it
// overshot instead of the whole leash - and no mob is moved twice within a couple of hundred
// milliseconds, because each placement re-seeds the mob's move path and a mob moved every frame is
// frozen: unable to move, to attack or to be hit. The anchor follows the player, softly and only once
// they have really walked off, so the mobs gather around the player (which is also where their own AI
// stops them and they start attacking). Mobs that spawn later join on their own - hooking CMob::Update
// covers the whole pool, not a snapshot.
//
// Why the client can do this at all: in v83 the client owns mob movement. The client that controls
// a mob generates its move path and reports it through MOVE_LIFE (0xBC); the server only checks that
// the reporter is the mob's controller and then follows the reported coordinates. So a local
// reposition is also a server-visible one - there is no packet for this module to forge, and the
// server's copy does move: a relogin re-spawns the mobs from it, on the vacuum point.
//
// What is touched (see MobVac.cpp for the addresses and the evidence behind them):
//   * CMob::Update            - BEFORE the client's own update of the mob, so its attack-range /
//                               skill / move-path decisions and the C->S report it derives from them
//                               are made with the mob already where this module put it. A mob inside
//                               the leash is left alone, and a mob that was moved is left alone for
//                               the cooldown - the client's own update of that mob runs in between,
//                               which is what lets it move, attack and be hit. The mob's vector
//                               controller is put on the corrected point through the client's own
//                               CVecCtrlMob::SetActive, which also binds the foothold under that
//                               point and re-seeds the move path there. The controller's "last
//                               acknowledged point" cache is moved with it - without that the client
//                               re-bases the mob onto the old point on its next movement decision and
//                               keeps reporting the old point - and the mob's two position copies are
//                               written so the rest of the frame sees a consistent mob;
//   * a thread this module owns - polls Ctrl+0 and drops the state when the character, the field or
//                               the life state changes. It only reads globals; the client's update
//                               chain stays untouched (BossHP already detours CUserLocal::Update,
//                               and chaining onto it is avoidable);
//   * CUserLocal::OnSetDead   - drops the state on death; a change of the current field
//                               (get_field's payload) drops it on map change / relogin.
//
// Not touched: the sprite layer (CMob+0x4C0) is left alone - it already follows the controller,
// because CMob::Init bound the layer's origin to it. Shifting that layer instead is what made the
// first attempt's mobs invisible: the layer's x/y is an offset from the controller, not a position.
//
// What this cannot do:
//   * mobs the server did not put under this player's control - their reports are dropped server
//     side and the server re-seeds the mob from its own copy when it hands control over again;
//   * mobs the client has no instance of (the server only sends SPAWN_MONSTER for mobs within view);
//   * bosses, which are deliberately excluded (CMobTemplate+0x208, the flag that makes the client
//     skip its per-mob HP tag loop and use the big gage instead).
//
// Enable with [optional] mobVac=true in config.ini (default off).
// With [debug] debug=true the module logs to the console DllMain allocates: the install result, the
// toggle, and (once a second) the running counters plus one row per mob - where it was when it was
// checked, where the frame left it, and what its controllers and its acknowledged-point cache say.
namespace MobVac { extern bool bDebug; }

void Hook_MobVac(bool enable);

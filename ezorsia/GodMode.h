#pragma once

// Invincibility (god mode) - Ctrl+9 in game toggles it.
//
// While it is on the local player cannot be hurt by a mob: not by a body/touch attack, not by a mob
// attack skill, and with no hit stun or hurt animation either. Nothing is forged for that - the
// client simply never sends the CP_UserHit (0x30) report that carries the damage it computed, and
// the server's TakeDamageHandler takes the damage out of that very report, so the server-side HP
// stays untouched too. Mob-skill debuffs travel on a different packet and are refused separately.
//
// How, and why it is the client's own mechanism rather than a new one:
//   * in v83 the CLIENT owns the damage its character takes. CUserLocal::SetDamaged (0x009581A9) is
//     the single function behind it: it computes the damage, writes the character's HP, plays the
//     hurt animation/sound, and finally builds and sends CP_UserHit (0x30). It is the only 0x30
//     sender that names a mob (the client's three other 0x30 senders are CUserLocal::Update's field
//     damage - swamp, mist - which never name one);
//   * its prologue already refuses to run while a "no damage until" timestamp in the CUserLocal
//     object is still in the future. That is the window the game itself opens around melee attacks;
//     this module keeps the timestamp in the future, i.e. holds the client's own invincibility state
//     open. No detour on SetDamaged is needed, and the field is the pure damage gate (nothing else
//     in the client reads it), so the character's look does not change;
//   * a mob skill is not part of that report: the client that controls the mob reports the skill
//     (MOVE_LIFE 0xBC), the server applies it to the players in range and answers with a GIVE_BUFF
//     packet whose 128-bit stat mask holds only disease bits. That packet is dropped on arrival, so
//     the client never learns about the debuff. Only this player's own debuff can arrive that way:
//     the broadcast about other players uses GIVE_FOREIGN_BUFF (0xC7), which is a different opcode
//     with a different client handler and is left completely alone.
//
// What this cannot do:
//   * field damage (swamp, poison mist, ...) does not go through SetDamaged and is not gated by that
//     timestamp - it still lands;
//   * the server still records the debuff on the character; only the client refuses it. For the
//     diseases in question that costs nothing visible: the server has no seal/darkness check at all,
//     and ZOMBIFY only changes the HP floor Character.applyHpMpChange :8227 uses;
//   * damage the server applies on its own (GM commands, event scripts) is untouched.
//
// The state survives a map change, and two client behaviours have to be allowed for to make that
// work: entering a field REBUILDS the local user (CField::Init calls CUserPool::CreateLocalUser, so
// the object the timestamp is held on is replaced), and the field carrier is cleared while the client
// swaps fields. The mode therefore follows the object instead of being tied to it, and only a lasting
// absence of the field (login screen, character select) ends it - an object that fails to validate
// only costs one tick, it does not end the mode. Switching the mode off also clears the timestamp, so
// the immunity ends immediately instead of running out its last window.
//
// Enable with [optional] godMode=true in config.ini (default off). There is nothing to tune.
// The module prints nothing while it runs - only the one install/refuse line at startup.
//
// The addresses, the mask layout and the evidence behind all of the above:
// Ursa-Server docs/client/reverse/10-无敌与受击免疫.md (that manual is the only home for it).
void Hook_GodMode(bool enable);

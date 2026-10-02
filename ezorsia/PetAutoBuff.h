#pragma once

// Pet auto-buff refresh -- the second half of the pet equip window's buff slots (see Ursa-Server
// docs/client/003). PetSkillSlot remembers which buff skill sits in which of the six cells; this
// module watches those buffs and re-casts them when they run out.
//
// The cast is silent by construction: it composes the client's own 0x5B packet and hands it to
// CClientSocket::SendPacket, deliberately NOT going through CUserLocal::DoActiveSkill or
// SendSkillUseRequest. Both of those set the local cast-wait fields (user + 8356 / + 8360), which is
// what makes a cast consume the player's action; skipping them is exactly why the buff lands without
// an animation or an action on the caster, while other players still see the buff effect (the server
// broadcasts showBuffEffect to everyone but the caster).
class PetAutoBuff
{
public:
    static void Hook();

    // config.ini [optional]
    static bool bEnabled;   // petAutoBuff       : re-cast buffs when they run out
    static int nLeadMs;     // petAutoBuffLead   : re-cast this many ms before they expire
    static int nTickMs;     // petAutoBuffTickMs : how often the six cells are scanned (ms, clamped
                            //                     100..5000); each cell is additionally retried at
                            //                     most once per 2 s, which bounds the packets
};

void Hook_PetAutoBuff(bool enable);

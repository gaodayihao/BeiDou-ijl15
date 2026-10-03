#pragma once

// Card / rate-coupon buff coexistence (see Ursa-Server docs/client/004, BUG-029).
//
// CTemporaryStatView keeps one entry per mask bit, and a monster card's DEFENSE_ATT shares its bit with
// the drop coupon's COUPON_DRP1 (four more pairs behave the same way), so the later GIVE_BUFF used to
// carry the earlier entry away and one of the two icons disappeared. This module keeps the other
// entry alive while the client processes a GIVE_BUFF/CANCEL_BUFF, mirroring the client's own rule
// ("a re-sent buff replaces its own old entry, a different buff sharing the bit is kept").
//
// The resistance itself is NOT a client-side job: the client's CalcBuffDefenseAttr is reached with
// element = 0 for the attacks in question (measured in game), and its switch returns the damage
// unchanged for that code -- the element resistance is settled on the server (docs/client/004 §6).
class CardDefenseAttr
{
public:
    static void Hook();

    // config.ini [optional]
    static bool bCoexist;   // cardBuffCoexist : keep card and coupon buff icons side by side
};

void Hook_CardDefenseAttr(bool coexist);

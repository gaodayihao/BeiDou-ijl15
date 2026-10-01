#pragma once

// Pet auto-buff configuration persistence -- the piece that keeps the server's copy of the six slots
// in step with PetSkillSlot's memory (see Ursa-Server docs/client/003 §5.11).
//
//   save : every slot change (drop stores a skill, drag removes one) re-sends the WHOLE six-slot
//          configuration as PET_BUFF_CONFIG 0x1001 -> version(1) + 6 x int32. Whole-packet and
//          idempotent: the server stores exactly what it is told, and a lost packet is corrected by
//          the next change. Nothing is sent when the slots are cleared on leaving the game -- that
//          clear is a character switch, and the server already holds the outgoing character's config.
//   load : the server sends the same opcode back when the player enters the game, in the same layout,
//          so both directions share one serialisation. No repaint is needed: the push arrives before
//          the pet equip window exists, and CUIPetEquip::Draw reads the slots live every time it runs.
//
// 0x1001 is an Ursa addition (v83 has no case for it), so the receive hook CONSUMES the packet
// instead of forwarding it to the client's own dispatcher.
class PetBuffConfig
{
public:
    static void Hook();

    // config.ini [optional]
    static bool bDebug;     // petBuffDebug : append every send / load to petbuff.log (same switch as
                            // the slots themselves)
};

void Hook_PetBuffConfig(bool enable);

// Sends all six slots to the server. Called by PetSkillSlot right after a change.
void PetBuffConfig_Send();

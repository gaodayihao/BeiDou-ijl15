#pragma once

// Pet auto-buff slots -- client-side configuration UI (see Ursa-Server docs/client/003).
//
// The pet equip window's SECOND row has four cells. #1/#2 hold the pet's item pouch and meso
// magnet (the client registers both in its rect table at 0x00BE2260); #3/#4 are painted by the
// window background but the client registers nothing there -- sub_8011FA answers 0 and a click
// falls through. Those two cells are this feature's slots:
//
//   row 1 (y 11..43): [auto-HP potion gear][HP potion 200][auto-MP potion gear][MP potion 201]
//   row 2 (y 44..76): [item pouch      ][meso magnet  ][   buff slot   ][   buff slot     ]
//
// A buff skill dragged from the skill window onto #3/#4 is accepted by hooking
// CDraggableSkill::OnDropped (0x004FAA22) and remembered per pet tab, so each of the three pets
// keeps its own pair (3 tabs x 2 cells = 6). The tab index is the window object's this+360, the
// same field sub_8011FA switches on.
//
// Everything here is client-side state: nothing is sent to the server, nothing is written to
// disk. The drop is only accepted when the drag's own handler declined it, so quickslot / macro /
// skill-window drops are untouched.
class PetSkillSlot
{
public:
    static void Hook();

    // config.ini [optional]
    static bool bEnabled;   // petBuff      : the whole feature
    static bool bDebug;     // petBuffDebug : append every drop decision to petbuff.log

    // FONT_TYPE of the 「自动技能」 label over the two cells, 0..55 as get_basic_font has them.
    static int nLabelFont;  // petSkillSlotFont

    // Skill id in slot (nPet 0..2, nSlot 0..1); 0 = empty. Used by the auto-refresh step and by the
    // configuration sync (PetBuffConfig) that ships it to / receives it from the server.
    static int GetSkill(int nPet, int nSlot);

    // Writes one slot without notifying the server. Used only by the configuration load (the values
    // come from the server and must not be echoed back); player-driven changes go through the drop
    // paths, which send the whole configuration themselves.
    static void SetSkill(int nPet, int nSlot, int nSkillId);
};

void Hook_PetSkillSlot(bool enable);

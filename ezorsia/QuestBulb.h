#pragma once

// Pins the "auto-accept quest" light bulb to the left edge of the screen instead of letting it
// float above the character's head (same treatment the official nostalgia client gives it).
//
// The client builds the bulb as an IWzGr2DLayer parented to the character
// (Effect/BasicEff.img/QuestAlert/{appear,default}). This module only re-points that layer's
// coordinate origin to screen space and pins its position: the character, the wz files and
// BeiDou.exe are left untouched.
class QuestBulb
{
public:
	static void Hook();

	// config.ini [optional]
	static bool bFixed;   // questBulbFixed : true = bulb stays at a fixed screen position
	static int  nFixedX;  // questBulbX     : pixels from the screen's left edge
	static int  nFixedY;  // questBulbY     : pixels from the screen's top edge; <0 = vertically centred

private:
	static void Apply(void* pCUserLocal);
	static void PinLayer(void* pLayer);
};

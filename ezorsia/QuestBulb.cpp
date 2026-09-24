#include "stdafx.h"
#include "QuestBulb.h"
#include <stdarg.h>
#include <oleauto.h>

// ===== Reverse-engineering anchors (Angel.exe / BeiDou.exe, v83) =====
//
// The bulb is `Effect/BasicEff.img/QuestAlert/{appear,default}`. The chain that puts it on the
// character is:
//
//   CWvsContext::UpdateAutoQuestAlertIcon  0x00A08D30  - if any of the "quest available" flags
//                                                        (CWvsContext+0x3760/+0x3790/+0x37B0) is
//                                                        non-zero it jumps straight into the
//                                                        worker below, otherwise it clears the
//                                                        layer and returns.
//   CUserLocal::UpdateQuestAlertIcon       0x0095CED9  - loads the wz animation through
//                                                        CUser::LoadLayer (0x00941417, called at
//                                                        0x0095CFD9/0x0095D13C), keeps the
//                                                        resulting IWzGr2DLayer at
//                                                        CUserLocal+0x3180, and maintains the
//                                                        companion flag at CUserLocal+0x317C.
//   CUserLocal::Update                    0x0094A144  - calls the above every frame (0x0094BCB3).
//
// CUser::LoadLayer hands the new layer the avatar layer under the face as its coordinate origin
// (CAvatar::GetLayerUnderFace, 0x00451E7E -> sub_43EA3E), which is why the bulb rides the
// character's head, the camera and the walk animation all at once.
//
// Positioning an IWzVector2D - the layer's own x/y is NOT a screen coordinate:
//
//   * The engine keeps an absolute value alongside it: the layer's position in the root frame.
//     That is what get_rx/get_ry report, and what raw_RelMove takes. `x` is the same value
//     converted into the layer's current origin frame - measured on a live layer, after
//     raw_RelMove(X) the layer reads back x = X + (origin's own x), i.e. the absolute is
//     x - origin.x. The client's own CWnd::GetAbsLeft reads a position back exactly that way.
//   * The absolute frame is centre-based: CWndMan's constructor raw_RelMoves the root layer to
//     (-width/2, -height/2), so `screen = absolute + (width/2, height/2)`. The client's own
//     placement of this bulb corroborates the scale: it reads back as (609,282) on a 1280x720
//     window, i.e. just above the character's head and slightly left of centre.
//
// The three relevant slots, with their signatures read off the mangled names in this exe (never
// guess an argument count here - a wrong one corrupts the stack silently, see below):
//
//   raw_RelMove  (+144)  long __stdcall(long x, long y, VARIANT, VARIANT)
//                        Sets the ABSOLUTE position and converts internally. It does not touch the
//                        origin, so nothing has to be saved or restored - this is the client's own
//                        "put this layer here" call (CWnd::CreateWnd 0x009DE4D2 uses it for every
//                        window, right after pointing the window's origin at the screen root).
//                        Both VARIANTs are filled with VT_EMPTY, matching the client.
//   raw_Move     (+64)   long __stdcall(long x, long y)
//                        Teleport: CVecCtrl::raw_Move 0x009B5E7F zeroes the velocity fields and
//                        calls SetMovePathAttribute(3), and it *detaches the layer from its
//                        origin* - GetLT comes back null straight after. A bulb moved this way
//                        freezes at a spot in the map that the character can walk up to, and
//                        trying to undo it with put_origin rewrites the coordinates.
//   put_origin   (+100)  long __stdcall(VARIANT)
//                        Reparents. Right for a freshly created layer - CWnd::CreateWnd attaches
//                        every window to CWndMan+0xDC that way - but reparenting *this* layer made
//                        it disappear from the screen in two test rounds while its position read
//                        back exactly as asked, so it is deliberately not used any more.
//
// Vtable slots are confirmed twice over in the same binary:
//   * CVecCtrl implements IWzVector2D, and the vtable it installs at this+0x0C (0x00B3E1F8,
//     written by CVecCtrl::CVecCtrl 0x009B0F71) still carries every PDB name: +32 get_x,
//     +40 get_y, +96 get_origin, +100 put_origin, +104 get_rx, +112 get_ry, +144 raw_RelMove;
//   * the stateful wrappers agree - IWzShape2D::Getx 0x00403CB7 uses +32, IWzShape2D::Gety
//     0x00403CDE uses +40, IWzVector2D::GetOrigin 0x00441AA1 uses +96.
const int nVtbl_IWzVector2D__get_x = 32;
const int nVtbl_IWzVector2D__get_y = 40;
const int nVtbl_IWzVector2D__get_rx = 104;
const int nVtbl_IWzVector2D__get_ry = 112;
const int nVtbl_IWzVector2D__raw_RelMove = 144;

const DWORD dwCUserLocal__UpdateQuestAlertIcon = 0x0095CED9;
const DWORD dwIWzGr2DLayer__GetHeight = 0x00440C2A;
const DWORD dwIWzGr2DLayer__GetZ = 0x0044337D;

// CUserLocal+0x3180 is the bulb's IWzGr2DLayer (com_ptr, null while no bulb is shown);
// the sibling flag CUserLocal+0x317C is not "a layer exists" - the client also loads the idle
// animation with that flag cleared - so only the pointer is used here.
const int nCUserLocal__QuestAlertLayer = 0x3180;

// Where the client's own placement puts the bulb on screen: just above the character's head, and
// the follow camera keeps the character on one spot. Only differences against this are ever used,
// so a constant error here is a constant error in the landing position, tuneable with
// questBulbX/questBulbY.
const int nCharScreenOffsetY = 60;

typedef long(__stdcall* IWzVector2D__get_long_t)(void* pThis, long* pnOut);
typedef long(__stdcall* IWzVector2D__raw_RelMove_t)(void* pThis, long nX, long nY, VARIANTARG vAttr1, VARIANTARG vAttr2);
typedef int(__fastcall* IWzGr2DLayer__GetHeight_t)(void* pThis, void* edx);
typedef int(__fastcall* IWzGr2DLayer__GetZ_t)(void* pThis, void* edx);

bool QuestBulb::bFixed = true;
int QuestBulb::nFixedX = 10;
int QuestBulb::nFixedY = -1;
bool QuestBulb::bDebug = false;

static void* pPinnedLayer = nullptr;
static unsigned int nPinCount = 0;
static long nWantedX = 0;     // the bulb's absolute position, i.e. screen minus half the window
static long nWantedY = 0;
static int nPinnedHeight = 0; // the layer height the vertical centring was derived from

void QuestBulb::Log(const char* sFormat, ...) {
	if (!bDebug) return;
	char sLine[256];
	va_list args;
	va_start(args, sFormat);
	_vsnprintf_s(sLine, sizeof(sLine), _TRUNCATE, sFormat, args);
	va_end(args);
	FILE* pFile = nullptr;
	if (fopen_s(&pFile, "quest_bulb.log", "a") == 0 && pFile != nullptr) {
		fprintf(pFile, "%s\n", sLine);
		fclose(pFile);
	}
}

static void GetLocalPosition(void* pVector, long* pnX, long* pnY) {
	void** pVtbl = *reinterpret_cast<void***>(pVector);
	*pnX = 0;
	*pnY = 0;
	reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_x / sizeof(void*)])(pVector, pnX);
	reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_y / sizeof(void*)])(pVector, pnY);
}

// The engine's own absolute position - the value raw_RelMove writes. Diagnostics: it is the one
// number that says whether the bulb really ended up where it was asked to.
static void GetAbsolutePosition(void* pVector, long* pnX, long* pnY) {
	void** pVtbl = *reinterpret_cast<void***>(pVector);
	*pnX = 0;
	*pnY = 0;
	reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_rx / sizeof(void*)])(pVector, pnX);
	reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_ry / sizeof(void*)])(pVector, pnY);
}

// Places a layer at an absolute position. This is the client's own positioning call.
//
// The two trailing VARIANTs are not optional padding: the signature is
// `long __stdcall raw_RelMove(long x, long y, VARIANT, VARIANT)` (mangled
// `?raw_RelMove@CVecCtrl@@UAGJJJUtagVARIANT@@0@Z`), and CWnd::CreateWnd fills both with an empty
// variant. Calling it with x/y alone leaves 32 bytes of garbage on the stack and the callee reads
// the empty slot as a pointer - that is what crashed the client once (fault: write to NULL at
// IWzGr2DLayer::GetLT+0x2E, from a stack already unwound by the wrong pop count).
static void SetLayerPosition(void* pVector, long nX, long nY) {
	void** pVtbl = *reinterpret_cast<void***>(pVector);
	VARIANTARG vAttr1;
	VARIANTARG vAttr2;
	memset(&vAttr1, 0, sizeof(vAttr1)); // VT_EMPTY: no move-path attributes, i.e. an instant move
	memset(&vAttr2, 0, sizeof(vAttr2));
	reinterpret_cast<IWzVector2D__raw_RelMove_t>(pVtbl[nVtbl_IWzVector2D__raw_RelMove / sizeof(void*)])(pVector, nX, nY, vAttr1, vAttr2);
}

void QuestBulb::Hook() {
	typedef void(__fastcall* UserLocal__UpdateQuestAlertIcon_t)(void* pThis, void* edx);
	static auto _UpdateQuestAlertIcon = reinterpret_cast<UserLocal__UpdateQuestAlertIcon_t>(dwCUserLocal__UpdateQuestAlertIcon);

	UserLocal__UpdateQuestAlertIcon_t Hook = [](void* pThis, void* edx) -> void
	{
		_UpdateQuestAlertIcon(pThis, edx);
		QuestBulb::Apply(pThis); // the layer is (re)created inside the call above
	};

	Memory::SetHook(true, reinterpret_cast<void**>(&_UpdateQuestAlertIcon), Hook);
}

void QuestBulb::Apply(void* pCUserLocal) {
	if (pCUserLocal == nullptr) return;
	if (IsBadReadPtr(reinterpret_cast<char*>(pCUserLocal) + nCUserLocal__QuestAlertLayer, sizeof(void*))) return;

	void* pLayer = *reinterpret_cast<void**>(reinterpret_cast<char*>(pCUserLocal) + nCUserLocal__QuestAlertLayer);
	if (pLayer == nullptr) {
		pPinnedLayer = nullptr; // bulb taken down: the next one gets measured again
		return;
	}
	if (!bFixed) return;

	PinLayer(pLayer);
}

void QuestBulb::PinLayer(void* pLayer) {
	if (IsBadReadPtr(pLayer, sizeof(void*))) return;
	void** pVtbl = *reinterpret_cast<void***>(pLayer);
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__raw_RelMove + sizeof(void*))) return;

	if (pPinnedLayer != pLayer) {
		pPinnedLayer = pLayer;
		nPinCount = 0;
		nPinnedHeight = 0;

		// Vertical centring is resolved once per layer instance: the bulb's animation frames do
		// not all have the same height, so re-centring every frame would make it jitter.
		int nTargetY = nFixedY;
		if (nTargetY < 0) {
			nPinnedHeight = reinterpret_cast<IWzGr2DLayer__GetHeight_t>(dwIWzGr2DLayer__GetHeight)(pLayer, nullptr);
			if (nPinnedHeight < 0 || nPinnedHeight > Client::m_nGameHeight) nPinnedHeight = 0;
			nTargetY = (Client::m_nGameHeight - nPinnedHeight) / 2;
			if (nTargetY < 0) nTargetY = 0;
		}
		int nTargetX = (nFixedX < 0) ? 0 : nFixedX;

		// The client's own placement is the reference: it puts the bulb just above the character's
		// head, and the follow camera keeps the character on one screen spot, so that spot is a
		// known screen position. Only the difference between the target and it is needed, which
		// makes the absolute frame's own bias cancel out.
		long lx = 0, ly = 0, rx = 0, ry = 0;
		GetLocalPosition(pLayer, &lx, &ly);
		GetAbsolutePosition(pLayer, &rx, &ry);
		long nAnchorScreenX = Client::m_nGameWidth / 2;
		long nAnchorScreenY = Client::m_nGameHeight / 2 - nCharScreenOffsetY;
		nWantedX = rx + (nTargetX - nAnchorScreenX);
		nWantedY = ry + (nTargetY - nAnchorScreenY);

		int nZ = reinterpret_cast<IWzGr2DLayer__GetZ_t>(dwIWzGr2DLayer__GetZ)(pLayer, nullptr);
		Log("layer=%p h=%d anchor=(%ld,%ld) target=(%d,%d) wanted=(%ld,%ld) stock=(%ld,%ld) stockScreen=(%ld,%ld) local=(%ld,%ld) z=%d",
			pLayer, nPinnedHeight, nAnchorScreenX, nAnchorScreenY, nTargetX, nTargetY, nWantedX, nWantedY, rx, ry,
			rx + Client::m_nGameWidth / 2, ry + Client::m_nGameHeight / 2, lx, ly, nZ);
	}

	// The absolute has to be re-asserted because the layer's origin chain (the character) keeps
	// moving, which changes the layer's own coordinates. raw_RelMove does not touch the origin, so
	// re-writing the same absolute is free of side effects.
	SetLayerPosition(pLayer, nWantedX, nWantedY);

	nPinCount++;
	if (bDebug && (nPinCount <= 5 || (nPinCount % 600) == 0)) {
		long lx = 0, ly = 0, rx = 0, ry = 0;
		GetLocalPosition(pLayer, &lx, &ly);
		GetAbsolutePosition(pLayer, &rx, &ry);
		Log("[%u] layer=%p abs=(%ld,%ld) wanted=(%ld,%ld) eq=%d local=(%ld,%ld)",
			nPinCount, pLayer, rx, ry, nWantedX, nWantedY,
			(rx == nWantedX && ry == nWantedY) ? 1 : 0, lx, ly);
	}
}

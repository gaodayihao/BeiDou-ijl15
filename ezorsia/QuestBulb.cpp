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
// character's head - and with it the camera and the walk animation. That origin is *world* space,
// so no position written against it can ever stay put on screen: the bulb keeps the character's
// on-screen motion (camera easing, jumps).
//
// The cure is the client's own recipe for anything that must sit at a fixed screen position, and
// it is plain to read in CWnd::CreateWnd (0x009DE4D2), which builds every UI window:
//
//     sub_410FDF(&v, *(CWndMan::ms_pInstance + 0xDC), 1);   // VARIANT{VT_UNKNOWN, screen origin}
//     IWzVector2D::put_origin (+100)  <- that VARIANT        // child of the SCREEN frame
//     IWzVector2D::raw_RelMove (+144) <- nLeft, nTop, empty, empty   // the window's screen position
//     IWzGr2DLayer::PutZ (+180) / Putcolor (+224)
//
// `CWndMan+0xDC` is what CWndMan::GetOrgWindow (0x0048BBA5) answers with - it ignores its UIOrigin
// argument and always returns that field - and CUIStatusBar::OnCreate (0x008D01B2) hangs the HUD
// off it too. Sprite layers use the same origin: CChatBalloon::MakeScreenBalloon (0x0048B86A)
// creates its layer at screen coordinates with IWzGr2D::CreateLayer (0x00426C7E) and points its
// origin there. A layer parented to it takes *screen pixels* in raw_RelMove, so the target is the
// literal questBulbX/questBulbY.
//
// Slot signatures are read off the mangled names in this exe, never guessed - a wrong argument
// count on a vtable call corrupts the stack silently:
//   ?raw_Move@CVecCtrl@@UAGJJJ@Z                    long raw_Move(long x, long y)
//   ?raw_RelMove@CVecCtrl@@UAGJJJUtagVARIANT@@0@Z   long raw_RelMove(long x, long y, VARIANT, VARIANT)
//   ?put_origin@CVecCtrl@@UAGJUtagVARIANT@@@Z       long put_origin(VARIANT)
//   ?get_origin@CVecCtrl@@UAGJPAUtagVARIANT@@@Z     long get_origin(VARIANT* pOut)
//   ?get_x@CVecCtrl@@UAGJPAJ@Z                      long get_x(long* pOut)
//   ?get_rx@CVecCtrl@@UAGJPAJ@Z / ?get_ry@...       long get_rx(long* pOut)
//
// raw_Move (+64) is the one to avoid: CVecCtrl::raw_Move 0x009B5E7F zeroes the velocity fields and
// calls SetMovePathAttribute(3), and it *detaches the layer from its origin* (GetLT comes back null
// straight after) - a bulb moved that way freezes at a spot in the map the character can walk up
// to. raw_RelMove does not touch the origin, so re-writing the same position is free of side
// effects and needs no restore.
//
// Vtable slots are confirmed twice over in the same binary:
//   * CVecCtrl implements IWzVector2D, and the vtable it installs at this+0x0C (0x00B3E1F8,
//     written by CVecCtrl::CVecCtrl 0x009B0F71) still carries every PDB name: +32 get_x,
//     +40 get_y, +96 get_origin, +100 put_origin, +104 get_rx, +112 get_ry, +144 raw_RelMove;
//   * the stateful wrappers agree - IWzShape2D::Getx 0x00403CB7 uses +32, IWzShape2D::Gety
//     0x00403CDE uses +40, IWzVector2D::GetOrigin 0x00441AA1 uses +96.
const int nVtbl_IWzVector2D__get_x = 32;
const int nVtbl_IWzVector2D__get_y = 40;
const int nVtbl_IWzVector2D__put_origin = 100;
const int nVtbl_IWzVector2D__raw_RelMove = 144;

const DWORD dwCUserLocal__UpdateQuestAlertIcon = 0x0095CED9;
const DWORD dwIWzGr2DLayer__GetHeight = 0x00440C2A;
const DWORD dwIWzGr2DLayer__GetZ = 0x0044337D;
const DWORD dwCWndMan__ms_pInstance = 0x00BEC20C;

// CWndMan::GetOrgWindow 0x0048BBA5 answers with this field, whatever UIOrigin it is asked for.
const int nCWndMan__OrgWindow = 0xDC;

// CUserLocal+0x3180 is the bulb's IWzGr2DLayer (com_ptr, null while no bulb is shown);
// the sibling flag CUserLocal+0x317C is not "a layer exists" - the client also loads the idle
// animation with that flag cleared - so only the pointer is used here.
const int nCUserLocal__QuestAlertLayer = 0x3180;

typedef long(__stdcall* IWzVector2D__get_long_t)(void* pThis, long* pnOut);
typedef long(__stdcall* IWzVector2D__raw_RelMove_t)(void* pThis, long nX, long nY, VARIANTARG vAttr1, VARIANTARG vAttr2);
typedef long(__stdcall* IWzVector2D__put_origin_t)(void* pThis, VARIANTARG vOrigin);
typedef int(__fastcall* IWzGr2DLayer__GetHeight_t)(void* pThis, void* edx);
typedef int(__fastcall* IWzGr2DLayer__GetZ_t)(void* pThis, void* edx);

bool QuestBulb::bFixed = true;
int QuestBulb::nFixedX = 10;
int QuestBulb::nFixedY = -1;
bool QuestBulb::bDebug = false;

static void* pPinnedLayer = nullptr;
static bool bScreenParented = false; // false for a layer we must not touch
static unsigned int nPinCount = 0;
static long nWantedX = 0;     // screen position the bulb is pinned to (per layer instance)
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

// Reparents a layer onto the given screen-space origin. The origin is passed as a by-value
// VARIANT built the way the client builds it (sub_410FDF: vt = VT_UNKNOWN, AddRef'd); the callee
// takes that reference over, so the AddRef here is the one it will release.
static void PutOrigin(void* pVector, void* pOrigin) {
	void** pVtbl = *reinterpret_cast<void***>(pVector);
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__put_origin + sizeof(void*))) return;

	VARIANTARG vOrigin;
	memset(&vOrigin, 0, sizeof(vOrigin));
	vOrigin.vt = VT_UNKNOWN;
	vOrigin.punkVal = reinterpret_cast<IUnknown*>(pOrigin);
	vOrigin.punkVal->AddRef();
	reinterpret_cast<IWzVector2D__put_origin_t>(pVtbl[nVtbl_IWzVector2D__put_origin / sizeof(void*)])(pVector, vOrigin);
}

// Places a layer at a screen position - the client's own call, and the only writer that does not
// disturb the layer. The two trailing VARIANTs are not optional padding: the signature is
// `long __stdcall raw_RelMove(long x, long y, VARIANT, VARIANT)` and CWnd::CreateWnd fills both
// with an empty variant. Calling it with x/y alone leaves 32 bytes of garbage on the stack and the
// callee reads the empty slot as a pointer - that is what crashed the client once (fault: write to
// NULL at IWzGr2DLayer::GetLT+0x2E, from a stack already unwound by the wrong pop count).
static void SetLayerPosition(void* pVector, long nX, long nY) {
	void** pVtbl = *reinterpret_cast<void***>(pVector);
	VARIANTARG vAttr1;
	VARIANTARG vAttr2;
	memset(&vAttr1, 0, sizeof(vAttr1)); // VT_EMPTY: no move-path attributes, i.e. an instant move
	memset(&vAttr2, 0, sizeof(vAttr2));
	reinterpret_cast<IWzVector2D__raw_RelMove_t>(pVtbl[nVtbl_IWzVector2D__raw_RelMove / sizeof(void*)])(pVector, nX, nY, vAttr1, vAttr2);
}

// The screen origin every UI window is built under (CWnd::CreateWnd).
static void* GetScreenOrigin() {
	void* pWndMan = *reinterpret_cast<void**>(dwCWndMan__ms_pInstance);
	if (pWndMan == nullptr || IsBadReadPtr(pWndMan, nCWndMan__OrgWindow + sizeof(void*))) return nullptr;
	void* pOrgWindow = *reinterpret_cast<void**>(reinterpret_cast<char*>(pWndMan) + nCWndMan__OrgWindow);
	if (pOrgWindow == nullptr || IsBadReadPtr(pOrgWindow, sizeof(void*))) return nullptr;
	return pOrgWindow;
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
		bScreenParented = false;
		nPinnedHeight = 0;

		// One-time reparenting onto the screen frame. From here on raw_RelMove takes screen pixels,
		// which is exactly how CWnd::CreateWnd places every UI window; the layer's own x/y is then
		// frame-relative (a large negative number) and must not be used as a position.
		void* pOrgWindow = GetScreenOrigin();
		if (pOrgWindow == nullptr) {
			Log("layer=%p SCREEN_ORIGIN_MISSING - bulb left where the client put it", pLayer);
			return;
		}
		PutOrigin(pLayer, pOrgWindow);

		// Vertical centring is resolved once per layer instance: the bulb's animation frames do
		// not all have the same height, so re-centring every frame would make it jitter.
		int nTargetY = nFixedY;
		if (nTargetY < 0) {
			nPinnedHeight = reinterpret_cast<IWzGr2DLayer__GetHeight_t>(dwIWzGr2DLayer__GetHeight)(pLayer, nullptr);
			if (nPinnedHeight < 0 || nPinnedHeight > Client::m_nGameHeight) nPinnedHeight = 0;
			nTargetY = (Client::m_nGameHeight - nPinnedHeight) / 2;
			if (nTargetY < 0) nTargetY = 0;
		}
		nWantedX = (nFixedX < 0) ? 0 : nFixedX;
		nWantedY = nTargetY;
		bScreenParented = true;

		long lx = 0, ly = 0, ox = 0, oy = 0;
		GetLocalPosition(pLayer, &lx, &ly);
		GetLocalPosition(pOrgWindow, &ox, &oy);
		int nZ = reinterpret_cast<IWzGr2DLayer__GetZ_t>(dwIWzGr2DLayer__GetZ)(pLayer, nullptr);
		Log("layer=%p h=%d target=(%ld,%ld) orgWindow=%p z=%d stock=(%ld,%ld) local=(%ld,%ld)",
			pLayer, nPinnedHeight, nWantedX, nWantedY, pOrgWindow, nZ, lx - ox, ly - oy, lx, ly);
	}

	if (!bScreenParented) return;

	// Screen position = the layer's own coordinates minus the screen origin's. Verified against the
	// engine: after writing (x,y) the layer reads back local = (x,y) + origin, and the value stays
	// put as the camera moves - so this holds the bulb on one screen spot.
	long lx = 0, ly = 0, ox = 0, oy = 0;
	GetLocalPosition(pLayer, &lx, &ly);
	GetLocalPosition(GetScreenOrigin(), &ox, &oy);
	long nAbsX = lx - ox;
	long nAbsY = ly - oy;
	bool bCorrected = false;
	if (nAbsX != nWantedX || nAbsY != nWantedY) {
		SetLayerPosition(pLayer, nWantedX, nWantedY);
		bCorrected = true;
	}

	nPinCount++;
	if (bDebug && (bCorrected || nPinCount <= 5 || (nPinCount % 600) == 0)) {
		if (bCorrected) { // confirm the write really landed where it was asked to
			GetLocalPosition(pLayer, &lx, &ly);
			GetLocalPosition(GetScreenOrigin(), &ox, &oy);
			nAbsX = lx - ox;
			nAbsY = ly - oy;
		}
		Log("[%u] layer=%p screen=(%ld,%ld) wanted=(%ld,%ld) eq=%d corrected=%d local=(%ld,%ld)",
			nPinCount, pLayer, nAbsX, nAbsY, nWantedX, nWantedY,
			(nAbsX == nWantedX && nAbsY == nWantedY) ? 1 : 0, bCorrected ? 1 : 0, lx, ly);
	}
}

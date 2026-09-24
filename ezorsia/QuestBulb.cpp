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
// Moving the bulb is a two-step problem, both halves measured on a live client:
//
//   * The layer hangs off the character through its origin chain, so its own coordinates are an
//     offset inside that frame; where it lands on screen is only decided by the chain. Asking
//     GetLT (+200 wrapper 0x00440C54) for the position handle and summing x/y along get_origin
//     (+96) resolves to world/map coordinates - (739,954) and (785,1178) for a 1280x720 window -
//     which is exactly the camera-independent frame we want to leave alone.
//   * Writing a layer's coordinates with raw_Move (+64) *detaches it from that origin*: the client
//     never calls raw_Move on a layer anywhere (it positions everything with raw_RelMove, +144),
//     and CVecCtrl::raw_Move 0x009B5E7F is plainly a teleport - it zeroes the velocity fields and
//     calls SetMovePathAttribute(3). On a live layer the position handle also came back null right
//     after the call. The symptom of leaving it detached is a bulb frozen at a spot in the map
//     that the character can walk up to. put_x/put_rx are no help: CVecCtrl::put_x 0x009B5DA5
//     and put_rx 0x009B60D0 both funnel back into raw_Move/raw_RelMove.
//
// So the position is written and the origin is put back immediately afterwards. Reading the
// origin off the GetLT handle works (5 levels deep); the layer's own get_origin returns nothing.
// put_origin (+100) is safe to call on a live layer - the very first attempt proved it relocates
// the bulb without breaking its rendering. Putting the origin back also rewrites the layer's
// coordinates (measured: (-539,104) in, (-371,281) out), so the write is iterated until the value
// that survives the restore is the wanted one.
//
// The relocation itself is a constant offset: the follow camera pins the character to one spot on
// screen, so the bulb's screen position moves 1:1 with the layer's local coordinates. The target
// is expressed in screen pixels and converted against the coordinates the client itself picked for
// the bulb (see nCharScreenOffsetY); questBulbX/questBulbY are therefore plain screen coordinates,
// and any constant error is tunable from config.ini.
//
// Vtable slots are confirmed twice over in the same binary:
//   * CVecCtrl implements IWzVector2D, and the vtable it installs at this+0x0C (0x00B3E1F8,
//     written by CVecCtrl::CVecCtrl 0x009B0F71) still carries every PDB name: +32 get_x,
//     +40 get_y, +64 raw_Move, +96 get_origin, +100 put_origin, +104 get_rx, +112 get_ry,
//     +144 raw_RelMove;
//   * the stateful wrappers agree - IWzShape2D::Getx 0x00403CB7 uses +32, IWzShape2D::Gety
//     0x00403CDE uses +40, IWzVector2D::GetOrigin 0x00441AA1 uses +96.
const int nVtbl_IWzVector2D__get_x = 32;
const int nVtbl_IWzVector2D__get_y = 40;
const int nVtbl_IWzVector2D__raw_Move = 64;
const int nVtbl_IWzVector2D__get_origin = 96;
const int nVtbl_IWzVector2D__put_origin = 100;

const DWORD dwCUserLocal__UpdateQuestAlertIcon = 0x0095CED9;
const DWORD dwIWzGr2DLayer__GetLT = 0x00440C54;
const DWORD dwIWzGr2DLayer__GetHeight = 0x00440C2A;

// CUserLocal+0x3180 is the bulb's IWzGr2DLayer (com_ptr, null while no bulb is shown);
// the sibling flag CUserLocal+0x317C is not "a layer exists" - the client also loads the idle
// animation with that flag cleared - so only the pointer is used here.
const int nCUserLocal__QuestAlertLayer = 0x3180;

// The wanted position is a *screen* position, but nothing readable speaks screen coordinates -
// get_rx/get_ry only echo the layer's own x/y, and WzGr2D::GetCamera is not exported. What saves
// the day is that the client's own placement of this bulb is usable as the reference: the follow
// camera pins the character to one spot on screen, so the bulb's screen position moves 1:1 with
// the layer's local coordinates, and the spot the client itself picked for "just above the head"
// (window centre horizontally, slightly above centre vertically) is the on-screen origin to
// measure against. Everything below is therefore `clientLocal + (target - thatSpot)`.
const int nCharScreenOffsetY = 60; // head height above the vertical centre: where the client puts the bulb

// Probe step: confirms raw_Move really moves this layer, since its meaning cannot be read off
// statically (no call site anywhere in this exe).
const int nProbeDelta = 1000;
const int nProbeTolerance = 400;

typedef long(__stdcall* IWzVector2D__get_long_t)(void* pThis, long* pnOut);
typedef long(__stdcall* IWzVector2D__raw_Move_t)(void* pThis, long nX, long nY);
typedef long(__stdcall* IWzVector2D__get_origin_t)(void* pThis, VARIANTARG* pvOrigin);
typedef long(__stdcall* IWzVector2D__put_origin_t)(void* pThis, VARIANTARG vOrigin);
typedef void*(__fastcall* IWzGr2DLayer__GetLT_t)(void* pThis, void* edx, void** ppOut);
typedef int(__fastcall* IWzGr2DLayer__GetHeight_t)(void* pThis, void* edx);

bool QuestBulb::bFixed = true;
int QuestBulb::nFixedX = 10;
int QuestBulb::nFixedY = -1;
bool QuestBulb::bDebug = false;

static void* pPinnedLayer = nullptr;
static bool bPositionable = false; // stays false for a layer we must not touch
static unsigned int nPinCount = 0;
static long nWantedX = 0;     // local coordinates that put the bulb on target (per layer instance)
static long nWantedY = 0;
static long nLocalBaseX = 0;  // the local coordinates the client itself chose, i.e. where it laid
static long nLocalBaseY = 0;  // the bulb out before we touched it - the on-screen reference point
static long nAnchorScreenX = 0;
static long nAnchorScreenY = 0;
static int nPinnedHeight = 0;             // the layer height the centring was derived from
static VARIANTARG vSavedOrigin;           // the origin the client gave the layer (we own a ref)
static bool bHaveSavedOrigin = false;

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

static void ReleaseVariant(VARIANTARG& v) {
	if (v.vt == VT_UNKNOWN && v.punkVal != nullptr) v.punkVal->Release();
	else if (v.vt == VT_DISPATCH && v.pdispVal != nullptr) v.pdispVal->Release();
	memset(&v, 0, sizeof(v));
}

// get_origin answers with either pointer flavour, and may hand it back by reference.
static void* ExtractObject(const VARIANTARG& v) {
	VARTYPE vtBase = v.vt & ~VT_BYREF;
	if (vtBase != VT_UNKNOWN && vtBase != VT_DISPATCH) return nullptr;
	void* p = (vtBase == VT_UNKNOWN) ? static_cast<void*>(v.punkVal) : static_cast<void*>(v.pdispVal);
	if ((v.vt & VT_BYREF) != 0) {
		if (p == nullptr || IsBadReadPtr(p, sizeof(void*))) return nullptr;
		p = *reinterpret_cast<void**>(p);
	}
	return p;
}

static void GetLocalPosition(void* pVector, long* pnX, long* pnY) {
	void** pVtbl = *reinterpret_cast<void***>(pVector);
	*pnX = 0;
	*pnY = 0;
	reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_x / sizeof(void*)])(pVector, pnX);
	reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_y / sizeof(void*)])(pVector, pnY);
}

// Opens the layer's position handle (caller-owned, like the client treats it) and walks
// get_origin from it, summing x/y. Returns false when the handle cannot be read at all.
static void* OpenPositionHandle(void* pLayer) {
	static auto _GetLT = reinterpret_cast<IWzGr2DLayer__GetLT_t>(dwIWzGr2DLayer__GetLT);
	void* pLT = nullptr;
	_GetLT(pLayer, nullptr, &pLT);
	if (pLT == nullptr || IsBadReadPtr(pLT, sizeof(void*))) return nullptr;
	return pLT;
}

static void ClosePositionHandle(void* pHandle) {
	void** pVtbl = *reinterpret_cast<void***>(pHandle);
	reinterpret_cast<unsigned long(__stdcall*)(void*)>(pVtbl[2])(pHandle); // IUnknown::Release
}

// Sums x/y along the origin chain starting at the layer's position handle: the layer's rendered
// position in world coordinates. Diagnostics only - it tells whether the layer is still hanging
// off the character (the value keeps changing as the character walks) or has been detached
// (the value freezes), which is exactly what a position write must not do.
static bool ResolveAbsolutePosition(void* pLayer, long* pnX, long* pnY, int* pnDepth) {
	*pnX = 0;
	*pnY = 0;
	*pnDepth = 0;

	void* pCur = OpenPositionHandle(pLayer);
	if (pCur == nullptr) return false;
	reinterpret_cast<IUnknown*>(pCur)->AddRef();

	long nSumX = 0, nSumY = 0;
	int nDepth = 0;
	while (pCur != nullptr && nDepth < 8) {
		void** pVtbl = *reinterpret_cast<void***>(pCur);
		if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__get_origin + sizeof(void*))) break;

		long nX = 0, nY = 0;
		reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_x / sizeof(void*)])(pCur, &nX);
		reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_y / sizeof(void*)])(pCur, &nY);
		nSumX += nX;
		nSumY += nY;
		nDepth++;

		VARIANTARG vOrigin;
		memset(&vOrigin, 0, sizeof(vOrigin));
		long hr = reinterpret_cast<IWzVector2D__get_origin_t>(pVtbl[nVtbl_IWzVector2D__get_origin / sizeof(void*)])(pCur, &vOrigin);
		void* pParent = (hr >= 0) ? ExtractObject(vOrigin) : nullptr;
		if (pParent == pCur) pParent = nullptr;
		if (pParent != nullptr) reinterpret_cast<IUnknown*>(pParent)->AddRef();
		ReleaseVariant(vOrigin);

		reinterpret_cast<IUnknown*>(pCur)->Release();
		pCur = pParent;
	}
	if (pCur != nullptr) reinterpret_cast<IUnknown*>(pCur)->Release();

	*pnX = nSumX;
	*pnY = nSumY;
	*pnDepth = nDepth;
	return nDepth >= 2;
}

// Saves the origin the client gave the layer, so it can be put back after a position write.
static bool SaveOrigin(void* pHandle) {
	bHaveSavedOrigin = false;
	ReleaseVariant(vSavedOrigin);

	void** pVtbl = *reinterpret_cast<void***>(pHandle);
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__get_origin + sizeof(void*))) return false;

	memset(&vSavedOrigin, 0, sizeof(vSavedOrigin));
	long hr = reinterpret_cast<IWzVector2D__get_origin_t>(pVtbl[nVtbl_IWzVector2D__get_origin / sizeof(void*)])(pHandle, &vSavedOrigin);
	if (hr < 0 || ExtractObject(vSavedOrigin) == nullptr) {
		ReleaseVariant(vSavedOrigin);
		return false;
	}
	bHaveSavedOrigin = true;
	return true;
}

// Re-attaches the saved origin. raw_Move detaches the layer from it, so every write is followed
// by this; the callee takes over the reference, hence the fresh AddRef.
static void RestoreOrigin(void* pLayer) {
	if (!bHaveSavedOrigin) return;
	void** pVtbl = *reinterpret_cast<void***>(pLayer);
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__put_origin + sizeof(void*))) return;

	VARIANTARG vCopy;
	memset(&vCopy, 0, sizeof(vCopy));
	vCopy.vt = vSavedOrigin.vt;
	vCopy.punkVal = vSavedOrigin.punkVal;
	if (vCopy.vt == VT_UNKNOWN && vCopy.punkVal != nullptr) vCopy.punkVal->AddRef();
	else if (vCopy.vt == VT_DISPATCH && vCopy.pdispVal != nullptr) vCopy.pdispVal->AddRef();
	else return;
	reinterpret_cast<IWzVector2D__put_origin_t>(pVtbl[nVtbl_IWzVector2D__put_origin / sizeof(void*)])(pLayer, vCopy);
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
	if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__put_origin + sizeof(void*))) return;

	if (pPinnedLayer != pLayer) {
		long lx = 0, ly = 0, px = 0, py = 0;
		GetLocalPosition(pLayer, &lx, &ly);

		// The origin has to be saved before anything is written, otherwise there is no way back.
		void* pLT = OpenPositionHandle(pLayer);
		bool bOriginSaved = (pLT != nullptr) && SaveOrigin(pLT);
		if (pLT != nullptr) ClosePositionHandle(pLT);

		// Confirm raw_Move really moves this layer, then undo both the move and the detachment.
		bool bMoveWorks = false;
		if (bOriginSaved) {
			reinterpret_cast<IWzVector2D__raw_Move_t>(pVtbl[nVtbl_IWzVector2D__raw_Move / sizeof(void*)])(pLayer, lx + nProbeDelta, ly);
			GetLocalPosition(pLayer, &px, &py);
			reinterpret_cast<IWzVector2D__raw_Move_t>(pVtbl[nVtbl_IWzVector2D__raw_Move / sizeof(void*)])(pLayer, lx, ly);
			RestoreOrigin(pLayer);
			long nMoved = px - lx;
			bMoveWorks = (nMoved > nProbeDelta - nProbeTolerance) && (nMoved < nProbeDelta + nProbeTolerance);
		}

		pPinnedLayer = pLayer;
		nPinCount = 0;
		bPositionable = bOriginSaved && bMoveWorks;

		if (!bPositionable) {
			Log("probe layer=%p UNPOSITIONABLE (originSaved=%d move %ld->%ld) - bulb left as the client made it",
				pLayer, bOriginSaved ? 1 : 0, lx, px);
			return;
		}

		// Vertical centring is resolved once per layer instance: the bulb's animation frames do
		// not all have the same height, so re-centring every frame would make it jitter.
		nPinnedHeight = 0;
		int nTargetY = nFixedY;
		if (nTargetY < 0) {
			nPinnedHeight = reinterpret_cast<IWzGr2DLayer__GetHeight_t>(dwIWzGr2DLayer__GetHeight)(pLayer, nullptr);
			if (nPinnedHeight < 0 || nPinnedHeight > Client::m_nGameHeight) nPinnedHeight = 0;
			nTargetY = (Client::m_nGameHeight - nPinnedHeight) / 2;
			if (nTargetY < 0) nTargetY = 0;
		}
		int nTargetX = (nFixedX < 0) ? 0 : nFixedX;

		// The client's own coordinates for this bulb are the on-screen reference: they put it
		// just above the character's head, and the camera keeps the character on one spot, so
		// nAnchorScreen is where the bulb would appear if it were left alone. Since the origin
		// chain is 1:1, adding (target - anchor) moves it by exactly that many screen pixels.
		nLocalBaseX = lx;
		nLocalBaseY = ly;
		nAnchorScreenX = Client::m_nGameWidth / 2;
		nAnchorScreenY = Client::m_nGameHeight / 2 - nCharScreenOffsetY;
		nWantedX = lx + (nTargetX - nAnchorScreenX);
		nWantedY = ly + (nTargetY - nAnchorScreenY);

		Log("probe layer=%p h=%d anchor=(%ld,%ld) target=(%d,%d) => wanted=(%ld,%ld) (client had (%ld,%ld))",
			pLayer, nPinnedHeight, nAnchorScreenX, nAnchorScreenY, nTargetX, nTargetY,
			nWantedX, nWantedY, lx, ly);
	}

	if (!bPositionable) return; // never touch a layer we cannot put back

	// bulbScreen = anchor + (local - localBase): the chain is 1:1, so the layer's own coordinates
	// are the only thing that has to be driven, and they have to end up on the constant nWanted.
	//
	// One wrinkle: putting the origin back rewrites those coordinates. Measured on a live client:
	// writing (-539,104) came back as (-371,281), an extra (168,177) - the same x-rx gap the layer
	// reports on its own. So the write is iterated until the *post-restore* coordinates are the
	// wanted ones; each round compensates whatever the restore added on top.
	long nWriteX = nWantedX;
	long nWriteY = nWantedY;
	for (int i = 0; i < 3; i++) {
		reinterpret_cast<IWzVector2D__raw_Move_t>(pVtbl[nVtbl_IWzVector2D__raw_Move / sizeof(void*)])(pLayer, nWriteX, nWriteY);
		RestoreOrigin(pLayer);

		long lx = 0, ly = 0;
		GetLocalPosition(pLayer, &lx, &ly);
		if (lx == nWantedX && ly == nWantedY) break;
		nWriteX -= (lx - nWantedX);
		nWriteY -= (ly - nWantedY);
	}

	if (bDebug && (++nPinCount <= 5 || (nPinCount % 300) == 0)) {
		long lx = 0, ly = 0, ax = 0, ay = 0;
		int nDepth = 0;
		GetLocalPosition(pLayer, &lx, &ly);
		bool bResolved = ResolveAbsolutePosition(pLayer, &ax, &ay, &nDepth);
		Log("[%u] layer=%p local=(%ld,%ld) wanted=(%ld,%ld) wrote=(%ld,%ld) screen~=(%ld,%ld) world=(%ld,%ld) depth=%d resolved=%d",
			nPinCount, pLayer, lx, ly, nWantedX, nWantedY, nWriteX, nWriteY,
			nAnchorScreenX + (lx - nLocalBaseX), nAnchorScreenY + (ly - nLocalBaseY),
			ax, ay, nDepth, bResolved ? 1 : 0);
	}
}

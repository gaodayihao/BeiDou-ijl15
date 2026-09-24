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
// CUser::LoadLayer (0x00941417, `int CUser::LoadLayer(Ztl_bstr_t, int, USERLAYER&, long*)`) hands
// the new layer the avatar layer under the face as its coordinate origin
// (CAvatar::GetLayerUnderFace, 0x00941A?? -> sub_43EA3E). That is the whole problem: the bulb's
// own coordinates are then an offset in *world/map* space, so it drifts with the camera and
// inherits the walk animation's bobbing. Neither can be fixed by writing coordinates - every
// writer on that interface is a teleport (see below).
//
// What the client itself does for anything that must sit at a fixed screen position is the answer,
// and it is plain to read in CWnd::CreateWnd (0x009DE4D2), which builds every UI window:
//
//     sub_410FDF(&v, *(CWndMan::ms_pInstance + 0xDC), 1);   // VARIANT{VT_UNKNOWN, org window}
//     IWzVector2D::put_origin (+100)  <- that VARIANT            // child of the SCREEN origin
//     IWzVector2D::raw_RelMove (+144) <- nLeft, nTop, empty, empty  // position *inside* that frame
//     IWzGr2DLayer::PutZ (+180) / Putcolor (+224)
//
// Slot signatures are read off the mangled names in this exe, not guessed - a wrong argument
// count on a vtable call corrupts the stack silently:
//   ?raw_Move@CVecCtrl@@UAGJJJ@Z                    long raw_Move(long x, long y)
//   ?raw_RelMove@CVecCtrl@@UAGJJJUtagVARIANT@@0@Z   long raw_RelMove(long x, long y, VARIANT, VARIANT)
//   ?put_origin@CVecCtrl@@UAGJUtagVARIANT@@@Z       long put_origin(VARIANT)
//   ?get_origin@CVecCtrl@@UAGJPAUtagVARIANT@@@Z     long get_origin(VARIANT* pOut)
//   ?get_x@CVecCtrl@@UAGJPAJ@Z                      long get_x(long* pOut)
//
// `CWndMan+0xDC` is what CWndMan::GetOrgWindow (0x0048BBA5) returns - it ignores its UIOrigin
// argument and always answers with that field, and CUIStatusBar::OnCreate (0x008D01B2) hangs the
// HUD off it too. Its frame *is* screen space, so a layer parented to it is positioned in screen
// pixels. CChatBalloon::MakeScreenBalloon (0x0048B86A) is the same recipe for a sprite that is not
// a window: IWzGr2D::CreateLayer at screen coordinates, then put_origin(org window).
//
// The setter matters as much as the origin. On IWzVector2D:
//
//   raw_Move     (+64)  teleport: CVecCtrl::raw_Move 0x009B5E7F zeroes the velocity fields and
//                       calls SetMovePathAttribute(3), and on a live layer GetLT comes back null
//                       straight after - it *detaches the layer from its origin*. Writing the
//                       bulb's coordinates with it froze the bulb at a spot in the map (character
//                       could walk up to it), and compensating the detach with put_origin made it
//                       wobble instead, because put_origin rewrites those coordinates
//                       (measured: (-539,104) in, (-371,281) out).
//   raw_RelMove  (+144) the client's own "put this layer here" call, always against the layer's
//                       current origin - exactly what CWnd::CreateWnd uses right after
//                       put_origin. Nothing is detached, so nothing has to be restored.
//
// hence the implementation below: re-point the origin once per layer instance, then write the
// position (and only when it is actually off) with raw_RelMove.
//
// Vtable slots are confirmed twice over in the same binary:
//   * CVecCtrl implements IWzVector2D, and the vtable it installs at this+0x0C (0x00B3E1F8,
//     written by CVecCtrl::CVecCtrl 0x009B0F71) still carries every PDB name: +32 get_x,
//     +40 get_y, +96 get_origin, +100 put_origin, +104 get_rx, +112 get_ry, +144 raw_RelMove;
//   * the stateful wrappers agree - IWzShape2D::Getx 0x00403CB7 uses +32, IWzShape2D::Gety
//     0x00403CDE uses +40, IWzVector2D::GetOrigin 0x00441AA1 uses +96.
const int nVtbl_IWzVector2D__get_x = 32;
const int nVtbl_IWzVector2D__get_y = 40;
const int nVtbl_IWzVector2D__get_origin = 96;
const int nVtbl_IWzVector2D__put_origin = 100;
const int nVtbl_IWzVector2D__raw_RelMove = 144;

const DWORD dwCUserLocal__UpdateQuestAlertIcon = 0x0095CED9;
const DWORD dwIWzGr2DLayer__GetLT = 0x00440C54;
const DWORD dwIWzGr2DLayer__GetHeight = 0x00440C2A;
const DWORD dwIWzGr2DLayer__GetZ = 0x0044337D;
const DWORD dwCWndMan__ms_pInstance = 0x00BEC20C;

// CWndMan::GetOrgWindow 0x0048BBA5 answers with this field, whatever UIOrigin it is asked for.
const int nCWndMan__OrgWindow = 0xDC;

// Where the client's own placement puts the bulb: just above the character's head, and the follow
// camera keeps the character on one screen spot, so that spot is the reference the target is
// measured against. Only the difference matters, so a constant error here is a constant error in
// the landing position - tuneable with questBulbX/questBulbY.
const int nCharScreenOffsetY = 60;

// CUserLocal+0x3180 is the bulb's IWzGr2DLayer (com_ptr, null while no bulb is shown);
// the sibling flag CUserLocal+0x317C is not "a layer exists" - the client also loads the idle
// animation with that flag cleared - so only the pointer is used here.
const int nCUserLocal__QuestAlertLayer = 0x3180;

typedef long(__stdcall* IWzVector2D__get_long_t)(void* pThis, long* pnOut);
typedef long(__stdcall* IWzVector2D__raw_RelMove_t)(void* pThis, long nX, long nY, VARIANTARG vAttr1, VARIANTARG vAttr2);
typedef long(__stdcall* IWzVector2D__get_origin_t)(void* pThis, VARIANTARG* pvOrigin);
typedef long(__stdcall* IWzVector2D__put_origin_t)(void* pThis, VARIANTARG vOrigin);
typedef void*(__fastcall* IWzGr2DLayer__GetLT_t)(void* pThis, void* edx, void** ppOut);
typedef int(__fastcall* IWzGr2DLayer__GetHeight_t)(void* pThis, void* edx);
typedef int(__fastcall* IWzGr2DLayer__GetZ_t)(void* pThis, void* edx);

bool QuestBulb::bFixed = true;
int QuestBulb::nFixedX = 10;
int QuestBulb::nFixedY = -1;
bool QuestBulb::bDebug = false;

static void* pPinnedLayer = nullptr;
static bool bScreenParented = false; // false for a layer we must not touch
static unsigned int nPinCount = 0;
static long nWantedX = 0;        // screen position the bulb is pinned to (per layer instance)
static long nWantedY = 0;
static long nAnchorScreenX = 0;  // where the client's own placement put the bulb on screen
static long nAnchorScreenY = 0;
static int nPinnedHeight = 0;    // the layer height the vertical centring was derived from

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

// Opens the layer's position handle (caller-owned, like the client treats it).
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

// Moves a layer inside its current origin frame - the client's own positioning call.
//
// The two trailing VARIANTs are not optional padding: the real signature is
// `long __stdcall raw_RelMove(long x, long y, VARIANT, VARIANT)` (mangled
// `?raw_RelMove@CVecCtrl@@UAGJJJUtagVARIANT@@0@Z`), and CWnd::CreateWnd fills both with an empty
// variant. Calling it with x/y alone leaves 32 bytes of garbage on the stack and the callee reads
// the empty slot as a pointer - that is what crashed the client in the seventh test round
// (fault: write to NULL at IWzGr2DLayer::GetLT+0x2E, from a stack that had already been unwound
// by the wrong pop count).
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

// Sums x/y along the origin chain starting at the given vector (the start is borrowed; it is
// AddRef'd here). Diagnostics only: on the screen origin it reveals which convention the UI frame
// uses - (0,0) for plain screen pixels, or (-width/2,-height/2) for the centre-based frame the
// CWndMan constructor sets up. Both give the same target, since the frame's own offset and the
// renderer's centre are the same shift.
static bool ChainSum(void* pStart, long* pnX, long* pnY, int* pnDepth) {
	*pnX = 0;
	*pnY = 0;
	*pnDepth = 0;
	if (pStart == nullptr || IsBadReadPtr(pStart, sizeof(void*))) return false;

	reinterpret_cast<IUnknown*>(pStart)->AddRef();
	void* pCur = pStart;
	long nSumX = 0, nSumY = 0;
	int nDepth = 0;
	while (pCur != nullptr && nDepth < 8) {
		void** pVtbl = *reinterpret_cast<void***>(pCur);
		if (pVtbl == nullptr || IsBadReadPtr(pVtbl, nVtbl_IWzVector2D__get_origin + sizeof(void*))) break;

		long nX = 0, nY = 0;
		GetLocalPosition(pCur, &nX, &nY);
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

// Same, starting from a layer's position handle.
static bool ResolveAbsolutePosition(void* pLayer, long* pnX, long* pnY, int* pnDepth) {
	void* pLT = OpenPositionHandle(pLayer);
	if (pLT == nullptr) return false;
	bool bOk = ChainSum(pLT, pnX, pnY, pnDepth);
	ClosePositionHandle(pLT);
	return bOk;
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
		pPinnedLayer = nullptr; // bulb taken down: the next one gets prepared from scratch
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

		// One-time reparenting: from here on the layer's coordinates live in the screen frame, so
		// nothing has to be re-derived as the character walks.
		void* pOrgWindow = GetScreenOrigin();
		if (pOrgWindow == nullptr) {
			Log("layer=%p SCREEN_ORIGIN_MISSING - bulb left where the client put it", pLayer);
			return;
		}
		long lxBefore = 0, lyBefore = 0;
		GetLocalPosition(pLayer, &lxBefore, &lyBefore);
		PutOrigin(pLayer, pOrgWindow);

		// The screen frame has a bias of its own (CWnd::GetAbsLeft reads it back as
		// `layer x - org window x`), and the screen origin's own coordinates are not readable.
		// So the target is not written as an absolute screen position but as a shift from where
		// the client's own placement lands *inside the new frame*: the bulb is one spot on screen
		// before the reparenting and the anchor is where that spot is (window centre horizontally,
		// head height above the vertical centre), and the frame is 1:1 with screen pixels.
		long lxAnchored = 0, lyAnchored = 0;
		GetLocalPosition(pLayer, &lxAnchored, &lyAnchored);
		bScreenParented = true;

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
		nAnchorScreenX = Client::m_nGameWidth / 2;
		nAnchorScreenY = Client::m_nGameHeight / 2 - nCharScreenOffsetY;
		nWantedX = lxAnchored + (nTargetX - nAnchorScreenX);
		nWantedY = lyAnchored + (nTargetY - nAnchorScreenY);

		int nZ = reinterpret_cast<IWzGr2DLayer__GetZ_t>(dwIWzGr2DLayer__GetZ)(pLayer, nullptr);
		long ox = 0, oy = 0;
		GetLocalPosition(pOrgWindow, &ox, &oy);
		Log("layer=%p h=%d anchor=(%d,%d) target=(%d,%d) wanted=(%ld,%ld) before=(%ld,%ld) anchored=(%ld,%ld) orgOwn=(%ld,%ld) orgWindow=%p z=%d",
			pLayer, nPinnedHeight, nAnchorScreenX, nAnchorScreenY, nTargetX, nTargetY,
			nWantedX, nWantedY, lxBefore, lyBefore, lxAnchored, lyAnchored, ox, oy, pOrgWindow, nZ);
	}

	if (!bScreenParented) return;

	// Write only when the position is actually off. In screen space the client has no reason to
	// move this layer again, so after the first frame this normally does nothing at all - and a
	// write that is not needed is a write that only adds jitter.
	long lx = 0, ly = 0;
	GetLocalPosition(pLayer, &lx, &ly);
	bool bCorrected = false;
	if (lx != nWantedX || ly != nWantedY) {
		SetLayerPosition(pLayer, nWantedX, nWantedY);
		GetLocalPosition(pLayer, &lx, &ly);
		bCorrected = true;
	}

	nPinCount++;
	if (bDebug && (bCorrected || nPinCount <= 5 || (nPinCount % 600) == 0)) {
		long ax = 0, ay = 0;
		int nDepth = 0;
		bool bResolved = ResolveAbsolutePosition(pLayer, &ax, &ay, &nDepth);
		Log("[%u] layer=%p local=(%ld,%ld) wanted=(%ld,%ld) corrected=%d screen=(%ld,%ld) depth=%d resolved=%d",
			nPinCount, pLayer, lx, ly, nWantedX, nWantedY, bCorrected ? 1 : 0, ax, ay, nDepth, bResolved ? 1 : 0);
	}
}

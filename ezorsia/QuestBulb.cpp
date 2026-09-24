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
// CUser::LoadLayer hands the new layer a *character* coordinate origin, so the bulb moves with
// the head. The client already has a screen-space origin for exactly this kind of job: the
// vector returned by CWndMan::GetOrgWindow (0x0048BBA5, i.e. CWndMan+0xDC), which is what
// CField::ShowScreenEffect (0x00534F92) and CChatBalloon::MakeScreenBalloon (0x0048B86A) pass
// as the origin of their own screen effects. Re-pointing our layer at that vector and moving it
// to a fixed (x, y) is therefore the same mechanism the client uses itself.
//
// IWzVector2D vtable slots below are confirmed twice over in the same binary:
//   * CVecCtrl implements that interface, and the vtable it installs at this+0x0C
//     (0x00B3E1F8, written by CVecCtrl::CVecCtrl 0x009B0F71) still carries every PDB name:
//     +32 get_x / +40 get_y / +64 raw_Move / +96 get_origin / +100 put_origin / +144 raw_RelMove;
//   * the stateful wrappers agree - IWzShape2D::Getx 0x00403CB7 uses +32, IWzShape2D::Gety
//     0x00403CDE uses +40, IWzVector2D::GetOrigin 0x00441AA1 uses +96, and the client's own
//     CChatBalloon::MakeScreenBalloon 0x0048B86A re-origins a freshly created layer with +100.
const int nVtbl_IWzVector2D__get_x = 32;
const int nVtbl_IWzVector2D__get_y = 40;
const int nVtbl_IWzVector2D__raw_Move = 64;
const int nVtbl_IWzVector2D__put_origin = 100;

const DWORD dwCUserLocal__UpdateQuestAlertIcon = 0x0095CED9;
const DWORD dwTSingleton_CWndMan__ms_pInstance = 0x00BEC20C;
const DWORD dwIWzGr2DLayer__GetHeight = 0x00440C2A; // IWzGr2DLayer::GetHeight wrapper, __thiscall

// CUserLocal+0x3180 is the bulb's IWzGr2DLayer (com_ptr, null while no bulb is shown);
// the sibling flag CUserLocal+0x317C is not "a layer exists" - the client also loads the idle
// animation with that flag cleared - so only the pointer is used here.
const int nCUserLocal__QuestAlertLayer = 0x3180;

// CWndMan+0xDC is exactly what CWndMan::GetOrgWindow returns (it ignores its UIOrigin argument).
const int nCWndMan__OrgWindow = 0xDC;

typedef long(__stdcall* IWzVector2D__get_long_t)(void* pThis, long* pnOut);
typedef long(__stdcall* IWzVector2D__raw_Move_t)(void* pThis, long nX, long nY);
typedef long(__stdcall* IWzVector2D__put_origin_t)(void* pThis, VARIANTARG vOrigin);
typedef int(__fastcall* IWzGr2DLayer__GetHeight_t)(void* pThis, void* edx);

bool QuestBulb::bFixed = true;
int QuestBulb::nFixedX = 10;
int QuestBulb::nFixedY = -1;
bool QuestBulb::bDebug = false;

// The layer we already re-origined. put_origin AddRefs the screen origin vector (the client
// passes it the same way, see sub_410FDF(..., 1)), and the callee owns that reference, so it is
// set once per layer instance instead of once per frame.
static void* pPinnedLayer = nullptr;
static unsigned int nPinCount = 0;
static int nPinnedY = 0;       // resolved once per layer instance (see PinLayer)
static int nPinnedHeight = 0;  // the layer height that nPinnedY was derived from (diagnostics)

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
		pPinnedLayer = nullptr; // bulb taken down: the next one gets origined again
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
		void* pWndMan = *reinterpret_cast<void**>(dwTSingleton_CWndMan__ms_pInstance);
		if (pWndMan == nullptr) return;
		void* pOrgWindow = *reinterpret_cast<void**>(reinterpret_cast<char*>(pWndMan) + nCWndMan__OrgWindow);
		if (pOrgWindow == nullptr || IsBadReadPtr(pOrgWindow, sizeof(void*))) return;

		// put_origin(VARIANT) - the client builds the same VT_UNKNOWN variant through
		// sub_410FDF(variant, pointer, 1), which also AddRefs before handing the reference over.
		VARIANTARG vOrigin;
		memset(&vOrigin, 0, sizeof(vOrigin));
		vOrigin.vt = VT_UNKNOWN;
		vOrigin.punkVal = reinterpret_cast<IUnknown*>(pOrgWindow);
		vOrigin.punkVal->AddRef();
		reinterpret_cast<IWzVector2D__put_origin_t>(pVtbl[nVtbl_IWzVector2D__put_origin / sizeof(void*)])(pLayer, vOrigin);

		// Resolve the y target once per layer instance: the bulb's animation frames are not all
		// the same height, so re-centring on the live height every frame would make it jitter.
		nPinnedHeight = 0;
		nPinnedY = nFixedY;
		if (nPinnedY < 0) {
			nPinnedHeight = reinterpret_cast<IWzGr2DLayer__GetHeight_t>(dwIWzGr2DLayer__GetHeight)(pLayer, nullptr);
			if (nPinnedHeight < 0 || nPinnedHeight > Client::m_nGameHeight) nPinnedHeight = 0;
			nPinnedY = (Client::m_nGameHeight - nPinnedHeight) / 2;
			if (nPinnedY < 0) nPinnedY = 0;
		}

		pPinnedLayer = pLayer;
		nPinCount = 0;
	}

	int nX = (nFixedX < 0) ? 0 : nFixedX;

	reinterpret_cast<IWzVector2D__raw_Move_t>(pVtbl[nVtbl_IWzVector2D__raw_Move / sizeof(void*)])(pLayer, nX, nPinnedY);

	// Diagnostics (quest_bulb.log): what raw_Move was asked for, plus what the layer reports back.
	// A readback that is not (nX, nPinnedY) means get_x/get_y are not in the origin's frame -
	// useful when tuning, harmless otherwise.
	if (bDebug && (++nPinCount <= 5 || (nPinCount % 300) == 0)) {
		long nBackX = -1, nBackY = -1;
		reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_x / sizeof(void*)])(pLayer, &nBackX);
		reinterpret_cast<IWzVector2D__get_long_t>(pVtbl[nVtbl_IWzVector2D__get_y / sizeof(void*)])(pLayer, &nBackY);
		Log("[%u] layer=%p h=%d move=(%d,%d) readback=(%ld,%ld)",
			nPinCount, pLayer, nPinnedHeight, nX, nPinnedY, nBackX, nBackY);
	}
}

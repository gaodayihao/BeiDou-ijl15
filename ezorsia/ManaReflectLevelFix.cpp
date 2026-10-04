#include "stdafx.h"
#include "ManaReflectLevelFix.h"
#include "Memory.h"
#include <stdarg.h>
#include <intrin.h>

// ---------------------------------------------------------------------------------------------
// Mana Reflection reflect-rate fix - see ManaReflectLevelFix.h for why.
//
// Prototypes are read off the shipped binary:
//   * SKILLENTRY::GetLevelData (0x00760F23) - ?GetLevelData@SKILLENTRY@@QBEABUSKILLLEVELDATA@@J@Z,
//     __thiscall with one stack argument and a single `retn 4` (0x00761188): ecx = the SKILLENTRY*,
//     the argument is the level. Returns a pointer to the SKILLLEVELDATA row in eax.
//   * CSkillInfo::GetSkill (0x0075C755) - __thiscall with one stack argument (`retn 4`), ecx = the
//     CSkillInfo singleton (*0x00BE78DC).
//   * CSkillInfo::GetSkillLevel (0x007616F6) - __thiscall, ecx = the singleton, arguments =
//     (const CharacterData&, skill id, SKILLENTRY** out).
//   * CWvsContext::GetCharacterData (0x00425D0B) - returns a ZRef whose payload is the second dword.
// The SEH / C++-EH split around the COM-flavoured calls is the same one PetAutoBuff.cpp uses for the very
// same helpers (an escaping _com_error takes the client down silently).
// ---------------------------------------------------------------------------------------------

static const DWORD ADDR_SKILLENTRY_GetLevelData = 0x00760F23;
static const DWORD ADDR_CSkillInfo_GetSkill = 0x0075C755;
static const DWORD ADDR_CSkillInfo_GetSkillLevel = 0x007616F6;
static const DWORD ADDR_CWvsContext_GetCharacterData = 0x00425D0B;
static const DWORD ADDR_CWvsContext_Instance = 0x00BE7918;
static const DWORD ADDR_SkillInfoInstance = 0x00BE78DC;

// The id the client hard-codes in the reflect branch, and the two siblings a character may have instead.
static const int MANA_REFLECT_FP = 2121002;
static const int MANA_REFLECT_IL = 2221002;
static const int MANA_REFLECT_BISHOP = 2321002;

// The Mana Reflection block of CUserLocal::SetDamaged (0x00958D50-0x00958E12). Its two GetLevelData call
// sites return to 0x00958DCC (chance/prop) and 0x00958E17 (rate/x), so the return address alone identifies
// the calls the fix has to serve - which keeps it working even if the entry pointer cannot be resolved.
static const DWORD ADDR_SetDamaged_MRBlock_Lo = 0x00958D50;
static const DWORD ADDR_SetDamaged_MRBlock_Hi = 0x00958E30;

typedef const void*(__fastcall* tGetLevelData)(void* pEntry, void* edx, int nLevel);
typedef void*(__fastcall* tGetSkill)(void* pSkillInfo, void* edx, int nSkillId);

static tGetLevelData g_origGetLevelData = nullptr;
static void* g_pFpEntry = nullptr;   // CSkillInfo::GetSkill(2121002), cached once at install time
static bool g_enabled = false;

static void FixLog(const char* sFormat, ...) {
	char sBody[512];
	va_list args;
	va_start(args, sFormat);
	_vsnprintf_s(sBody, sizeof(sBody), _TRUNCATE, sFormat, args);
	va_end(args);

	SYSTEMTIME st;
	GetLocalTime(&st);

	FILE* pFile = nullptr;
	if (fopen_s(&pFile, "mana_reflect_fix.log", "a") == 0 && pFile != nullptr) {
		fprintf(pFile, "%02d:%02d:%02d.%03d %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, sBody);
		fclose(pFile);
	}
}

// ---- ABI helpers (same shape as PetAutoBuff.cpp's, for the same client calls) ----

static void* GetCharacterDataRaw(void* pWvsContext) {
	try {
		unsigned char aRef[16];
		memset(aRef, 0, sizeof(aRef));
		typedef void(__thiscall* GetCharacterData_t)(void* pThis, void* pRetBuf);
		reinterpret_cast<GetCharacterData_t>(ADDR_CWvsContext_GetCharacterData)(pWvsContext, aRef);
		return *reinterpret_cast<void**>(aRef + 4);
	}
	catch (...) {
		return nullptr;
	}
}

static void* GetCharacterData(void* pWvsContext) {
	__try {
		return GetCharacterDataRaw(pWvsContext);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

static int GetSkillLevelRaw(void* pSkillInfo, void* pCharacterData, int nSkillId) {
	try {
		typedef long(__thiscall* GetSkillLevel_t)(void* pThis, const void* pCharacterData, long nSkillId, void** ppEntry);
		const long nLevel = reinterpret_cast<GetSkillLevel_t>(ADDR_CSkillInfo_GetSkillLevel)(
			pSkillInfo, pCharacterData, nSkillId, nullptr);
		return nLevel > 0 ? static_cast<int>(nLevel) : 0;
	}
	catch (...) {
		return 0;
	}
}

static int GetSkillLevel(void* pCharacterData, int nSkillId) {
	if (pCharacterData == nullptr) {
		return 0;
	}

	__try {
		void* pInfo = *reinterpret_cast<void**>(ADDR_SkillInfoInstance);
		if (pInfo == nullptr) {
			return 0;
		}
		return GetSkillLevelRaw(pInfo, pCharacterData, nSkillId);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		return 0;
	}
}

// Whichever Mana Reflection the character actually has (F/P, I/L or Bishop), or 0 when none is learned.
static int GetCharacterManaReflectLevel(void* pCharacterData) {
	const int aIds[3] = { MANA_REFLECT_FP, MANA_REFLECT_IL, MANA_REFLECT_BISHOP };
	for (int i = 0; i < 3; ++i) {
		const int nLevel = GetSkillLevel(pCharacterData, aIds[i]);
		if (nLevel > 0) {
			return nLevel;
		}
	}
	return 0;
}

// The entry the reflect branch looks up. It cannot be cached from DllMain - the client's skill table is
// not loaded yet at that point (GetSkill returns null) - so resolve it on first use inside the game.
static void* ResolveFpEntry() {
	if (g_pFpEntry != nullptr) {
		return g_pFpEntry;
	}

	void* pSkillInfo = *reinterpret_cast<void**>(ADDR_SkillInfoInstance);
	if (pSkillInfo == nullptr) {
		return nullptr;
	}

	g_pFpEntry = reinterpret_cast<tGetSkill>(ADDR_CSkillInfo_GetSkill)(pSkillInfo, nullptr, MANA_REFLECT_FP);
	return g_pFpEntry;
}

static const void* __fastcall GetLevelData_Hook(void* pEntry, void* /*edx*/, int nLevel) {
	if (g_enabled) {
		const DWORD dwReturn = reinterpret_cast<DWORD>(_ReturnAddress());
		const bool bFromMrBlock = dwReturn >= ADDR_SetDamaged_MRBlock_Lo && dwReturn <= ADDR_SetDamaged_MRBlock_Hi;
		void* pTarget = ResolveFpEntry();
		const bool bIsTargetEntry = pTarget != nullptr && pEntry == pTarget;

		if (bFromMrBlock || bIsTargetEntry) {
			// Queried on every call, never cached: the character's level changes as the skill is levelled
			// up, and the fix has to follow it. Only the WZ entry pointer above is cached, and that is
			// static skill data independent of the level.
			void* pWvsContext = *reinterpret_cast<void**>(ADDR_CWvsContext_Instance);
			void* pCharacterData = pWvsContext != nullptr ? GetCharacterData(pWvsContext) : nullptr;
			const int nRealLevel = GetCharacterManaReflectLevel(pCharacterData);
			if (nRealLevel > 0 && nRealLevel != nLevel) {
				// One line per distinct level, so a level-up shows up in the log without one line per hit.
				static int nLoggedLevel = -1;
				if (nRealLevel != nLoggedLevel) {
					nLoggedLevel = nRealLevel;
					FixLog("[fix] Mana Reflection level row %d -> %d (%s)", nLevel, nRealLevel,
						bFromMrBlock ? "mr-block" : "entry");
				}
				nLevel = nRealLevel;
			}
		}
	}

	return g_origGetLevelData(pEntry, nullptr, nLevel);
}

void Hook_ManaReflectLevelFix(bool enable) {
	g_enabled = enable;

	if (!enable) {
		FixLog("[fix] module ran but is disabled by config (manaReflectFix=false)");
		std::cout << "mana reflect level fix disabled by config (manaReflectFix=false)" << std::endl;
		return;
	}

	// Sanity: this client build starts the function with "mov eax, imm32" (B8).
	if (*reinterpret_cast<unsigned char*>(ADDR_SKILLENTRY_GetLevelData) != 0xB8) {
		FixLog("[fix] skipped: unexpected client build at 0x%08X (first byte 0x%02X)",
			ADDR_SKILLENTRY_GetLevelData, *reinterpret_cast<unsigned char*>(ADDR_SKILLENTRY_GetLevelData));
		std::cout << "mana reflect level fix skipped: unexpected client build at 0x" << std::hex
			<< ADDR_SKILLENTRY_GetLevelData << std::dec << std::endl;
		return;
	}

	// The entry pointer cannot be resolved here (DllMain runs before the client's skill table is loaded),
	// so it is resolved lazily on the first in-game call - see ResolveFpEntry.
	void* pSkillInfo = *reinterpret_cast<void**>(ADDR_SkillInfoInstance);

	g_origGetLevelData = reinterpret_cast<tGetLevelData>(ADDR_SKILLENTRY_GetLevelData);
	if (Memory::SetHook(true, reinterpret_cast<void**>(&g_origGetLevelData),
		reinterpret_cast<void*>(&GetLevelData_Hook))) {
		FixLog("[fix] installed (GetLevelData=0x%08X skillinfo=0x%08X entry resolved lazily)",
			ADDR_SKILLENTRY_GetLevelData, reinterpret_cast<DWORD>(pSkillInfo));
		std::cout << "mana reflect level fix hook created (mana_reflect_fix.log)" << std::endl;
	}
	else {
		FixLog("[fix] FAILED to hook SKILLENTRY::GetLevelData at 0x%08X", ADDR_SKILLENTRY_GetLevelData);
		std::cout << "mana reflect level fix hook FAILED" << std::endl;
	}
}

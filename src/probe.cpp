// Log-free hooks on single vtable slots. Each slot gets a small thunk that reports the call to the observer and then
// continues to the original function with every register, the stack and the flags exactly as they were.
#include "probe.h"
#include "log.h"
#include "observe.h"
#include <windows.h>
#include <cstdio>
#include <cstring>

struct Probe { char label[56]; uint32_t slotAddr, original; int role; };
static const size_t kMaxProbes = 64;
static Probe g_probes[kMaxProbes]; static size_t g_n;
static uint8_t* g_mem; static size_t g_used; static const size_t kCap = 8192;

// Called from every thunk: idx, the original this (ecx), and a pointer to the original return address / stack arguments.
extern "C" void __cdecl LogHit(uint32_t idx, uint32_t thisPtr, uint32_t* stack) { ObserveOnHit(g_probes[idx].role, thisPtr, stack); }

static uint8_t* Alloc(size_t n) {
	if (!g_mem) g_mem = (uint8_t*)VirtualAlloc(nullptr, kCap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_mem || g_used + n > kCap) return nullptr;
	uint8_t* p = g_mem + g_used; g_used += (n + 15) & ~size_t(15); return p;
}
static void Put32(uint8_t*& p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
// pushad; pushfd; lea eax,[esp+24h]; push eax; push ecx; push idx; call LogHit; add esp,0Ch; popfd; popad; jmp original
static uint8_t* EmitThunk(uint32_t idx, uint32_t continueTo) {
	uint8_t* start = Alloc(48); if (!start) return nullptr; uint8_t* p = start;
	*p++ = 0x60; *p++ = 0x9C; *p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24; *p++ = 0x50; *p++ = 0x51; *p++ = 0x68; Put32(p, idx);
	*p++ = 0xB8; Put32(p, (uint32_t)(uintptr_t)&LogHit); *p++ = 0xFF; *p++ = 0xD0; *p++ = 0x83; *p++ = 0xC4; *p++ = 0x0C; *p++ = 0x9D; *p++ = 0x61;
	*p++ = 0xE9; Put32(p, continueTo - ((uint32_t)(uintptr_t)p + 4));
	return start;
}
static int RoleFor(const char* label) {
	if (strcmp(label, "Character[206]") == 0 || strcmp(label, "Creature[206]") == 0 || strcmp(label, "PlayerCharacter[206]") == 0) return kRoleDamage;
	if (strncmp(label, "MotionImpulse", 13) == 0) return kRoleImpulse;
	if (strcmp(label, "HighProcess[262]") == 0 || strcmp(label, "MiddleHighProcess[262]") == 0) return kRolePush;
	if (strcmp(label, "bhkBlendCollisionObject[44]") == 0) return kRoleMotionType;
	if (strcmp(label, "bhkWorld[50]") == 0) return kRolePick;
	if (strncmp(label, "bhkWorld[", 9) == 0) return kRoleWorld;
	return kRoleNone;
}
static bool AddSlot(const char* label, uint32_t slotAddr) {
	if (g_n >= kMaxProbes) return false;
	uint32_t orig = *(volatile uint32_t*)slotAddr;
	if (orig < 0x401000 || orig > 0xFDE58B) return false;                      // not a code pointer: leave it alone
	uint32_t idx = (uint32_t)g_n; Probe& pr = g_probes[idx]; memset(&pr, 0, sizeof pr);
	snprintf(pr.label, sizeof pr.label, "%s", label); pr.slotAddr = slotAddr; pr.original = orig; pr.role = RoleFor(label);
	uint8_t* t = EmitThunk(idx, orig); if (!t) return false;
	DWORD old; if (!VirtualProtect((void*)slotAddr, 4, PAGE_EXECUTE_READWRITE, &old)) return false;
	*(volatile uint32_t*)slotAddr = (uint32_t)(uintptr_t)t; VirtualProtect((void*)slotAddr, 4, old, &old);
	g_n++; return true;
}
int ProbesInstall(const char* ini) {
	char sect[4096]; int ok = 0;
	DWORD n = GetPrivateProfileSectionA("SlotProbes", sect, sizeof sect, ini);        // Label=0xVTABLE,slot
	for (const char* e = sect; n && *e; e += strlen(e) + 1) {
		const char* eq = strchr(e, '='); if (!eq) continue;
		char key[64]; snprintf(key, sizeof key, "%.*s", (int)(eq - e), e);
		char* comma; uint32_t vt = strtoul(eq + 1, &comma, 0); if (*comma != ',') continue;
		uint32_t slot = strtoul(comma + 1, nullptr, 0);
		if (AddSlot(key, vt + 4 * slot)) ok++; else Log("slot probe %s NOT hooked", key);
	}
	return ok;
}

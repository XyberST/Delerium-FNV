#include "pick.h"
#include "actor.h"
#include "log.h"
#include <windows.h>
#include <malloc.h>
#include <cstring>

static const uint32_t kPickObject = 0x00C696D0;
static const uint32_t kPickGate = 0x012677A3;   // byte: when non-zero the function skips its per-frame time-budget early-out (which otherwise reports "no hit")
static volatile LONG g_world;
#ifdef _MSC_VER
typedef bool(__thiscall* PickFn)(void*, void*);
#else
typedef bool(__attribute__((thiscall)) * PickFn)(void*, void*);
#endif
void PickSetWorld(uint32_t w) { g_world = (LONG)w; }
uint32_t PickWorld() { return (uint32_t)g_world; }

#ifdef _MSC_VER
static bool Call(PickFn fn, void* world, void* blk, bool& hit) { __try { hit = fn(world, blk); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
#else
static bool Call(PickFn fn, void* world, void* blk, bool& hit) { hit = fn(world, blk); return true; }
#endif

// ---- world discovery without hooks: the slot probes never fired (the game calls PickObject directly, and the live world may be a derived class with its own vtable)
#include <cstdio>
static bool LooksLikeWorld(uint32_t p) {
	if (p < 0x10000 || p > 0x7FFF0000 || (p & 3)) return false;
	uint32_t vt = 0, s = 0;
	if (!Rd(p, &vt, 4) || vt < 0x1000000 || vt > 0x1300000 || (vt & 3)) return false;
	if (vt == 0x10C40B4) return true;
	return Rd(vt + 4 * 50, &s, 4) && s == kPickObject;                                  // a bhkWorld-family vtable: slot 50 is PickObject
}
static bool ReadBlock(uint32_t addr, uint32_t* buf, size_t maxDwords, size_t& got) {   // largest readable prefix, in dwords
	for (size_t n = maxDwords; n >= 8; n /= 2) if (Rd(addr, buf, n * 4)) { got = n; return true; }
	got = 0; return false;
}
static uint32_t g_cacheCell, g_cacheWorld;
uint32_t PickFindWorld(uint32_t actor, char* how, size_t howLen) {
	uint32_t cell = 0; if (!Rd(actor + 0x40, &cell, 4) || cell < 0x10000) { snprintf(how, howLen, "actor has no parent cell"); return 0; }
	if (cell == g_cacheCell && g_cacheWorld && LooksLikeWorld(g_cacheWorld)) { snprintf(how, howLen, "cached, cell %08X", cell); return g_cacheWorld; }
	static uint32_t l0[128], l1[64]; size_t n0 = 0, n1 = 0;
	if (!ReadBlock(cell, l0, 128, n0)) { snprintf(how, howLen, "cell %08X unreadable", cell); return 0; }
	for (size_t i = 0; i < n0; i++) if (l0[i] != cell && LooksLikeWorld(l0[i])) { g_cacheCell = cell; g_cacheWorld = l0[i]; snprintf(how, howLen, "cell %08X +0x%X", cell, (unsigned)(i * 4)); return l0[i]; }
	unsigned reads = 0;
	for (size_t i = 0; i < n0; i++) {
		uint32_t p = l0[i]; if (p < 0x10000 || p > 0x7FFF0000 || (p & 3) || p == cell) continue;
		if (!ReadBlock(p, l1, 64, n1)) continue; reads++;
		for (size_t j = 0; j < n1; j++) if (l1[j] != p && LooksLikeWorld(l1[j])) { g_cacheCell = cell; g_cacheWorld = l1[j]; snprintf(how, howLen, "cell %08X +0x%X -> +0x%X", cell, (unsigned)(i * 4), (unsigned)(j * 4)); return l1[j]; }
	}
	snprintf(how, howLen, "cell %08X, %u pointers followed, no bhkWorld", cell, reads); return 0;
}

bool PickCast(const float from[3], const float to[3], uint32_t filter, bool& hit, float& fraction, float normal[3]) {
	uint32_t world = PickWorld(); if (!world) return false;
	static uint8_t* blk = (uint8_t*)_aligned_malloc(0x100, 16);
	static bool checked, ok;
	if (!checked) { checked = true; uint8_t b[6] = {}; ok = Rd(kPickObject, b, 6) && b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC; if (!ok) Log("pick: prologue of 0xC696D0 is not as expected, ray casts disabled"); }
	if (!ok || !blk) return false;
	memset(blk, 0, 0x100);
	float* f = (float*)blk; f[0] = from[0]; f[1] = from[1]; f[2] = from[2]; f[3] = 0; f[4] = to[0]; f[5] = to[1]; f[6] = to[2]; f[7] = 0;
	*(uint32_t*)(blk + 0x24) = filter; *(float*)(blk + 0x40) = 1.0f; *(int32_t*)(blk + 0x44) = -1; *(int32_t*)(blk + 0x50) = -1;
	uint8_t old = 0, one = 1; bool gate = Rd(kPickGate, &old, 1); if (gate) Wr(kPickGate, &one, 1);
	hit = false; bool called = Call((PickFn)(uintptr_t)kPickObject, (void*)(uintptr_t)world, blk, hit);
	if (gate) Wr(kPickGate, &old, 1);
	if (!called) return false;
	fraction = *(float*)(blk + 0x40); const float* n = (const float*)(blk + 0x30); normal[0] = n[0]; normal[1] = n[1]; normal[2] = n[2];
	return true;
}

// ---------------------------------------------------------------------------------------------------------------------
static const uint32_t kTimeGlobalSeconds = 0x011F63A0;   // TimeGlobal (0x11F6394) + 0x0C: secondsPassed
static const uint32_t kVatsMultFn = 0x009C8CC0, kVatsObj = 0x011F2250;   // the call xNVSE itself uses to scale game time for VATS
#ifdef _MSC_VER
typedef double(__thiscall* VatsFn)(void*);
typedef void(__thiscall* MotionFn)(void*, int, int, int);
static bool CallVats(double& out) { __try { out = ((VatsFn)(uintptr_t)kVatsMultFn)((void*)(uintptr_t)kVatsObj); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
static bool CallMotion(void* o, int t) { __try { ((MotionFn)(uintptr_t)0x00C823B0)(o, t, 0, 0); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
#else
typedef double(__attribute__((thiscall)) * VatsFn)(void*);
typedef void(__attribute__((thiscall)) * MotionFn)(void*, int, int, int);
static bool CallVats(double& out) { out = ((VatsFn)(uintptr_t)kVatsMultFn)((void*)(uintptr_t)kVatsObj); return true; }
static bool CallMotion(void* o, int t) { ((MotionFn)(uintptr_t)0x00C823B0)(o, t, 0, 0); return true; }
#endif
float GameFrameSeconds() {
	float sp = 0; if (!Rd(kTimeGlobalSeconds, &sp, 4) || !(sp > 0.0f && sp < 0.5f)) return 0;
	double vm = 1.0; if (!CallVats(vm) || !(vm > 0.0 && vm <= 4.0)) vm = 1.0;
	return sp * (float)vm;
}
bool GameSetMotionType(uint32_t co, int type) {
	static bool checked, ok;
	if (!checked) { checked = true; uint8_t b[4] = {}; ok = Rd(0x00C823B0, b, 4) && b[0] == 0x51 && b[1] == 0x53 && b[2] == 0x8B && b[3] == 0xD9; if (!ok) Log("game: motion type setter is not as expected, body switching disabled"); }
	return ok && CallMotion((void*)(uintptr_t)co, type);
}

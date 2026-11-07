#include <cmath>
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
static uint32_t g_cacheCell, g_cacheWorld, g_cacheHkw, g_lastParent, g_forced, g_forcedHkw;
// A bhkWorld keeps its hkpWorld at +8 (bhkRefObject). We know the hkpWorld from the actor's own body (collision object +0x10 -> body, +8 -> hkp body, +8 -> hkpWorld),
// so any object whose +8 is that hkpWorld and whose first dword is a code-section vtable is the bhkWorld. Checked in memory while walking the cell, no per-candidate reads.
static bool IsRdata(uint32_t v) { return v >= 0x1000000 && v < 0x1300000 && !(v & 3); }
// Walks one cell: its own pointers, and the pointers inside those. Returns the bhkWorld, or 0.
static uint32_t WalkCell(uint32_t cell, uint32_t hkw, char* how, size_t howLen, unsigned& reads, unsigned& reads2) {
	static uint32_t l0[128], l1[64], l2[4]; size_t n0 = 0, n1 = 0;
	if (!ReadBlock(cell, l0, 128, n0)) return 0;
	auto accept = [&](uint32_t w, const char* by, unsigned a, unsigned b2, uint32_t parent) { g_lastParent = parent; g_cacheCell = cell; g_cacheWorld = w; g_cacheHkw = hkw; snprintf(how, howLen, "%s cell %08X +0x%X +0x%X", by, cell, a, b2); return w; };
	for (size_t i = 0; i < n0; i++) if (l0[i] != cell && LooksLikeWorld(l0[i])) return accept(l0[i], "vtable", (unsigned)(i * 4), 0, cell);
	for (size_t i = 0; i < n0; i++) {
		uint32_t p = l0[i]; if (p < 0x10000 || p > 0x7FFF0000 || (p & 3) || p == cell) continue;
		if (hkw && Rd(p, l2, 12) && IsRdata(l2[0]) && l2[2] == hkw) return accept(p, "hkp", (unsigned)(i * 4), 0, cell);
		if (!ReadBlock(p, l1, 64, n1)) continue; reads++;
		for (size_t j = 0; j < n1; j++) {
			uint32_t q = l1[j]; if (q == p || q < 0x10000 || q > 0x7FFF0000 || (q & 3)) continue;
			if (LooksLikeWorld(q)) return accept(q, "vtable", (unsigned)(i * 4), (unsigned)(j * 4), p);
			if (hkw && Rd(q, l2, 12)) { reads2++; if (IsRdata(l2[0]) && l2[2] == hkw) return accept(q, "hkp", (unsigned)(i * 4), (unsigned)(j * 4), p); }
		}
	}
	return 0;
}
// The actor's own cell does not always lead to the world (it did for some cells, not for others), so the player's cell is searched too.
uint32_t PickFindWorld(uint32_t actor, uint32_t co, char* how, size_t howLen) {
	uint32_t cell = 0; if (!Rd(actor + 0x40, &cell, 4) || cell < 0x10000) { snprintf(how, howLen, "actor has no parent cell"); return 0; }
	uint32_t body = 0, hkb = 0, hkw = 0;
	if (co) { Rd(co + 0x10, &body, 4); if (body) Rd(body + 8, &hkb, 4); if (hkb) Rd(hkb + 8, &hkw, 4); }
	if (g_forced && (!hkw || hkw == g_forcedHkw)) { snprintf(how, howLen, "world proven by a real hit"); return g_forced; }
	if (hkw && hkw == g_cacheHkw && g_cacheWorld) { uint32_t b = 0; if (Rd(g_cacheWorld + 8, &b, 4) && b == hkw) { snprintf(how, howLen, "cached by hkpWorld %08X", hkw); return g_cacheWorld; } }   // one hkpWorld serves many cells
	if (!hkw && cell == g_cacheCell && g_cacheWorld && LooksLikeWorld(g_cacheWorld)) { snprintf(how, howLen, "cached, cell %08X", cell); return g_cacheWorld; }
	{ uint32_t gw = 0, b = 0; if (Rd(0x011CA0D8, &gw, 4) && LooksLikeWorld(gw) && (!hkw || (Rd(gw + 8, &b, 4) && b == hkw))) { g_cacheWorld = gw; g_cacheHkw = hkw; snprintf(how, howLen, "the game's world pointer 0x11CA0D8"); return gw; } }   // FalloutNV.exe: NiPointer<bhkWorld> set up at 0x541B80
	uint32_t player = 0, pcell = 0; Rd(0x011DEA3C, &player, 4); if (player > 0x10000) Rd(player + 0x40, &pcell, 4);
	unsigned r1 = 0, r2 = 0; uint32_t w = WalkCell(cell, hkw, how, howLen, r1, r2);
	if (!w && pcell > 0x10000 && pcell != cell) { w = WalkCell(pcell, hkw, how, howLen, r1, r2); if (w) { size_t n = strlen(how); snprintf(how + n, howLen - n, " (player's cell)"); } }
	if (!w) snprintf(how, howLen, "cells %08X/%08X, %u+%u pointers followed; co %08X body %08X hkpBody %08X hkpWorld %08X", cell, pcell, r1, r2, co, body, hkb, hkw);
	return w;
}

static float g_pickScale = 1.0f / 7.0f;   // FO3/FNV Havok: 7 game units per Havok unit
void PickSetScale(float s) { g_pickScale = s; }
float PickScale() { return g_pickScale; }
bool PickCast(const float from[3], const float to[3], uint32_t filter, bool& hit, float& fraction, float normal[3]) {
	uint32_t world = PickWorld(); if (!world) return false;
	static uint8_t* blk = (uint8_t*)_aligned_malloc(0x100, 16);
	static bool checked, ok;
	if (!checked) { checked = true; uint8_t b[6] = {}; ok = Rd(kPickObject, b, 6) && b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC; if (!ok) Log("pick: prologue of 0xC696D0 is not as expected, ray casts disabled"); }
	if (!ok || !blk) return false;
	memset(blk, 0, 0x100);
	float* f = (float*)blk; const float k = g_pickScale; f[0] = from[0] * k; f[1] = from[1] * k; f[2] = from[2] * k; f[3] = 0; f[4] = to[0] * k; f[5] = to[1] * k; f[6] = to[2] * k; f[7] = 0;
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
#ifdef _MSC_VER
typedef void(__thiscall* VelFn)(void*, const float*);
static bool CallVel(uint32_t fn, uint32_t body, const float* v) { __try { ((VelFn)(uintptr_t)fn)((void*)(uintptr_t)body, v); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
#else
typedef void(__attribute__((thiscall)) * VelFn)(void*, const float*);
static bool CallVel(uint32_t fn, uint32_t body, const float* v) { ((VelFn)(uintptr_t)fn)((void*)(uintptr_t)body, v); return true; }
#endif
// bhkRigidBody + 8 = hkpRigidBody, whose hkpMaxSizeMotion lives at +0xE0 (FalloutNV.exe 1.4.0.525). The motion's virtual slots 16 / 17 (0xC864B0 / 0xC864D0) store a vector at
// motion+0xD0 / +0xE0: linear and angular velocity, in Havok units per second (game units / 7). (Slots 12 / 13 are setPosition / setRotation, so never pass those a velocity.)
static bool IsMotionVtable(uint32_t vt) {            // hkpMotion and every subclass sharing its velocity setters (FalloutNV.exe): Motion, KeyframedRigid, MaxSize, Sphere, Box, ThinBox, Character
	return vt == 0x10C59F4 || vt == 0x10C6BEC || vt == 0x10C6C64 || vt == 0x10C6D54 || vt == 0x10C6DC4 || vt == 0x10C6E34 || vt == 0x10C6EA4;
}
static bool MotionOf(uint32_t body, uint32_t& motion, uint32_t& fLin, uint32_t& fAng) {
	uint32_t hkp = 0, vt = 0;
	if (body < 0x10000 || !Rd(body + 8, &hkp, 4) || hkp < 0x10000 || !Rd(hkp + 0xE0, &vt, 4) || !IsMotionVtable(vt)) return false;
	if (!Rd(vt + 4 * 16, &fLin, 4) || !Rd(vt + 4 * 17, &fAng, 4) || fLin != 0x00C864B0 || fAng != 0x00C864D0) return false;
	motion = hkp + 0xE0; return true;
}
void GameBodyDescribe(uint32_t body, char* out, size_t n) {         // for the log when a body is refused
	uint32_t hkp = 0, vt = 0, a = 0, b = 0; Rd(body + 8, &hkp, 4); if (hkp) Rd(hkp + 0xE0, &vt, 4); if (vt) { Rd(vt + 64, &a, 4); Rd(vt + 68, &b, 4); }
	snprintf(out, n, "body %08X vt %08X hkp %08X motion vt %08X slot16 %08X slot17 %08X", body, [&] { uint32_t v = 0; Rd(body, &v, 4); return v; }(), hkp, vt, a, b);
}
bool GameBodyOk(uint32_t body) { uint32_t m, a, b; return MotionOf(body, m, a, b); }
bool GameSetBodyVelocity(uint32_t body, const float lin[3], const float ang[3]) {
	uint32_t m, a, b; if (!MotionOf(body, m, a, b)) return false;
	alignas(16) float l[4] = { lin[0], lin[1], lin[2], 0 }, w[4] = { ang[0], ang[1], ang[2], 0 };
	for (int i = 0; i < 3; i++) if (!(std::fabs(l[i]) < 1000.0f) || !(std::fabs(w[i]) < 100.0f)) return false;    // never hand the engine a huge or NaN value
	return CallVel(a, m, l) && CallVel(b, m, w);
}
bool GameSetMotionType(uint32_t co, int type) {
	static bool checked, ok;
	if (!checked) { checked = true; uint8_t b[4] = {}; ok = Rd(0x00C823B0, b, 4) && b[0] == 0x51 && b[1] == 0x53 && b[2] == 0x8B && b[3] == 0xD9; if (!ok) Log("game: motion type setter is not as expected, body switching disabled"); }
	return ok && CallMotion((void*)(uintptr_t)co, type);
}

int PickCandidates(uint32_t* out, int max) {
	int n = 0;
	auto add = [&](uint32_t c) { if (c < 0x10000 || c > 0x7FFF0000 || (c & 3) || n >= max) return; uint32_t vt = 0; if (!Rd(c, &vt, 4) || !IsRdata(vt)) return; for (int i = 0; i < n; i++) if (out[i] == c) return; out[n++] = c; };
	add(g_cacheWorld); add(g_lastParent);
	static uint32_t blk[64]; size_t got = 0; const uint32_t bases[2] = { g_cacheWorld, g_lastParent };
	for (uint32_t b : bases) if (b && ReadBlock(b, blk, 64, got)) for (size_t i = 0; i < got; i++) add(blk[i]);
	return n;
}
void PickDescribe(uint32_t o, char* out, size_t n) {
	uint32_t vt = 0, s = 0, f8 = 0; Rd(o, &vt, 4); if (IsRdata(vt)) Rd(vt + 4 * 50, &s, 4); Rd(o + 8, &f8, 4);
	snprintf(out, n, "%08X: vt %08X slot50 %08X +8 %08X", o, vt, s, f8);
}
void PickForceWorld(uint32_t w) { g_forced = w; g_forcedHkw = g_cacheHkw; g_world = (LONG)w; }

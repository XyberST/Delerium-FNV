#include "pick.h"
#include "actor.h"
#include "log.h"
#include <windows.h>
#include <malloc.h>
#include <cstring>

static const uint32_t kPickObject = 0x00C696D0;
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

bool PickCast(const float from[3], const float to[3], uint32_t filter, bool& hit, float& fraction, float normal[3]) {
	uint32_t world = PickWorld(); if (!world) return false;
	static uint8_t* blk = (uint8_t*)_aligned_malloc(0x100, 16);
	static bool checked, ok;
	if (!checked) { checked = true; uint8_t b[6] = {}; ok = Rd(kPickObject, b, 6) && b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC; if (!ok) Log("pick: prologue of 0xC696D0 is not as expected, ray casts disabled"); }
	if (!ok || !blk) return false;
	memset(blk, 0, 0x100);
	float* f = (float*)blk; f[0] = from[0]; f[1] = from[1]; f[2] = from[2]; f[3] = 0; f[4] = to[0]; f[5] = to[1]; f[6] = to[2]; f[7] = 0;
	*(uint32_t*)(blk + 0x24) = filter; *(float*)(blk + 0x40) = 1.0f; *(int32_t*)(blk + 0x44) = -1; *(int32_t*)(blk + 0x50) = -1;
	hit = false; if (!Call((PickFn)(uintptr_t)kPickObject, (void*)(uintptr_t)world, blk, hit)) return false;
	fraction = *(float*)(blk + 0x40); const float* n = (const float*)(blk + 0x30); normal[0] = n[0]; normal[1] = n[1]; normal[2] = n[2];
	return true;
}

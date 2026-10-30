#include "engine.h"
#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

bool EngineLoad(EngineApi& api, const std::string& dir) {
	std::string path = dir + "\\delirium_physics.dll";
	HMODULE h = LoadLibraryA(path.c_str());
	if (!h) { Log("engine: LoadLibrary(%s) failed, error %lu (is it the 32-bit build?)", path.c_str(), GetLastError()); return false; }
	api.module = h; int missing = 0;
#define NM_FN(ret, name, args) if (!(*(FARPROC*)&api.name = GetProcAddress(h, #name))) { Log("engine: missing export %s", #name); missing++; }
#include "engine_api.inc"
#undef NM_FN
	Log("engine: loaded %s, version %d, %d exports missing", path.c_str(), api.nm_version ? api.nm_version() : -1, missing);
	return missing == 0;
}

static std::string ReadAll(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static std::string Err(EngineApi& a) { char b[512] = {}; a.nm_last_error(b, sizeof b); return b; }

bool EngineSmokeTest(EngineApi& api, const std::string& dir) {
	DWORD t0 = GetTickCount();
	std::string chr = dir + "\\skyrim_character.json";
	if (api.nm_init_character_file(chr.c_str()) < 0) { Log("smoke: init character failed: %s", Err(api).c_str()); return false; }
	std::string json = ReadAll(chr);
	NmBuild* base = api.nm_build_from_character(json.c_str());
	if (!base) { Log("smoke: build failed: %s", Err(api).c_str()); return false; }
	// joints: 21 lines "x y z" in metres, pelvis-relative, from the NVCS skeleton
	std::vector<float> joints; { std::ifstream f(dir + "\\fnv_joints.txt"); float v; while (f >> v) joints.push_back(v); }
	if (joints.size() != 63) { Log("smoke: fnv_joints.txt has %u floats, want 63", (unsigned)joints.size()); return false; }
	std::vector<float> rots(21 * 4);
	for (int i = 0; i < 21; i++) { float p[3], q[4]; api.nm_build_part_frame(base, i, p, q); memcpy(&rots[i * 4], q, sizeof q); }
	NmBuild* fit = api.nm_build_remap(base, joints.data(), rots.data(), 0x1FFFFFu);
	if (!fit) { Log("smoke: remap failed: %s", Err(api).c_str()); return false; }
	NmWorld* w = api.nm_world_create(); api.nm_world_add_floor(w, 0, 0, 0, 100.0f);
	const float zero[3] = { 0, 0, 0 };
	NmAgent* a = api.nm_agent_create(fit, zero, 0.0f, 0.3f);
	if (!a) { Log("smoke: agent create failed: %s", Err(api).c_str()); return false; }
	api.nm_agent_task_balance(a, w, nullptr, nullptr);
	for (int i = 0; i < 180; i++) api.nm_agent_step(a, w, 1.0f / 60.0f);
	float com[3], vel[3]; api.nm_agent_get_com(a, com, vel);
	Log("smoke: after 3 s balance com=(%.2f %.2f %.2f) fallen=%d (expect z ~1.0, fallen 0)", com[0], com[1], com[2], api.nm_agent_has_fallen(a));
	const float at[3] = { 0, -0.1f, 1.3f }, dir3[3] = { 0, 1, 0.1f };
	api.nm_agent_task_shot(a, w, nullptr, 9, at, dir3, 1.0f);
	for (int i = 0; i < 240; i++) api.nm_agent_step(a, w, 1.0f / 60.0f);
	api.nm_agent_get_com(a, com, vel);
	Log("smoke: after shot com=(%.2f %.2f %.2f) fallen=%d (expect z ~0.1, fallen 1); took %lu ms", com[0], com[1], com[2], api.nm_agent_has_fallen(a), (unsigned long)(GetTickCount() - t0));
	api.nm_agent_destroy(a); api.nm_world_destroy(w); api.nm_build_destroy(fit); api.nm_build_destroy(base);
	return true;
}

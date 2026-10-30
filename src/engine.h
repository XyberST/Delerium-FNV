// Dynamic loader for the 32-bit delirium_physics.dll (the NM/Delirium engine C API) plus a no-game smoke test.
#pragma once
#define DELIRIUM_PHYSICS_STATIC
#include "delirium_physics.h"
#include <string>

struct EngineApi {
	void* module = nullptr;
#define NM_FN(ret, name, args) ret(__cdecl* name) args = nullptr;
#include "engine_api.inc"
#undef NM_FN
};
// Loads <dir>\delirium_physics.dll and resolves every nm_* function. Logs any that are missing.
bool EngineLoad(EngineApi& api, const std::string& dir);
// Builds the FNV body from <dir>\fnv_joints.txt + the character file, stands it on a floor for 3 s, shoots it, logs the result.
bool EngineSmokeTest(EngineApi& api, const std::string& dir);

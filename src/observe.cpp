#include "observe.h"
#include "actor.h"
#include "ingame.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <string>

static const float kUnit = 0.0142875f;                        // metres per game unit
static EngineApi* g_api; static NmBuild* g_fit; static bool g_enabled;
static volatile LONG g_lastDmgActor, g_lastDmgTick, g_pending, g_pendingTick;
static int g_sessions; static DWORD g_lastSession;

// Called from a probe thunk; must stay tiny and lock-free.
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack) {
	if (!g_enabled) return;
	DWORD now = GetTickCount();
	if (role == kRoleDamage) { g_lastDmgActor = (LONG)thisPtr; g_lastDmgTick = (LONG)now; }
	else if (role == kRoleMotionType && stack[1] == 1 /* motion type: dynamic */ && now - (DWORD)g_lastDmgTick <= 100 && !g_pending) {
		g_pending = g_lastDmgActor; g_pendingTick = (LONG)now;
	}
}
void ObserveInit(EngineApi* api, void* fit, const char*) { g_api = api; g_fit = (NmBuild*)fit; g_enabled = true; }

// ---- one observation session (game thread) ----
struct Session {
	bool active = false; uint32_t actor = 0, root = 0; unsigned frame = 0;
	NmWorld* world = nullptr; NmAgent* agent = nullptr; WorldXform rootW{};
	uint32_t pelvis = 0, head = 0, handL = 0, handR = 0, footL = 0, footR = 0;
};
static Session S;
static const char* kWatch[] = { "Bip01 Pelvis", "Bip01 Head", "Bip01 L Hand", "Bip01 R Hand", "Bip01 L Foot", "Bip01 R Foot" };

// world point -> metres in the actor's root frame (x right, y forward, z up, origin at the root)
static bool Local(uint32_t node, float out[3]) {
	WorldXform w; if (!NodeWorld(node, w)) return false;
	float d[3] = { w.T[0] - S.rootW.T[0], w.T[1] - S.rootW.T[1], w.T[2] - S.rootW.T[2] };
	const float* R = S.rootW.R; float inv = S.rootW.S != 0 ? 1.0f / S.rootW.S : 1.0f;
	for (int i = 0; i < 3; i++) out[i] = (R[0 * 3 + i] * d[0] + R[1 * 3 + i] * d[1] + R[2 * 3 + i] * d[2]) * inv * kUnit;   // R^T * d
	return true;
}
static void End() {
	if (S.agent) g_api->nm_agent_destroy(S.agent);
	if (S.world) g_api->nm_world_destroy(S.world);
	S = Session();
}
static void Begin(uint32_t actor) {
	DWORD now = GetTickCount();
	if (g_sessions >= 6 || now - g_lastSession < 4000) return;
	g_lastSession = now; g_sessions++;
	Log("observe: death burst, actor %08X (session %d)", actor, g_sessions);
	S = Session(); S.actor = actor; S.root = ActorRoot(actor);
	if (!S.root) { Log("observe: could not read the actor's 3D root (offset 0x64 -> 0x14)"); return; }
	if (!NodeWorld(S.root, S.rootW)) { Log("observe: root %08X unreadable", S.root); return; }
	std::string rn; NodeName(S.root, rn);
	Log("observe: root %08X '%s' world T=(%.1f %.1f %.1f) scale %.3f", S.root, rn.c_str(), S.rootW.T[0], S.rootW.T[1], S.rootW.T[2], S.rootW.S);
	auto bones = CollectBones(S.root);
	Log("observe: %u named nodes found", (unsigned)bones.size());
	uint32_t* slots[6] = { &S.pelvis, &S.head, &S.handL, &S.handR, &S.footL, &S.footR }; int found = 0;
	for (int i = 0; i < 6; i++) { auto it = bones.find(kWatch[i]); if (it != bones.end()) { *slots[i] = it->second; found++; } else Log("observe: bone '%s' NOT FOUND", kWatch[i]); }
	Log("observe: %d/6 watched bones found", found);
	if (found < 6) return;
	float p[3]; if (Local(S.pelvis, p)) Log("observe: pelvis in root frame (m) = (%.3f %.3f %.3f)   [NVCS bind pose: 0.000 0.000 0.968]", p[0], p[1], p[2]);
	if (g_api && g_fit) {                                                  // Delirium body from the NVCS fit, standing at the root, floor at root height
		S.world = g_api->nm_world_create(); g_api->nm_world_add_floor(S.world, 0, 0, 0, 100.0f);
		const float zero[3] = { 0, 0, 0 };
		S.agent = g_api->nm_agent_create(g_fit, zero, 0.0f, 0.3f);
		if (S.agent) { const float at[3] = { 0, -0.1f, 1.3f }, dir[3] = { 0, 1, 0.1f }; g_api->nm_agent_task_shot(S.agent, S.world, nullptr, 9, at, dir, 1.0f); }
		else Log("observe: agent create failed");
	} else Log("observe: no engine body, logging the game skeleton only");
	S.active = true; S.frame = 0;
	IngameNotify("DeliriumFNV: observed a death, see log");
}

void ObserveTick() {
	if (!g_enabled) return;
	if (S.active) {
		S.frame++;
		if (S.agent) g_api->nm_agent_step(S.agent, S.world, 1.0f / 60.0f);
		if (S.frame % 15 == 0 || S.frame == 1) {
			float gp[3] = {}, gh[3] = {}, gl[3] = {}, gr[3] = {};
			bool ok = Local(S.pelvis, gp) && Local(S.head, gh) && Local(S.footL, gl) && Local(S.footR, gr);
			float tr[21 * 7] = {}; bool have = S.agent && g_api->nm_agent_get_part_transforms(S.agent, tr, 21) >= 0;
			char line[400]; int n = snprintf(line, sizeof line, "observe f%03u game pelvis z=%.2f head z=%.2f feet z=%.2f/%.2f", S.frame, gp[2], gh[2], gl[2], gr[2]);
			if (have) snprintf(line + n, sizeof line - n, " | delirium pelvis z=%.2f head z=%.2f feet z=%.2f/%.2f", tr[0 * 7 + 2], tr[12 * 7 + 2], tr[3 * 7 + 2], tr[6 * 7 + 2]);
			Log("%s%s", line, ok ? "" : "   (a bone read failed)");
		}
		if (S.frame >= 240) { Log("observe: session over"); End(); }
		return;
	}
	if (g_pending) { uint32_t a = (uint32_t)g_pending; g_pending = 0; Begin(a); }
}

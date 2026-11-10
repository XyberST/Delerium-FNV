#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include "observe.h"
#include "actor.h"
#include "rig.h"
#include "mathx.h"
#include "ingame.h"
#include "pick.h"
#include "ground.h"
#include "log.h"
#include "script.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

static const float kUnit = 0.0142875f;                        // metres per game unit
static EngineApi* g_api; static NmBuild* g_fit; static Rig g_rig;
static bool g_enabled, g_drive = true, g_requireDead = true;
static int g_maxActors = 4; static float g_simSeconds = 8, g_holdSeconds = 300;
static bool g_balance = true, g_dismemberedOk = true; static float g_balanceHealth = 120.0f, g_balanceSeconds = 2.0f, g_balanceMaxSpeed = 6.0f, g_balanceFall = 1.0f, g_balanceVelScale = 0.5f; static int g_balanceSteps = 8; static bool g_balancePush = true;
static bool g_calibDone = false;
static ExtraCfg g_x; static std::vector<std::string> g_burnKw, g_shockKw; static void (*g_reloadFn)() = nullptr; static int g_reloadVk = 0; static float g_fps = 0;
static double NowMs() { static double inv = 0; LARGE_INTEGER c; if (!inv) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); inv = 1000.0 / (double)f.QuadPart; } QueryPerformanceCounter(&c); return (double)c.QuadPart * inv; }
static std::vector<std::string> Split(const char* csv) { std::vector<std::string> r; std::string cur; for (const char* p = csv; ; p++) { if (*p == ',' || !*p) { if (!cur.empty()) r.push_back(cur); cur.clear(); if (!*p) break; } else if (*p != ' ') cur += (char)tolower((unsigned char)*p); } return r; }
void ObserveSetExtra(const ExtraCfg& c) { g_x = c; g_burnKw = Split(c.burnKeywords); g_shockKw = Split(c.shockKeywords); }
void ObserveSetReloadHook(void (*fn)(), int vk) { g_reloadFn = fn; g_reloadVk = vk; }
static bool g_hitLoc = true; static float g_hitMin = 1.0f, g_mulHead = 0.0f, g_mulTorso = 1.0f, g_mulPelvis = 0.8f, g_mulLeg = 0.4f, g_mulArm = 1.3f;
static bool g_driveBodies = true; static float g_driveGain = 1.0f, g_driveMax = 8.0f, g_driveAngGain = 0.4f;
static bool g_worldCollision, g_calibrate, g_groundAuto, g_groundReady = true, g_release = true; static unsigned g_calibTries; static float g_restSpeed = 1.5f; static unsigned g_restFrames = 30; static uint32_t g_groundFilter = 0x011F0025; static float g_groundCell = 0.35f; static int g_groundHalf = 10;
static float g_velScale = 1.0f, g_shotHealth = 1.0f; static bool g_keyframe = true, g_skipDismembered = true;
static int g_started;                                         // sessions begun this run (verbose logging for the first few only)

void ObserveSetDrive(bool on) { g_drive = on; }
void ObserveSetRequireDead(bool on) { g_requireDead = on; }
void ObserveSetDriveBodies(bool on, float gain, float maxSpeed, float angGain) { g_driveBodies = on; g_driveGain = gain; g_driveMax = maxSpeed; g_driveAngGain = angGain; }
void ObserveSetBalance(bool on, float health, float seconds, float maxSpeed, bool dismemberedOk) { g_balance = on; g_balanceHealth = health; g_balanceSeconds = seconds; g_balanceMaxSpeed = maxSpeed; g_dismemberedOk = dismemberedOk; }
void ObserveSetBalanceMore(float fallSeconds, int steps, float velocityScale, bool push) { g_balanceFall = fallSeconds; g_balanceSteps = steps; g_balanceVelScale = velocityScale; g_balancePush = push; }
void ObserveSetHitLocation(bool on, float minKick, float head, float torso, float pelvis, float leg, float arm) { g_hitLoc = on; g_hitMin = minKick; g_mulHead = head; g_mulTorso = torso; g_mulPelvis = pelvis; g_mulLeg = leg; g_mulArm = arm; }
void ObserveSetRelease(bool on, float restSpeed, unsigned restFrames) { g_release = on; g_restSpeed = restSpeed; g_restFrames = restFrames; }
void ObserveSetBodies(bool keyframeGameBodies, bool skipDismembered) { g_keyframe = keyframeGameBodies; g_skipDismembered = skipDismembered; }
void ObserveSetTuning(float velocityScale, float shotHealth) { g_velScale = std::max(0.0f, std::min(velocityScale, 4.0f)); g_shotHealth = std::max(0.1f, shotHealth); }
// an auto-chosen ground layer survives a live reload
void ObserveSetWorld(bool collision, bool calibrate, uint32_t filter, float cell, int half) { g_worldCollision = collision; g_calibrate = calibrate; g_groundAuto = filter == 0; if (filter) { g_groundReady = true; g_groundFilter = filter; } else if (!g_calibDone) { g_groundReady = false; g_groundFilter = 0x011F0025; } g_groundCell = std::max(0.15f, cell); g_groundHalf = std::max(2, std::min(half, 16)); }
void ObserveSetLimits(int maxActors, float simSeconds, float holdSeconds) { g_maxActors = std::max(1, std::min(maxActors, 8)); g_simSeconds = simSeconds; g_holdSeconds = holdSeconds; }
void ObserveInit(EngineApi* api, void* fit, const char* dir) {
	g_api = api; g_fit = (NmBuild*)fit; g_enabled = true;
	std::string rp = std::string(dir) + "\\fnv_character_rig.txt";
	if (RigLoad(g_rig, rp)) Log("observe: rig loaded (21 parts)"); else Log("observe: rig %s NOT loaded; nothing will be driven", rp.c_str());
}

// ---- death detection (runs inside the probe thunks: tiny and lock-free) ----
static volatile LONG g_dmgActor[64], g_dmgTick[64], g_dmgN, g_dmgAtk[64];
static std::vector<NmAgent*> g_pool; static float g_lodScale = 1.0f; static double g_frameMs = 0;
// Where the attacker of the latest hit on this actor stands (third argument of the damage slot), checked to be a real actor before it is believed.
static bool AttackerPos(uint32_t victim, float out[3]) {
	const DWORD now = GetTickCount(); LONG top = g_dmgN;
	for (LONG i = top - 1; i >= 0 && i >= top - 64; i--) if ((uint32_t)g_dmgActor[i & 63] == victim && now - (DWORD)g_dmgTick[i & 63] <= 600) {
		const uint32_t a = (uint32_t)g_dmgAtk[i & 63]; uint32_t vt = 0;
		if (!a || a == victim || !Rd(a, &vt, 4) || (vt != 0x1086A6C && vt != 0x108AA3C && vt != 0x10870AC)) return false;
		return Rd(a + 0x30, out, 12);
	}
	return false;
}
static float DistToPlayer(uint32_t actor) { uint32_t pl = 0; float ap[3], pp[3]; if (Rd(0x011DEA3C, &pl, 4) && pl && Rd(actor + 0x30, ap, 12) && Rd(pl + 0x30, pp, 12)) { const float dx = ap[0] - pp[0], dy = ap[1] - pp[1], dz = ap[2] - pp[2]; return std::sqrt(dx * dx + dy * dy + dz * dz); } return -1.0f; }
static int DamageTicks(uint32_t actor, DWORD windowMs) { DWORD now = GetTickCount(); int n = 0; LONG top = g_dmgN; for (LONG i = top > 64 ? top - 64 : 0; i < top; i++) if ((uint32_t)g_dmgActor[i & 63] == actor && now - (DWORD)g_dmgTick[i & 63] <= windowMs) n++; return n; }
static volatile LONG g_worldCalls, g_oddVt, g_ours;          // g_ours: set while we make our own motion-type calls
static volatile LONG g_wake[32], g_ww;                      // collision objects the game switched back to a simulated motion type (a grab, an impulse)
static volatile LONG g_lastDmgActor, g_lastDmgTick;
static volatile LONG g_mtAll, g_mtDyn, g_plDmg, g_mtRingN; static volatile LONG g_mtPtr[128], g_mtTick[128], g_mtVal[128];   // every motion-type call (object, time, new type) for the player diagnostics
static volatile LONG g_q[16], g_qw, g_qr; static volatile LONG g_lastQ, g_lastQTick;
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack) {
	if (!g_enabled) return;
	DWORD now = GetTickCount();
	if (role == kRoleWorld || role == kRolePick) { uint32_t vt = 0; InterlockedIncrement(&g_worldCalls); if (Rd(thisPtr, &vt, 4) && vt == 0x10C40B4) PickSetWorld(thisPtr); else g_oddVt = (LONG)vt; return; }   // any call on a bhkWorld tells us the physics world
	if (role == kRoleMotionType && g_ours) return;
	if (role == kRoleMotionType) { InterlockedIncrement(&g_mtAll); if (stack[1] == 1) InterlockedIncrement(&g_mtDyn); LONG ri = (InterlockedIncrement(&g_mtRingN) - 1) & 127; g_mtPtr[ri] = (LONG)thisPtr; g_mtTick[ri] = (LONG)now; g_mtVal[ri] = (LONG)stack[1]; }
	if (role == kRoleDamage) { uint32_t pl0 = 0; if (Rd(0x011DEA3C, &pl0, 4) && pl0 == thisPtr) InterlockedIncrement(&g_plDmg); }
	if (role == kRoleDamage && !g_x.playerDeath) { uint32_t pl = 0; if (Rd(0x011DEA3C, &pl, 4) && pl == thisPtr) return; }
	if (role == kRoleDamage) { g_lastDmgActor = (LONG)thisPtr; g_lastDmgTick = (LONG)now; LONG di = InterlockedIncrement(&g_dmgN) - 1; g_dmgActor[di & 63] = (LONG)thisPtr; g_dmgTick[di & 63] = (LONG)now; g_dmgAtk[di & 63] = (LONG)stack[3]; }
	else if (role == kRoleMotionType && (stack[1] == 1 || stack[1] == 2 || stack[1] == 3 || stack[1] == 6) && now - (DWORD)g_lastDmgTick > 100) { LONG w = InterlockedIncrement(&g_ww) - 1; g_wake[w & 31] = (LONG)thisPtr; }
	else if (role == kRoleMotionType && stack[1] == 1 /* dynamic */ && now - (DWORD)g_lastDmgTick <= 100) {
		LONG a = g_lastDmgActor;
		if (!a || (a == g_lastQ && now - (DWORD)g_lastQTick < 1000)) return;                 // the burst is 36 calls; queue the actor once
		g_lastQ = a; g_lastQTick = (LONG)now;
		LONG w = InterlockedIncrement(&g_qw) - 1; g_q[w & 15] = a;
	}
}

// ---- sessions ----
struct Session {
	double stageMs[3] = { 0, 0, 0 };                              // start-up timings
	bool unconscious = false; int woundPart = -1; int lodFrame = 0; float lodRate = 0;   // life state 3; the rig part that was hit (-1 unknown); distance-based step rate
	bool knock = false; int knockState = 0;                      // a living actor knocked down (game knock state 1 explode, 3 knocked out), not a corpse
	int phase = 0;                                              // 1 pose sampled, 2 simulating, 3 holding the final pose
	uint32_t actor = 0, root = 0, vt0 = 0, refId = 0; WorldXform rootW{}; bool verbose = false; unsigned id = 0;
	uint32_t bone[21]{}, boneA[21]{}, boneB[21]{};
	std::vector<uint32_t> order; std::map<uint32_t, int> rotPart, posPart;
	struct NodeId { uint32_t node, parent, name; }; std::vector<NodeId> ids; unsigned warm = 0;
	GroundGrid* ground = nullptr; bool groundFailed = false; unsigned groundHits = 0;
	V3 p0[21]{}; Q q0[21]{}; LARGE_INTEGER t0{}, last{};
	NmWorld* world = nullptr; NmAgent* agent = nullptr;
	unsigned frame = 0, calm = 0, hold = 0, redrives = 0, nWritten = 0, nPropagated = 0, lifeCheck = 0;
	float tr[21 * 7]{}, prev[21 * 3]{}, driveErr = 0;
	std::map<uint32_t, V3> wrote;                               // game-world bone translation we last wrote
	std::vector<uint32_t> cos; LONG wakeSeen = 0;
	float balanceT = 0, balanceFor = 0; bool relaxed = false;
	float simTime = 0, simAcc = 0, calmT = 0, rt = 0, floorDrop = 0; int kind = 0;             // simulated seconds; step accumulator; calm time; reaction clock; metres to the floor if airborne; 0 normal 1 burn 2 shock 3 thrown 4 fall
	bool nodeBurn = false, nodeShock = false; int dotCount = 0; float lastFastest = 0;
	std::vector<ScriptMsg> smsgs;                                // messages from reaction scripts this session uses (timeline value = index)
	struct TA { float t; int a; float v; }; std::vector<TA> tl; size_t tlNext = 0;               // scripted reactions: (time, action, value)
	double costSum = 0, costMax = 0, beginMs = 0, startMs = 0; unsigned costN = 0;
	unsigned hiddenMask = 0; bool headGone = false, legGone = false, balancing = false;   // rig parts whose bones are scaled away (cut off); head among them; trying to stay on its feet
	int mode = 0;                                               // 1 = the game's own bodies are driven by velocity; 0 = bones are written (old way)
	struct DBody { uint32_t node, body; }; std::vector<DBody> dbodies; float firstErr = -1;                // the game's collision objects we keyframed; wake calls already looked at
};
static std::vector<Session*> g_active; static std::map<uint32_t, DWORD> g_recent, g_recentKnock;

static bool BoneRoot(const Session& s, uint32_t node, V3& pos, Q& rq) {      // metres and rotation in the actor's root frame
	WorldXform w; if (!NodeWorld(node, w)) return false;
	V3 d{ w.T[0] - s.rootW.T[0], w.T[1] - s.rootW.T[1], w.T[2] - s.rootW.T[2] };
	pos = mulMTV(s.rootW.R, d) * ((s.rootW.S != 0 ? 1.0f / s.rootW.S : 1.0f) * kUnit);
	float RR[9]; mulMTM(s.rootW.R, w.R, RR); normaliseCols(RR); rq = fromMat(RR); return true;
}
static bool CapturePose(const Session& s, V3* pos, Q* rq) {   // partRot = boneRot * conj(offset); partPos = joint - partRot * anchor
	for (int i = 0; i < 21; i++) {
		V3 pr, pa, pb; Q br, qa, qb;
		if (!BoneRoot(s, s.bone[i], pr, br) || !BoneRoot(s, s.boneA[i], pa, qa) || !BoneRoot(s, s.boneB[i], pb, qb)) return false;
		rq[i] = norm(mul(br, conj(g_rig.part[i].offset)));
		pos[i] = (pa + pb) * 0.5f - rot(rq[i], g_rig.part[i].anchor);
	}
	return true;
}
static void Free(Session* s) {
	delete s->ground; s->ground = nullptr;
	if (s->agent) g_api->nm_agent_destroy(s->agent);
	if (s->world) g_api->nm_world_destroy(s->world);
	s->agent = nullptr; s->world = nullptr;
}
static int Depth(uint32_t n) { int d = 0; while (n && d < 64) { n = NodeParent(n); d++; } return d; }
static float Deviation(const Session& s, uint32_t* worst = nullptr, V3* worstD = nullptr) {   // how far the game has moved the bones we wrote
	float dev = 0;
	for (auto& kv : s.wrote) { WorldXform w; if (!NodeWorld(kv.first, w)) continue; V3 d{ w.T[0] - kv.second.x, w.T[1] - kv.second.y, w.T[2] - kv.second.z }; float l = len(d); if (l > dev) { dev = l; if (worst) *worst = kv.first; if (worstD) *worstD = d; } }
	return dev;
}

static unsigned g_worldFails;
static void EnsureWorld(const Session& s) {   // the physics world of this corpse's own cell, found from the actor (recomputed per kill: cells change)
	uint32_t co0 = 0;
	{ uint32_t co = 0, vt = 0; if (s.bone[0] && Rd(s.bone[0] + 0x1C, &co, 4) && co && Rd(co, &vt, 4) && vt == 0x10C53DC) co0 = co; }   // the pelvis body is enough to name the Havok world
	char how[200] = {}; uint32_t w = PickFindWorld(s.actor, co0, how, sizeof how);
	if (w) { if (w != PickWorld() || s.id <= 3) Log("ray: bhkWorld %08X (%s)", w, how); PickSetWorld(w); }
	else if (g_worldFails++ < 4) Log("ray: physics world not found (%s); world-slot hook calls %ld, last foreign vtable %08X", how, (long)g_worldCalls, (unsigned)g_oddVt);
}

// One-off calibration: which ray layers see the floor, which also see the actor itself, and are the ray end points game units or Havok metres?
struct LayerRes { bool ok = false, floorHit = false, bodyHit = false; float z = 0, nz = 0; };
static int RunLayers(const Session& s, const char* tag, LayerRes* res) {
	WorldXform pel; if (!NodeWorld(s.bone[0], pel)) return 0;
	const float* r = s.rootW.T; int floorHits = 0;
	for (uint32_t layer = 0; layer < 64; layer++) {
		uint32_t filter = 0x011F0000u | layer; bool h1 = false, h2 = false; float f1 = 1, f2 = 1, n1[3] = {}, n2[3] = {};
		const float d0[3] = { r[0], r[1], r[2] + 60 }, d1[3] = { r[0], r[1], r[2] - 60 };                         // straight down through the feet
		const float b0[3] = { pel.T[0] + 80, pel.T[1], pel.T[2] }, b1[3] = { pel.T[0] - 80, pel.T[1], pel.T[2] };   // sideways through the pelvis
		bool ok1 = PickCast(d0, d1, filter, h1, f1, n1), ok2 = PickCast(b0, b1, filter, h2, f2, n2);
		if (!ok1 || !ok2) { Log("ray: layer %02X call not possible", layer); continue; }
		res[layer].ok = true; res[layer].floorHit = h1 && f1 < 1.0f; res[layer].bodyHit = h2 && f2 < 1.0f; res[layer].z = r[2] + 60 - 120 * f1; res[layer].nz = n1[2];
		if (res[layer].floorHit) floorHits++;
		Log("ray: [%s] layer %02X  floor %s (z %.1f, normal z %.2f)  body-height %s (fraction %.2f)", tag, layer, h1 ? "HIT " : "miss", res[layer].z, n1[2], h2 ? "HIT " : "miss", f2);
	}
	return floorHits;
}
// A quick yes/no: does a straight-down ray through the feet hit something near the floor on any of a few common layers, with the current world and scale?
static bool FloorWorks(const Session& s) {
	static const uint32_t L[] = { 0x01, 0x25, 0x0D, 0x11, 0x03, 0x02, 0x08, 0x0A };
	const float* r = s.rootW.T; const float d0[3] = { r[0], r[1], r[2] + 60 }, d1[3] = { r[0], r[1], r[2] - 60 };
	for (uint32_t l : L) { bool h = false; float f = 1, n[3] = {}; if (PickCast(d0, d1, 0x011F0000u | l, h, f, n) && h && f < 1.0f && std::fabs(r[2] + 60 - 120 * f - r[2]) <= 40.0f) return true; }
	return false;
}
static bool FindWorkingWorld(const Session& s) {
	const uint32_t orig = PickWorld(); const float scales[3] = { 1.0f / 7.0f, kUnit, 1.0f };   // FNV's Havok scale is 7 game units per Havok unit (the game's bodies sit at game units / 7)
	for (float k : scales) { PickSetScale(k); if (FloorWorks(s)) return true; }
	uint32_t cand[40]; int n = PickCandidates(cand, 40); char d[160];
	for (int i = 0; i < n && i < 4; i++) { PickDescribe(cand[i], d, sizeof d); Log("ray: candidate %s", d); }
	for (int i = 0; i < n; i++) for (float k : scales) {
		PickSetWorld(cand[i]); PickSetScale(k);
		if (FloorWorks(s)) { PickDescribe(cand[i], d, sizeof d); Log("ray: %s is the world that answers (%s)", d, k == 1.0f ? "raw units" : (k < 0.05f ? "game/70" : "game/7")); PickForceWorld(cand[i]); return true; }
	}
	PickSetWorld(orig); PickSetScale(1.0f / 7.0f); Log("ray: %d objects around the found world tried, none answered a ray", n); return false;
}
static bool CalibrateRays(const Session& s) {
	if (!PickWorld()) { Log("ray: no physics world known yet, calibration skipped"); return false; }
	WorldXform pel; if (!NodeWorld(s.bone[0], pel)) return false;
	const float* r = s.rootW.T; Log("ray: bhkWorld %08X, root (%.1f %.1f %.1f), pelvis (%.1f %.1f %.1f)", PickWorld(), r[0], r[1], r[2], pel.T[0], pel.T[1], pel.T[2]);
	static LayerRes res[64]; for (auto& x : res) x = LayerRes();
	if (!FindWorkingWorld(s)) return false;
	int fh = RunLayers(s, std::fabs(PickScale() - 1.0f) < 1e-6f ? "raw units" : (PickScale() < 0.05f ? "game/70" : "game/7"), res); (void)fh;
	Log("ray: world %08X, ray ends in %s", PickWorld(), std::fabs(PickScale() - 1.0f) < 1e-6f ? "raw game units" : (PickScale() < 0.05f ? "game units / 70" : "Havok units (game units / 7)"));
	if (g_groundAuto) {                                                                      // prefer a layer that finds the floor under the feet but not the corpse itself
		int pick = -1;
		for (int pass = 0; pass < 2 && pick < 0; pass++)
			for (int l = 1; l < 64 && pick < 0; l++) { const LayerRes& x = res[l]; if (x.ok && x.floorHit && std::fabs(x.z - r[2]) <= 25.0f && x.nz > 0.5f && (pass == 1 || !x.bodyHit)) pick = l; }
		if (pick < 0) { Log("ray: no layer gave a plausible floor under the feet, ground collision stays off"); return true; }
		g_groundFilter = 0x011F0000u | (uint32_t)pick; g_groundReady = true; Log("ray: ground layer %02X chosen (floor z %.1f vs root z %.1f%s)", pick, res[pick].z, r[2], res[pick].bodyHit ? ", also sees the body" : "");
	}
	return true;
}

// Ground sampler: (x, y) in the actor's root frame, metres. Casts a vertical ray in game space and returns the height in the same frame.
static bool GroundRayCb(void* ctx, float x, float y, float& z) {
	Session& s = *(Session*)ctx; if (s.groundFailed) return false;
	const float toUnits = (s.rootW.S != 0 ? s.rootW.S : 1.0f) / kUnit;                  // metres in the root frame -> game units
	V3 w = mulMV(s.rootW.R, V3{ x * toUnits, y * toUnits, 0 });
	const float top = s.rootW.T[2] + 1.4f * toUnits, bottom = s.rootW.T[2] - 1.3f * toUnits;   // from above the head to below the feet
	float from[3] = { s.rootW.T[0] + w.x, s.rootW.T[1] + w.y, top }, to[3] = { from[0], from[1], bottom };
	bool hit = false; float fr = 1, n[3];
	if (!PickCast(from, to, g_groundFilter, hit, fr, n)) { s.groundFailed = true; return false; }
	if (!hit || fr >= 1.0f) return false;
	z = (top - (top - bottom) * fr - s.rootW.T[2]) / toUnits; s.groundHits++; return true;
}

static unsigned HiddenMask(const Session& s) {                    // bit i set = rig part i's bone is scaled to (almost) nothing, i.e. cut off
	const float base = s.rootW.S != 0 ? s.rootW.S : 1.0f; unsigned m = 0;
	for (int i = 0; i < 21; i++) { WorldXform w; if (NodeWorld(s.bone[i], w) && w.S < g_x.hiddenScale * base) m |= 1u << i; }
	return m;
}
static unsigned HeadMask() { static unsigned m = 0; if (!m) for (int i = 0; i < 21; i++) if (g_rig.part[i].ref.find("Head") != std::string::npos) m |= 1u << i; return m; }
static unsigned LegMask() { static unsigned m = 0; if (!m) for (int i = 1; i <= 6; i++) m |= 1u << i; return m; }   // thighs, shins, feet
static int HiddenLimb(const Session& s) {                        // index of a rig part whose bone is scaled to (almost) nothing, or -1
	const float base = s.rootW.S != 0 ? s.rootW.S : 1.0f;
	for (int i = 0; i < 21; i++) { WorldXform w; if (NodeWorld(s.bone[i], w) && w.S < g_x.hiddenScale * base) return i; }
	return -1;
}


// Length of the left leg (thigh to calf) in game units: the yardstick for how big an actor is; the player's own skeleton is the human reference.
static float LegLength(uint32_t actor) {
	const uint32_t root = ActorRoot(actor); if (!root) return -1.0f;
	auto b = CollectBones(root); auto t = b.find("Bip01 L Thigh"), c = b.find("Bip01 L Calf"); if (t == b.end() || c == b.end()) return -1.0f;
	WorldXform a, d; if (!NodeWorld(t->second, a) || !NodeWorld(c->second, d)) return -1.0f;
	const float dx = a.T[0] - d.T[0], dy = a.T[1] - d.T[1], dz = a.T[2] - d.T[2]; return std::sqrt(dx * dx + dy * dy + dz * dz);
}
static float BodyRatio(uint32_t actor) {
	static float base = -1.0f; if (base <= 0) { uint32_t pl = 0; if (Rd(0x011DEA3C, &pl, 4) && pl) base = LegLength(pl); }
	if (base <= 0) return -1.0f; const float l = LegLength(actor); return l > 0 ? l / base : -1.0f;
}
static void Begin(uint32_t actor, bool knock = false, int knockState = 0) {
	const double b0 = NowMs();
	DWORD now = GetTickCount();
	for (Session* a : g_active) if (a->actor == actor) return;
	uint32_t ls0 = 0xFFFF; Rd(actor + 0x108, &ls0, 4); const bool uncon = ls0 == 3 && g_x.unconscious;
	if (ls0 == 3 && !g_x.unconscious) return;
	if (uncon) { knock = true; knockState = 3; }
	std::map<uint32_t, DWORD>& rmap = knock ? g_recentKnock : g_recent;
	{ uint32_t avt = 0; Rd(actor, &avt, 4); if (avt == 0x10870AC && !g_x.creatures) return; }
	auto rc = rmap.find(actor); if (rc != rmap.end() && now - rc->second < (knock ? 3000u : 90000u)) return;         // dying then dead fire two bursts
	int sim = 0; for (Session* a : g_active) if (a->phase < 3) sim++;
	if (sim >= g_maxActors) return;                                                        // only bodies still being simulated count against the limit
	if (g_x.maxDistance > 0) {                                                             // far corpses are not worth the cost: the game's own ragdoll will do
		uint32_t pl = 0; float ap[3], pp[3];
		if (Rd(0x011DEA3C, &pl, 4) && pl && Rd(actor + 0x30, ap, 12) && Rd(pl + 0x30, pp, 12)) { const float dx = ap[0] - pp[0], dy = ap[1] - pp[1], dz = ap[2] - pp[2]; if (dx * dx + dy * dy + dz * dz > g_x.maxDistance * g_x.maxDistance) { rmap[actor] = now; return; } }
	}
	if (g_x.frameBudgetMs > 0 && g_lodScale <= 0.3f && g_active.size() >= 3) { rmap[actor] = now; static int said2 = 0; if (said2++ < 3) Log("observe: over the frame budget with %d ragdolls, leaving actor %08X to the game", (int)g_active.size(), actor); return; }
	if (g_x.minFps > 0 && g_fps > 0 && g_fps < g_x.minFps) { rmap[actor] = now; static int said = 0; if (said++ < 5) Log("observe: frame rate %.0f is below MinFps %.0f, corpse left to the game", g_fps, g_x.minFps); return; }
	if (g_active.size() >= 24) for (size_t i = 0; i < g_active.size(); i++) if (g_active[i]->phase == 3) { Free(g_active[i]); delete g_active[i]; g_active.erase(g_active.begin() + i); break; }   // drop the oldest held pose
	rmap[actor] = now;
	Session* s = new Session(); s->actor = actor; s->id = ++g_started; s->knock = knock; s->knockState = knockState; s->unconscious = uncon; s->verbose = s->id <= 4;
	if (knock) { static int kn = 0; s->verbose = kn++ < 6; }
	if (s->verbose) { if (knock) Log(uncon ? "observe[%u]: unconscious (life state 3, knock state %d), actor %08X" : "observe[%u]: knockdown (game knock state %d), actor %08X", s->id, knockState, actor); else Log("observe[%u]: death burst, actor %08X", s->id, actor); }
	{ uint32_t pl = 0; if (Rd(0x011DEA3C, &pl, 4) && pl == actor) { s->verbose = true; uint8_t tp = 0; uint32_t fp = 0; Rd(actor + 0x64A, &tp, 1); Rd(actor + 0x68C, &fp, 4); Log("observe[%u]: this is the player (third-person flag %u, first-person node %08X)", s->id, (unsigned)tp, fp); } }
	s->root = ActorRoot(actor); Rd(actor, &s->vt0, 4); Rd(actor + 0xC, &s->refId, 4);   // identity, to notice the memory being reused by another actor
	if (!s->root || !NodeWorld(s->root, s->rootW)) { if (s->verbose) Log("observe[%u]: cannot read the actor's 3D", s->id); delete s; return; }
	if (!g_rig.ok || !g_api || !g_fit) { delete s; return; }
	auto bones = CollectBones(s->root);
	{ uint32_t pl = 0; if (Rd(0x011DEA3C, &pl, 4) && pl == actor) { int nb = 0; for (auto& kv : bones) { uint32_t co = 0, vt = 0, tg = 0; if (Rd(kv.second + 0x1C, &co, 4) && co && Rd(co, &vt, 4) && vt == 0x10C53DC && Rd(co + 8, &tg, 4) && tg == kv.second) nb++; }
		Log("observe[%u]: player skeleton has %d collision bodies", s->id, nb); if (nb < 17) { Log("observe[%u]: the player has no ragdoll bodies, left to the game", s->id); delete s; return; } } }
	if (!g_burnKw.empty() || !g_shockKw.empty() || g_x.dumpNodes) {                           // look for what is attached to a burning / shocked actor
		std::string dump; int shown = 0;
		for (auto& kv : bones) {
			std::string nm = kv.first; for (auto& ch : nm) ch = (char)tolower((unsigned char)ch);
			for (auto& k : g_burnKw) if (nm.find(k) != std::string::npos) s->nodeBurn = true;
			for (auto& k : g_shockKw) if (nm.find(k) != std::string::npos) s->nodeShock = true;
			if (g_x.dumpNodes && s->verbose && kv.first.compare(0, 5, "Bip01") != 0 && shown < 80) { dump += kv.first; dump += " | "; shown++; }
		}
		for (size_t i = 0; i < dump.size(); i += 200) Log("observe[%u]: other nodes: %s", s->id, dump.substr(i, 200).c_str());
	}
	s->dotCount = DamageTicks(actor, (DWORD)(g_x.dotWindow * 1000.0f));
	uint32_t cvt = 0; Rd(actor, &cvt, 4); const bool isCreature = cvt == 0x10870AC;
	// Creatures like feral ghouls have fewer spine bones: a missing one is replaced by the nearest bone of the chain (the first part to claim a bone keeps it).
	auto find = [&](const std::string& n) -> uint32_t {
		auto it = bones.find(n); if (it != bones.end()) return it->second;
		if (!isCreature) return 0;
		const char* alt[4] = { nullptr, nullptr, nullptr, nullptr };
		if (n == "Bip01 Spine2") { alt[0] = "Bip01 Spine1"; alt[1] = "Bip01 Spine"; }
		else if (n == "Bip01 Spine1") { alt[0] = "Bip01 Spine"; }
		else if (n == "Bip01 Neck1") { alt[0] = "Bip01 Neck"; alt[1] = "Bip01 Spine2"; alt[2] = "Bip01 Spine1"; alt[3] = "Bip01 Spine"; }
		else if (n == "Bip01 Neck") { alt[0] = "Bip01 Neck1"; alt[1] = "Bip01 Spine2"; alt[2] = "Bip01 Spine1"; alt[3] = "Bip01 Spine"; }
		for (const char* a : alt) if (a) { auto j = bones.find(a); if (j != bones.end()) { static int sub = 0; if (sub++ < 12) Log("observe[%u]: creature %08X has no '%s', using '%s'", s->id, actor, n.c_str(), a); return j->second; } }
		return 0;
	};
	for (int i = 0; i < 21; i++) {
		s->bone[i] = find(g_rig.part[i].ref); s->boneA[i] = find(g_rig.part[i].jointA); s->boneB[i] = find(g_rig.part[i].jointB);
		if (!s->bone[i] || !s->boneA[i] || !s->boneB[i]) { uint32_t vt = 0; Rd(actor, &vt, 4); static int nh = 0; if (nh++ < 8) Log("observe[%u]: bone '%s' not found on actor %08X (class %s): no humanoid skeleton, left to the game", s->id, g_rig.part[i].ref.c_str(), actor, vt == 0x10870AC ? "creature" : vt == 0x1086A6C ? "character" : "other"); delete s; return; }
	}
	{ const float ratio = BodyRatio(actor); static int said3 = 0;
	  if (ratio > 0) {
		if (said3++ < 40) Log("observe[%u]: actor %08X is %.2f times the player's leg length (%s)", s->id, actor, ratio, cvt == 0x10870AC ? "creature" : "character");
		if (!g_x.largeActors && ratio > g_x.largeScale) { Log("observe[%u]: actor %08X is large (%.2f), left to the game (LargeActors=0)", s->id, actor, ratio); delete s; return; } } }
	if (g_skipDismembered) {                                                                // a cut-off limb is hidden by scaling its bone to nothing; only those actors are left to the game
		s->hiddenMask = HiddenMask(*s); s->headGone = (s->hiddenMask & HeadMask()) != 0; s->legGone = (s->hiddenMask & LegMask()) != 0;
		int hid = HiddenLimb(*s);
		if (hid >= 0 && !(g_dismemberedOk && g_driveBodies)) { Log("observe: actor %08X has a cut-off limb ('%s' scaled away), left to the game", actor, g_rig.part[hid].ref.c_str()); delete s; return; }
		if (hid >= 0 && s->verbose) Log("observe[%u]: cut-off limb ('%s'): the rest of the body still gets Delirium%s", s->id, g_rig.part[hid].ref.c_str(), s->headGone ? ", and falls at once (no head)" : "");
	}
	for (int i = 0; i < 21; i++) {
		if (!g_rig.part[i].write) continue;
		uint32_t n = s->bone[i]; if (s->rotPart.count(n)) continue; s->rotPart[n] = i; int pp = i;
		for (int j = 0; j < 21; j++) if (g_rig.part[j].ref == g_rig.part[i].ref && g_rig.part[j].jointA == g_rig.part[j].jointB) { pp = j; break; }
		s->posPart[n] = pp; s->order.push_back(n);
	}
	std::stable_sort(s->order.begin(), s->order.end(), [](uint32_t a, uint32_t b) { return Depth(a) < Depth(b); });
	V3 p[21]; Q q[21];
	if (!CapturePose(*s, p, q)) { if (s->verbose) Log("observe[%u]: bone read failed", s->id); delete s; return; }
	{ std::map<uint32_t, bool> seen;                                          // remember what each cached bone looked like, to notice the game replacing or freeing it
	  auto add = [&](uint32_t n) { if (seen[n]) return; seen[n] = true; Session::NodeId id{ n, NodeParent(n), 0 }; Rd(n + 8, &id.name, 4); s->ids.push_back(id); };
	  for (int i = 0; i < 21; i++) { add(s->bone[i]); add(s->boneA[i]); add(s->boneB[i]); } }
	memcpy(s->p0, p, sizeof p); memcpy(s->q0, q, sizeof q);
	if (g_calibrate || g_worldCollision) EnsureWorld(*s);
	if (g_x.fall && PickWorld() && g_groundReady) {                                          // how far above the floor is it? (shot mid-air, thrown off a ledge)
		const float* r = s->rootW.T; const float from[3] = { r[0], r[1], r[2] + 20.0f }, to[3] = { r[0], r[1], r[2] - 6000.0f }; bool hit = false; float fr = 1, nrm[3] = {};
		if (PickCast(from, to, g_groundFilter, hit, fr, nrm) && hit) { const float drop = 20.0f + fr * 6020.0f - 20.0f; if (drop > g_x.fallMinHeight) s->floorDrop = drop * kUnit; }
	}
	if ((g_calibrate || (g_worldCollision && g_groundAuto)) && !g_calibDone && PickWorld()) { bool done = CalibrateRays(*s); if (done || ++g_calibTries >= 3) g_calibDone = true; }   // retried on later kills until it has run
	
	if (g_worldCollision && PickWorld() && g_groundReady && s->floorDrop <= 0.0f) { s->ground = new GroundGrid(); GroundInit(*s->ground, 0, 0, g_groundCell, g_groundHalf, -1.0f, 0.9f); }
	LARGE_INTEGER c; QueryPerformanceCounter(&c); s->t0 = c; s->last = c; s->phase = 1;
	s->beginMs = NowMs() - b0;
	g_active.push_back(s);
}

// Frame time in game seconds: wall clock until the game's own scaled frame time is readable (VATS, slow motion and "turbo" scale it).
static float GameDt(float wallDt) { float g = GameFrameSeconds(); return std::min(std::max(g > 0 ? g : wallDt, 0.0005f), 0.05f); }

// The game's own ragdoll bodies would keep simulating (and keep being what the crosshair, bullets and looting see). Switch them to
// keyframed so they follow the bones we write, as the game does itself when a corpse comes to rest.
static void KeyframeGameBodies(Session& s) {
	if (!g_keyframe) return;
	int n = 0, bad = 0;
	for (auto& kv : CollectBones(s.root)) {
		uint32_t node = kv.second, co = 0, vt = 0, target = 0;
		if (!Rd(node + 0x1C, &co, 4) || !co || !Rd(co, &vt, 4) || vt != 0x10C53DC || !Rd(co + 8, &target, 4) || target != node) continue;   // must be a bhkBlendCollisionObject pointing back at this node
		g_ours = 1; bool okm = GameSetMotionType(co, 4); g_ours = 0;
		if (okm) { n++; s.cos.push_back(co); } else bad++;
	}
	s.wakeSeen = g_ww;
	if (s.verbose) Log("observe[%u]: %d of the game's bodies switched to keyframed%s", s.id, n, bad ? " (some failed)" : "");
}

// ---- discovery only: how does the game store a rigid body's position and velocity? Samples three game bodies on consecutive warm-up frames, while the game's own
// ragdoll is still moving them, and logs which dwords change, next to the speed measured from the bones. Read-only.
static bool g_probe = false;
static void ProbeBodies(Session& s, const V3* p, LARGE_INTEGER c) {
	if (!g_probe || s.id > 2) return;
	static const char* names[3] = { "Bip01 Pelvis", "Bip01 Spine1", "Bip01 Head" };
	static uint32_t snap[3][128]; static uint32_t addr[3]; static int sid = 0;
	auto bones = CollectBones(s.root); LARGE_INTEGER f; QueryPerformanceFrequency(&f);
	const float wall = s.warm ? (float)((double)(c.QuadPart - s.t0.QuadPart) / (double)f.QuadPart) : 0;
	if (s.warm >= 1 && wall > 0) Log("probe[%u] frame %u: pelvis bone speed (%.2f %.2f %.2f) m/s over %.1f ms", s.id, s.warm, (p[0].x - s.p0[0].x) / wall, (p[0].y - s.p0[0].y) / wall, (p[0].z - s.p0[0].z) / wall, wall * 1000);
	for (int k = 0; k < 3; k++) {
		auto it = bones.find(names[k]); if (it == bones.end()) continue;
		uint32_t co = 0, body = 0, hkb = 0; if (!Rd(it->second + 0x1C, &co, 4) || !co || !Rd(co + 0x10, &body, 4) || !body || !Rd(body + 8, &hkb, 4) || !hkb) continue;
		uint32_t now[128]; if (!Rd(hkb, now, sizeof now)) continue;
		if (sid == s.id && addr[k] == hkb && s.warm >= 2) {
			int logged = 0;
			for (int i = 0; i < 128 && logged < 36; i++) if (now[i] != snap[k][i]) {
				float a, b; memcpy(&a, &snap[k][i], 4); memcpy(&b, &now[i], 4); bool fa = std::fabs(a) < 1e5f && std::fabs(b) < 1e5f && (std::fabs(a) > 1e-6f || std::fabs(b) > 1e-6f);
				if (fa) Log("probe[%u] %s hkp %08X +0x%03X: %.4f -> %.4f", s.id, names[k], hkb, i * 4, a, b); else Log("probe[%u] %s hkp %08X +0x%03X: %08X -> %08X", s.id, names[k], hkb, i * 4, snap[k][i], now[i]);
				logged++;
			}
		}
		memcpy(snap[k], now, sizeof now); addr[k] = hkb;
	}
	sid = s.id;
}

// ---- direct drive: Delirium's pose becomes velocities on the game's own (simulated) bodies. The game's solver then does collision, constraints, looting and grabbing itself.
static bool TargetWorld(const Session& s, const V3* P, const Q* Qp, uint32_t n, WorldXform& w) {
	auto ir = s.rotPart.find(n), ip = s.posPart.find(n); if (ir == s.rotPart.end() || ip == s.posPart.end()) return false;
	const int rp = ir->second, pp = ip->second;
	V3 joint = P[pp] + rot(Qp[pp], g_rig.part[pp].anchor);
	float Rb[9]; toMat(norm(mul(Qp[rp], g_rig.part[rp].offset)), Rb);
	w = WorldXform{}; mulMM(s.rootW.R, Rb, w.R);
	V3 t = mulMV(s.rootW.R, joint * (s.rootW.S / kUnit)); w.T[0] = s.rootW.T[0] + t.x; w.T[1] = s.rootW.T[1] + t.y; w.T[2] = s.rootW.T[2] + t.z; return true;
}
static bool SetupDriveBodies(Session& s) {
	s.dbodies.clear();
	for (auto& kv : s.rotPart) {
		uint32_t node = kv.first, co = 0, vt = 0, target = 0, body = 0;
		if (!Rd(node + 0x1C, &co, 4) || !co || !Rd(co, &vt, 4) || vt != 0x10C53DC || !Rd(co + 8, &target, 4) || target != node || !Rd(co + 0x10, &body, 4) || !GameBodyOk(body)) continue;
		s.dbodies.push_back({ node, body });
	}
	if (s.dbodies.empty()) {                                                                     // say why, once
		for (auto& kv : s.rotPart) { uint32_t co = 0, vt = 0, tg = 0, body = 0; if (!Rd(kv.first + 0x1C, &co, 4) || !co) { Log("observe[%u]: first body: node %08X has no collision object", s.id, kv.first); break; }
			Rd(co, &vt, 4); Rd(co + 8, &tg, 4); Rd(co + 0x10, &body, 4); char d[200]; GameBodyDescribe(body, d, sizeof d); Log("observe[%u]: first body refused: co %08X vt %08X target %08X (node %08X); %s", s.id, co, vt, tg, kv.first, d); break; }
	}
	if (s.verbose) Log("observe[%u]: driving %u of the game's bodies by velocity", s.id, (unsigned)s.dbodies.size());
	return s.dbodies.size() >= 10;
}
static void DriveBodies(Session& s, const float* tr) {
	V3 P[21]; Q Qp[21];
	for (int i = 0; i < 21; i++) { P[i] = { tr[i * 7], tr[i * 7 + 1], tr[i * 7 + 2] }; Qp[i] = norm({ tr[i * 7 + 3], tr[i * 7 + 4], tr[i * 7 + 5], tr[i * 7 + 6] }); }
	const float tau = g_x.driveTau, maxLin = g_driveMax * (s.kind >= 3 ? 2.0f : 1.0f) / kUnit / 7.0f, maxAng = g_x.driveMaxAngular;
	// Blocked bodies (a fence, a wall) must not be pushed ever harder: that tunnels them through. The further behind the bodies are on average, the softer the push.
	const float soft = s.driveErr > g_x.driveSoftAbove ? std::max(0.2f, g_x.driveSoftAbove / s.driveErr) : 1.0f;    // Havok units per second = game units per second / 7
	float errSum = 0; unsigned n = 0;
	for (auto& d : s.dbodies) {
		WorldXform tw, now; if (!TargetWorld(s, P, Qp, d.node, tw) || !NodeWorld(d.node, now)) continue;
		if (now.S < g_x.hiddenScale * (s.rootW.S != 0 ? s.rootW.S : 1.0f)) continue;                 // a cut-off limb is the game's business
		V3 e{ tw.T[0] - now.T[0], tw.T[1] - now.T[1], tw.T[2] - now.T[2] }; errSum += len(e); n++;
		if (len(e) > g_x.driveMaxCatchUp) e = e * (g_x.driveMaxCatchUp / len(e));                                  // never ask for more than about 0.7 m of catching up at once
		V3 lin = e * (g_driveGain * soft / tau / 7.0f); float ll = len(lin); if (ll > maxLin) lin = lin * (maxLin / ll);
		float Re[9]; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) Re[r * 3 + c] = tw.R[r * 3] * now.R[c * 3] + tw.R[r * 3 + 1] * now.R[c * 3 + 1] + tw.R[r * 3 + 2] * now.R[c * 3 + 2];   // target * current^T
		normaliseCols(Re); Q q = norm(fromMat(Re)); if (q.w < 0) { q = { -q.x, -q.y, -q.z, -q.w }; }
		float sh = std::sqrt(std::max(0.0f, 1.0f - q.w * q.w)); float ang = 2.0f * std::atan2(sh, q.w);
		V3 axis = sh > 1e-4f ? V3{ q.x / sh, q.y / sh, q.z / sh } : V3{ 0, 0, 0 }; V3 w = axis * (ang * g_driveAngGain * soft / tau);   // turning is driven more gently than moving: the game's joints do not allow Delirium's every twist
		float wl = len(w); if (wl > maxAng) w = w * (maxAng / wl);
		const float lv[3] = { lin.x, lin.y, lin.z }, av[3] = { w.x, w.y, w.z }; GameSetBodyVelocity(d.body, lv, av);
	}
	s.nWritten = n; if (n) { float m = errSum / n; if (s.firstErr < 0) s.firstErr = m; s.driveErr = m; }
}

// When the drive ends, the bodies still carry the last velocity we gave them. Left alone they shoot off once ("the corpse jumps"), so stop them first.
static void StopDrive(Session& s) {
	const float z[3] = { 0, 0, 0 };
	for (auto& d : s.dbodies) GameSetBodyVelocity(d.body, z, z);
}

static float RegionMul(int part) {                                  // how long a hit here lets the body keep fighting, relative to a torso hit (rig parts: 0 pelvis, 1-6 legs, 7-10 spine, 11-12 neck/head, 13-20 arms)
	if (part == 11 || part == 12) return g_mulHead;
	if (part >= 1 && part <= 6) return g_mulLeg;
	if (part >= 13) return g_mulArm;
	if (part == 0) return g_mulPelvis;
	return g_mulTorso;
}
static const char* PartName(int part) {
	static const char* n[21] = { "pelvis", "left thigh", "left shin", "left foot", "right thigh", "right shin", "right foot", "lower spine", "spine", "upper spine", "chest", "neck", "head", "left clavicle", "left upper arm", "left forearm", "left hand", "right clavicle", "right upper arm", "right forearm", "right hand" };
	return part >= 0 && part < 21 ? n[part] : "?";
}
// ---- scripted reactions on top of the engine's shot behaviour: flailing, curling, bracing (all parameters from the ini)
enum { A_WINDMILL_ON = 1, A_WRITHE_FULL, A_WRITHE_UPPER, A_PEDAL_ON, A_CURL_ON, A_ROLLUP_ON, A_EXPLOSION, A_HIGHFALL_ON, A_CATCHFALL_ON, A_STOP_ALL, A_AGONY_NEXT, A_SCRIPT };
static float Rnd01() { static unsigned seed = GetTickCount() * 2654435761u; seed = seed * 1664525u + 1013904223u; return (float)(seed >> 8) / 16777216.0f; }
struct MsgB {                                                                                  // one engine message: begin, parameters, send
	Session& s; MsgB(Session& s_, const char* n) : s(s_) { g_api->nm_msg_begin(s.agent, n); }
	MsgB& f(const char* k, float v) { g_api->nm_msg_set_float(s.agent, k, v); return *this; }
	MsgB& b(const char* k, bool v) { g_api->nm_msg_set_bool(s.agent, k, v ? 1 : 0); return *this; }
	MsgB& t(const char* k, const char* v) { g_api->nm_msg_set_string(s.agent, k, v); return *this; }
	void send() { g_api->nm_msg_send(s.agent, s.world); }
};

// ------------------------------------------------------------------------------------------------------------------------------ reaction scripts
// DeliriumFNV_reactions.txt can replace a built-in reaction (Explosion, Fall, Burn, Shock, Gut, Agony) by a list of timed engine messages.
struct TimeVars { float fall = 0, balance = 0, explosion = 0, burn = 0, shock = 0, gut = 0; };
static int MacroId(const std::string& n) {
	static const struct { const char* n; int id; } k[] = { { "@windmill", A_WINDMILL_ON }, { "@writhe", A_WRITHE_FULL }, { "@writheupper", A_WRITHE_UPPER }, { "@pedal", A_PEDAL_ON }, { "@curl", A_CURL_ON }, { "@rollup", A_ROLLUP_ON },
		{ "@explosion", A_EXPLOSION }, { "@highfall", A_HIGHFALL_ON }, { "@catchfall", A_CATCHFALL_ON }, { "@stop", A_STOP_ALL }, { "@next", A_AGONY_NEXT } };
	for (const auto& e : k) if (n == e.n) return e.id;
	return 0;
}
static float EvalTime(const std::string& e, const TimeVars& v) {                              // 1.5, $fall, $balance+0.3
	float total = 0, sign = 1; size_t i = 0;
	while (i < e.size()) {
		const char c = e[i];
		if (c == '+') { sign = 1; i++; } else if (c == '-') { sign = -1; i++; } else if (isspace((unsigned char)c)) i++;
		else if (c == '$') {
			size_t j = i + 1; while (j < e.size() && isalpha((unsigned char)e[j])) j++;
			std::string n = e.substr(i + 1, j - i - 1); for (auto& ch : n) ch = (char)tolower((unsigned char)ch);
			const float val = n == "fall" ? v.fall : n == "balance" ? v.balance : n == "explosion" ? v.explosion : n == "burn" ? v.burn : n == "shock" ? v.shock : n == "gut" ? v.gut : 0.0f;
			total += sign * val; sign = 1; i = j;
		} else { char* end = nullptr; const float val = (float)strtod(e.c_str() + i, &end); if (end == e.c_str() + i) break; total += sign * val; sign = 1; i = (size_t)(end - e.c_str()); }
	}
	return total;
}
static void SendScriptMsg(Session& s, const ScriptMsg& m) {
	if (m.macro || !s.agent) return;
	g_api->nm_msg_begin(s.agent, m.name.c_str());
	for (const auto& p : m.p) {
		const float val = p.rnd ? p.f + (p.f2 - p.f) * Rnd01() : p.f;
		switch (p.kind) {
		case 'f': g_api->nm_msg_set_float(s.agent, p.name.c_str(), val); break;
		case 'i': g_api->nm_msg_set_int(s.agent, p.name.c_str(), (int)std::lround(val)); break;
		case 'b': g_api->nm_msg_set_bool(s.agent, p.name.c_str(), p.b); break;
		case 'v': g_api->nm_msg_set_vec3(s.agent, p.name.c_str(), p.v); break;
		default: g_api->nm_msg_set_string(s.agent, p.name.c_str(), p.s.c_str()); break;
		}
	}
	if (g_api->nm_msg_send(s.agent, s.world) < 0) { static int bad = 0; if (bad++ < 12) { char e[256] = ""; g_api->nm_last_error(e, sizeof e); Log("script: message '%s' was refused by the engine: %s", m.name.c_str(), e); } }
}
// Puts the messages of a [base] block (or one of base2, base3 ... picked at random) on the timeline starting at t0. False when no such block exists.
static bool ApplyScript(Session& s, const char* base, float t0, const TimeVars& v, float* endT = nullptr, bool* hasNext = nullptr) {
	const auto blocks = ScriptBlocks(base); if (blocks.empty()) return false;
	const ScriptBlock* b = blocks[std::min(blocks.size() - 1, (size_t)(Rnd01() * (float)blocks.size()))];
	const size_t first = s.tl.size(); float last = t0; bool nxt = false;
	for (const auto& m : b->msgs) {
		const float t = t0 + EvalTime(m.tx, v); last = std::max(last, t);
		if (m.macro) { const int id = MacroId(m.name); if (!id) { static int bad = 0; if (bad++ < 6) Log("script: unknown macro %s in [%s]", m.name.c_str(), b->name.c_str()); continue; } if (id == A_AGONY_NEXT) nxt = true; s.tl.push_back({ t, id, 0.0f }); }
		else { s.smsgs.push_back(m); s.tl.push_back({ t, A_SCRIPT, (float)(s.smsgs.size() - 1) }); }
	}
	std::stable_sort(s.tl.begin() + (long)first, s.tl.end(), [](const Session::TA& a, const Session::TA& c) { return a.t < c.t; });
	if (endT) *endT = last; if (hasNext) *hasNext = nxt;
	return true;
}
// A knocked-down actor keeps fumbling on the floor until the game gets it up: cycles of writhing, pedalling and curling with short limp pauses.
static void ScheduleAgony(Session& s) {
	if (s.unconscious && s.rt > g_x.unconsciousAgonySeconds) return;                       // an unconscious actor stops writhing after a while and lies limp
	{ const char* base = "Agony"; if (g_x.woundAgony && s.woundPart >= 0) { const char* wb = (s.woundPart >= 1 && s.woundPart <= 6) ? "AgonyLeg" : "AgonyGut"; if (!ScriptBlocks(wb).empty()) base = wb; }
	  float endT = 0; bool nxt = false; TimeVars tv;
	  if (ApplyScript(s, base, s.rt, tv, &endT, &nxt)) { if (!nxt) s.tl.push_back({ endT + g_x.knockAgonyPause * (0.6f + 0.8f * Rnd01()), A_AGONY_NEXT, 0.0f }); return; } }
	const float t = s.rt, len = std::max(1.5f, g_x.knockAgonySeconds * (0.7f + 0.6f * Rnd01()));
	float r = Rnd01();
	if (g_x.woundAgony && s.woundPart >= 0) { const int w = s.woundPart; const bool leg = w >= 1 && w <= 6; r = leg ? r * 0.7f : 0.3f + r * 0.7f; }   // a leg shot writhes and pedals; a gut, arm or head shot curls up around the wound
	s.tl.push_back({ t, A_STOP_ALL, 0 });
	if (r < 0.4f) { s.tl.push_back({ t + 0.05f, A_WRITHE_FULL, 0 }); s.tl.push_back({ t + 0.05f, A_PEDAL_ON, 0 }); }
	else if (r < 0.7f) { s.tl.push_back({ t + 0.05f, A_ROLLUP_ON, 0 }); s.tl.push_back({ t + 0.05f, A_PEDAL_ON, 0 }); }
	else { s.tl.push_back({ t + 0.05f, A_CURL_ON, 0 }); s.tl.push_back({ t + len * 0.5f, A_STOP_ALL, 0 }); s.tl.push_back({ t + len * 0.5f + 0.05f, A_WRITHE_UPPER, 0 }); s.tl.push_back({ t + len * 0.5f + 0.05f, A_PEDAL_ON, 0 }); }
	s.tl.push_back({ t + len, A_STOP_ALL, 0 }); s.tl.push_back({ t + len + g_x.knockAgonyPause * (0.6f + 0.8f * Rnd01()), A_AGONY_NEXT, 0 });
}
static void DoAct(Session& s, int a, float v) {
	(void)v; const float K = g_x.flailStiffness, A = g_x.flailAmplitude, P = g_x.flailPeriod;
	switch (a) {
	case A_WINDMILL_ON: MsgB(s, "armsWindmillAdaptive").b("start", true).f("armStiffness", K).f("bodyStiffness", K * 0.9f).f("period", P).f("amplitude", A).f("phase", 0.0f).b("disableOnImpact", true).send(); break;
	case A_WRITHE_FULL: case A_WRITHE_UPPER:
		MsgB(s, "bodyWrithe").b("start", true).f("armStiffness", K).f("legStiffness", K).f("backStiffness", K).f("legPeriod", P * 0.9f).f("armPeriod", P).f("backPeriod", P * 1.3f)
			.f("legDamping", 1.0f).f("armDamping", 1.0f).f("backDamping", 1.0f).f("legAmplitude", A).f("armAmplitude", A).f("backAmplitude", A * 0.8f).f("kneeAmplitude", A).f("elbowAmplitude", A)
			.t("mask", a == A_WRITHE_FULL ? "fb" : "ub").b("rollOverFlag", a == A_WRITHE_FULL).send(); break;
	case A_PEDAL_ON: { bool pl = true, pr = true; if (g_x.woundAgony && s.knock) { if (s.woundPart >= 1 && s.woundPart <= 3) pl = false; else if (s.woundPart >= 4 && s.woundPart <= 6) pr = false; }   // the hurt leg stays limp
		MsgB(s, "pedalLegs").b("start", true).b("backPedal", false).f("legStiffness", K).b("pedalLeftLeg", pl).b("pedalRightLeg", pr).f("radius", 0.25f).f("angularSpeed", 6.0f).f("pedalOffset", 0.0f).f("speedAsymmetry", 0.3f).b("adaptivePedal4Dragging", false).send(); } break;
	case A_CURL_ON: MsgB(s, "bodyFoetal").b("start", true).t("mask", "fb").f("stiffness", K * 0.9f).f("asymmetry", 0.3f).f("dampingFactor", 1.0f).f("backTwist", 0.3f).send(); break;
	case A_ROLLUP_ON: MsgB(s, "bodyRollUp").b("start", true).t("mask", "fb").f("stiffness", K * 0.9f).f("legPush", 0.6f).f("armReachAmount", 0.5f).f("useArmToSlowDown", 0.5f).f("asymmetricalLegs", 0.3f).send(); break;
	case A_EXPLOSION: {                                                                         // like the engine's own explosion reaction: curl and pedal, or writhe, or windmill and pedal
		MsgB(s, "stopAllBehaviours").send(); const float r = Rnd01();
		if (r < g_x.explosionRollChance) { DoAct(s, A_PEDAL_ON, 0); DoAct(s, A_ROLLUP_ON, 0); }
		else if (r < g_x.explosionRollChance + g_x.explosionWritheChance) DoAct(s, A_WRITHE_FULL, 0);
		else { DoAct(s, A_WINDMILL_ON, 0); DoAct(s, A_PEDAL_ON, 0); }
		return; }
	case A_HIGHFALL_ON: MsgB(s, "highFall").b("start", true).f("bodyStiffness", K * 0.85f).f("armAmplitude", A).f("armPeriod", P * 0.6f).send(); break;
	case A_CATCHFALL_ON: MsgB(s, "stopAllBehaviours").send(); g_api->nm_agent_task_catch_fall(s.agent, nullptr); break;
	case A_STOP_ALL: MsgB(s, "stopAllBehaviours").send(); break;
	case A_AGONY_NEXT: ScheduleAgony(s); break;
	case A_SCRIPT: if ((size_t)v < s.smsgs.size()) SendScriptMsg(s, s.smsgs[(size_t)v]); break;
	}
}

static bool StartAgent(Session& s, const V3* p, const Q* q, LARGE_INTEGER c, float dt) {
	const double T0 = NowMs();
	uint32_t ls = 0xFFFF; Rd(s.actor + 0x108, &ls, 4);                    // Actor::lifeState: 1 dying, 2 dead
	if (s.verbose) Log("observe[%u]: actor %08X life state %u one frame after the burst", s.id, s.actor, ls);
	if (g_requireDead && !s.knock && ls != 1 && ls != 2) { if (s.verbose) Log("observe[%u]: not dying or dead, leaving it to the game", s.id); return false; }
	float st[21 * 13], zp[21 * 7]; V3 vel[21];
	V3 rv[21], vMean{ 0, 0, 0 };                                                              // the motion the game's own ragdoll gave each part in its first frames
	for (int i = 0; i < 21; i++) { rv[i] = (p[i] - s.p0[i]) * (g_velScale / dt); vMean = vMean + rv[i] * (1.0f / 21.0f); }
	int hitPart = 10; V3 hitDir{ 0, 0, 0 }; bool located = false;                              // the kill impulse lands on the part that was hit, so that part moves most relative to the rest
	if (g_hitLoc) { float best = 0; for (int i = 0; i < 21; i++) { V3 d = rv[i] - vMean; float l = len(d); if (l > best) { best = l; hitPart = i; hitDir = d; } } located = best >= g_hitMin; if (!located) { hitPart = 10; hitDir = { 0, 0, 0 }; } }
	const float chestSpeed = len(rv[10]);
	const float regionMul = located ? RegionMul(hitPart) : 1.0f;
	const bool willBalance = !s.knock && g_balance && !s.headGone && !s.legGone && chestSpeed <= g_balanceMaxSpeed && regionMul > 0.0f;
	const float bf = willBalance ? g_balanceVelScale : 1.0f, vs = g_velScale * bf;                       // a body that is meant to stay up inherits less of the kill's motion
	for (int i = 0; i < 21; i++) {
		vel[i] = rv[i] * bf;
		Q dq = mul(q[i], conj(s.q0[i])); if (dq.w < 0) dq = { -dq.x, -dq.y, -dq.z, -dq.w };
		float w = std::min(1.0f, dq.w), ang = 2 * std::acos(w), sn = std::sqrt(std::max(0.0f, 1 - w * w));
		V3 av = sn > 1e-5f ? V3{ dq.x / sn, dq.y / sn, dq.z / sn } * (ang * vs / dt) : V3{ 0, 0, 0 };
		float* o = st + i * 13; o[0] = p[i].x; o[1] = p[i].y; o[2] = p[i].z; o[3] = q[i].x; o[4] = q[i].y; o[5] = q[i].z; o[6] = q[i].w;
		o[7] = vel[i].x; o[8] = vel[i].y; o[9] = vel[i].z; o[10] = av.x; o[11] = av.y; o[12] = av.z;
		float* t = zp + i * 7; t[0] = p[i].x; t[1] = p[i].y; t[2] = p[i].z; t[3] = q[i].x; t[4] = q[i].y; t[5] = q[i].z; t[6] = q[i].w;
		s.prev[i * 3] = p[i].x; s.prev[i * 3 + 1] = p[i].y; s.prev[i * 3 + 2] = p[i].z;
	}
	const float zero[3] = { 0, 0, 0 };
	s.world = g_api->nm_world_create();
	bool meshed = false;
	if (s.ground) {
		int total = s.ground->total();
		if (s.groundFailed || s.groundHits < (unsigned)(total / 10)) { if (s.verbose) Log("observe[%u]: ground: %u of %d rays hit, not trusted, using a flat floor (check GroundFilter)", s.id, s.groundHits, total); }
		else {
			std::vector<float> pos; std::vector<uint32_t> tri; float low = GroundMesh(*s.ground, 0.0f, pos, tri);
			if (g_api->nm_world_add_mesh(s.world, pos.data(), (int)pos.size() / 3, tri.data(), (int)tri.size() / 3, nullptr) == 0) {
				g_api->nm_world_add_floor(s.world, 0, 0, low - 0.02f, 100.0f); meshed = true;
				if (s.verbose) Log("observe[%u]: ground mesh from %u/%d rays, lowest %.2f m, %d triangles", s.id, s.groundHits, total, low, (int)tri.size() / 3);
			}
		}
		delete s.ground; s.ground = nullptr;
	}
	if (!meshed) g_api->nm_world_add_floor(s.world, 0, 0, s.floorDrop > 0 ? -s.floorDrop - 0.02f : 0.0f, 100.0f);
	const double T1 = NowMs();
	if (!g_pool.empty()) { s.agent = g_pool.back(); g_pool.pop_back(); } else { const double c0 = NowMs(); s.agent = g_api->nm_agent_create(g_fit, zero, 0.0f, 0.3f); if (s.verbose) Log("observe[%u]: no agent in the pool, built one in %.1f ms", s.id, NowMs() - c0); }
	if (!s.agent) return false;
	g_api->nm_agent_set_mode(s.agent, NM_MODE_LEAD);
	g_api->nm_agent_set_part_states(s.agent, st, 21); g_api->nm_agent_set_zero_pose(s.agent, zp, 21);
	const double T2 = NowMs();
	V3 chest = p[10], cv = vel[10]; float hs = std::sqrt(cv.x * cv.x + cv.y * cv.y);
	float at[3] = { chest.x, chest.y, chest.z }, dir[3] = { hs > 0.05f ? cv.x / hs : 0.0f, hs > 0.05f ? cv.y / hs : 1.0f, 0.0f };
	if (located) {                                                                                // aim the engine's shot reaction at the part that was really hit, from the direction it was kicked
		at[0] = p[hitPart].x; at[1] = p[hitPart].y; at[2] = p[hitPart].z;
		const float hl = len(hitDir); if (hl > 0.05f) { dir[0] = hitDir.x / hl; dir[1] = hitDir.y / hl; dir[2] = hitDir.z / hl; }
		if (s.verbose) Log("observe[%u]: hit location: %s (part %d), kicked %.1f m/s relative to the rest", s.id, PartName(hitPart), hitPart, len(hitDir));
	} else if (s.verbose && g_hitLoc) Log("observe[%u]: hit location unclear, treating it as a chest hit", s.id);
	s.woundPart = located ? hitPart : -1;
	if (g_x.stagger && !s.knock) {                                                               // push it away from whoever shot it
		float ap[3], vp[3];
		if (AttackerPos(s.actor, ap) && Rd(s.actor + 0x30, vp, 12)) {
			const float dx = vp[0] - ap[0], dy = vp[1] - ap[1], l = std::sqrt(dx * dx + dy * dy);
			if (l > 1.0f) { V3 rd = mulMTV(s.rootW.R, V3{ dx / l, dy / l, 0.0f }); const float rl = len(rd); if (rl > 0.01f) { dir[0] = rd.x / rl; dir[1] = rd.y / rl; dir[2] = rd.z / rl; if (s.verbose) Log("observe[%u]: staggering away from the attacker", s.id); } }
		}
	}
	float health = g_shotHealth;                                                              // 'fatal' by default: the body collapses at once
	const float rr = Rnd01();
	const bool airborne = g_x.fall && s.floorDrop > 0.0f && !s.headGone;                          // shot in the air, or thrown off a ledge
	const bool burning = !airborne && g_x.burn && willBalance && (s.nodeBurn || (g_x.burnByDamageTicks && s.dotCount >= g_x.dotTicks && chestSpeed < 2.0f));
	const bool shocked = !airborne && !burning && g_x.shock && willBalance && s.nodeShock;
	const bool thrown = !airborne && !burning && !shocked && g_x.explosion && !s.headGone && (s.knock ? s.knockState == 1 : (regionMul > 0.0f && chestSpeed > g_balanceMaxSpeed));
	const bool gut = g_x.gutCurl && located && (hitPart == 0 || hitPart == 7 || hitPart == 8) && Rnd01() < g_x.gutCurlChance;
	s.kind = airborne ? 4 : burning ? 1 : shocked ? 2 : thrown ? 3 : 0;
	bool skipShot = false;
	if (s.kind == 4) {                                                                         // falling: arms flail, legs pedal, then brace for the landing
		skipShot = true; s.balancing = false; s.relaxed = true;
		const float T = std::sqrt(2.0f * s.floorDrop / 9.81f);
		TimeVars tv; tv.fall = T;
		if (!ApplyScript(s, "Fall", 0.0f, tv)) {
			s.tl.push_back({ 0.0f, A_HIGHFALL_ON, 0 });
			if (Rnd01() < g_x.fallCatchChance) s.tl.push_back({ std::max(0.1f, T - 0.3f), A_CATCHFALL_ON, 0 });
			s.tl.push_back({ T + 0.8f, A_STOP_ALL, 0 });
		}
		Log("observe[%u]: reaction: falling %.1f m", s.id, s.floorDrop);
	} else if (s.kind == 3) {                                                                  // thrown by a blast: windmill, pedal or writhe in the air
		skipShot = true; s.balancing = false; s.relaxed = true;
		TimeVars tv; tv.explosion = g_x.explosionSeconds * (0.8f + 0.4f * rr);
		if (!ApplyScript(s, "Explosion", 0.0f, tv)) {
			s.tl.push_back({ 0.0f, A_EXPLOSION, 0 });
			s.tl.push_back({ tv.explosion, A_STOP_ALL, 0 });
		}
		if (s.verbose) Log("observe[%u]: reaction: thrown (chest speed %.1f m/s)", s.id, chestSpeed);
	} else if (willBalance) {                                                                  // dying but not thrown: stay on its feet and stumble for a moment, then give up
		s.balanceFor = g_balanceSeconds * (0.7f + 0.6f * rr) * regionMul; s.balanceT = 0; s.relaxed = false;
		g_api->nm_set_task_param("NMShot_Standing", "*", "shotMaxSteps", (float)g_balanceSteps);
		health = g_balanceHealth; s.balancing = true;                                             // 100 or more keeps the body upright until we tell it to relax
		if (!g_balancePush) { dir[0] = dir[1] = dir[2] = 0.0f; }
		if (s.kind == 1) {                                                                       // burning: arms windmill while it staggers, then it writhes on the ground
			s.balanceFor = g_x.burnBalanceSeconds * (0.7f + 0.6f * rr); const float t = s.balanceFor + 0.05f;
			TimeVars tv; tv.balance = s.balanceFor; tv.burn = g_x.burnFlailSeconds;
			if (!ApplyScript(s, "Burn", 0.0f, tv)) { s.tl.push_back({ 0.0f, A_WINDMILL_ON, 0 }); s.tl.push_back({ t, A_WRITHE_FULL, 0 }); s.tl.push_back({ t, A_PEDAL_ON, 0 }); s.tl.push_back({ t + g_x.burnFlailSeconds, A_STOP_ALL, 0 }); }
			Log("observe[%u]: reaction: burning", s.id);
		} else if (s.kind == 2) {                                                                // shocked: convulses
			s.balanceFor = g_x.shockBalanceSeconds * (0.7f + 0.6f * rr); const float t = s.balanceFor + 0.05f;
			TimeVars tv; tv.balance = s.balanceFor; tv.shock = g_x.shockSeconds;
			if (!ApplyScript(s, "Shock", 0.0f, tv)) { s.tl.push_back({ 0.0f, A_WRITHE_UPPER, 0 }); s.tl.push_back({ t, A_WRITHE_FULL, 0 }); s.tl.push_back({ t + g_x.shockSeconds, A_STOP_ALL, 0 }); }
			Log("observe[%u]: reaction: shocked", s.id);
		} else if (gut) {                                                                        // hit low in the body: it folds over and curls up as it goes down
			TimeVars tv; tv.balance = s.balanceFor; tv.gut = g_x.gutCurlSeconds;
			if (!ApplyScript(s, "Gut", 0.0f, tv)) { s.tl.push_back({ s.balanceFor + 0.3f, A_CURL_ON, 0 }); s.tl.push_back({ s.balanceFor + g_x.gutCurlSeconds, A_STOP_ALL, 0 }); }
			if (s.verbose) Log("observe[%u]: reaction: gut shot, will curl up", s.id);
		}
		if (s.verbose) Log("observe[%u]: balancing for about %.1f s (chest speed %.1f m/s)", s.id, s.balanceFor, chestSpeed);
	} else if (s.verbose && g_balance) Log("observe[%u]: no balancing (%s, chest speed %.1f m/s)", s.id, s.headGone ? "head is gone" : (s.legGone ? "a leg is cut off" : (regionMul <= 0.0f ? "hit in the head" : "thrown too hard")), chestSpeed);
	if (!skipShot) g_api->nm_agent_task_shot(s.agent, s.world, nullptr, hitPart, at, dir, health);
	const double T3 = NowMs(); s.stageMs[0] = T1 - T0; s.stageMs[1] = T2 - T1; s.stageMs[2] = T3 - T2;
	if (s.knock && g_x.knockAgony && (s.knockState != 3 || g_x.knockOutAgony)) { float lastT = 0; for (const auto& e : s.tl) lastT = std::max(lastT, e.t); s.tl.push_back({ s.tl.empty() ? 1.0f : lastT + 0.15f, A_AGONY_NEXT, 0 }); if (s.verbose) Log("observe[%u]: will keep fumbling on the floor until the game gets it up", s.id); }
	s.last = c; s.frame = 0; s.phase = 2;
	s.mode = (g_driveBodies && SetupDriveBodies(s)) ? 1 : 0;
	if (!s.mode && s.hiddenMask && g_skipDismembered) { Log("observe[%u]: cut-off limb and the bodies cannot be driven, leaving the corpse to the game", s.id); return false; }
	if (!s.mode) KeyframeGameBodies(s);
	if (s.verbose) Log("observe[%u]: Delirium body started (pelvis %.2f m/s, chest %.2f m/s)", s.id, len(vel[0]), len(vel[10]));
	if (s.verbose) Log("observe[%u]: start-up stages: world and ground mesh %.1f ms, agent %.1f ms, shot %.1f ms, drive set-up %.1f ms", s.id, s.stageMs[0], s.stageMs[1], s.stageMs[2], NowMs() - T3);
	return true;
}

// Write the pose into the bones (world and local), then the world transforms of everything below, leaves included.
static void Drive(Session& s, const float* tr) {
	V3 P[21]; Q Qp[21];
	for (int i = 0; i < 21; i++) { P[i] = { tr[i * 7], tr[i * 7 + 1], tr[i * 7 + 2] }; Qp[i] = norm({ tr[i * 7 + 3], tr[i * 7 + 4], tr[i * 7 + 5], tr[i * 7 + 6] }); }
	std::map<uint32_t, WorldXform> cur; unsigned ok = 0;
	for (uint32_t n : s.order) {
		int rp = s.rotPart[n], pp = s.posPart[n];
		WorldXform now, l0; if (!NodeWorld(n, now) || !NodeLocal(n, l0)) continue;
		V3 joint = P[pp] + rot(Qp[pp], g_rig.part[pp].anchor);
		float Rb[9]; toMat(norm(mul(Qp[rp], g_rig.part[rp].offset)), Rb);
		WorldXform w{}; mulMM(s.rootW.R, Rb, w.R);
		V3 t = mulMV(s.rootW.R, joint * (s.rootW.S / kUnit)); w.T[0] = s.rootW.T[0] + t.x; w.T[1] = s.rootW.T[1] + t.y; w.T[2] = s.rootW.T[2] + t.z; w.S = now.S;       // scale is fixed up below from the parent and the local scale
		uint32_t par = NodeParent(n); WorldXform pw{};
		auto it = cur.find(par); if (it != cur.end()) pw = it->second; else if (!par || !NodeWorld(par, pw)) continue;
		if (l0.S < 0.02f) { cur[n] = now; continue; }                                    // the game scaled this part away (dismembered): leave it hidden, do not stretch it back
		w.S = pw.S * l0.S;
		WorldXform l{}; mulMTM(pw.R, w.R, l.R);
		V3 d{ w.T[0] - pw.T[0], w.T[1] - pw.T[1], w.T[2] - pw.T[2] }; float ips = pw.S != 0 ? 1.0f / pw.S : 1.0f;
		V3 lt = mulMTV(pw.R, d) * ips; l.T[0] = lt.x; l.T[1] = lt.y; l.T[2] = lt.z; l.S = l0.S;                                          // keep the game's local scale
		if (NodeWriteWorld(n, w) && NodeWriteLocal(n, l)) { cur[n] = w; s.wrote[n] = { w.T[0], w.T[1], w.T[2] }; ok++; }
	}
	s.nWritten = ok;
	std::deque<uint32_t> q; std::map<uint32_t, bool> done;
	for (auto& kv : cur) { q.push_back(kv.first); done[kv.first] = true; }
	unsigned prop = 0;
	while (!q.empty() && prop < 900) {
		uint32_t n = q.front(); q.pop_front(); const WorldXform pw = cur[n];
		std::vector<uint32_t> kids; if (!NodeIsPlainNode(n) || !NodeChildren(n, kids)) continue;
		for (uint32_t c : kids) {
			if (done[c]) continue;
			done[c] = true;
			WorldXform l; if (!NodeLocal(c, l)) continue;                        // hair and other rigid attachments are leaves: they need their world transform too
			WorldXform w{}; mulMM(pw.R, l.R, w.R); w.S = pw.S * l.S;
			V3 t = mulMV(pw.R, V3{ l.T[0], l.T[1], l.T[2] } * pw.S); w.T[0] = pw.T[0] + t.x; w.T[1] = pw.T[1] + t.y; w.T[2] = pw.T[2] + t.z;
			if (NodeWriteWorld(c, w)) { cur[c] = w; prop++; if (NodeIsPlainNode(c)) q.push_back(c); }
		}
	}
	s.nPropagated = prop;
}

static bool NodesStillValid(const Session& s) {                  // a freed or replaced bone (dismemberment swaps parts of the skeleton) must never be written to
	for (const auto& id : s.ids) {
		uint32_t vt = 0, nm = 0;
		if (!Rd(id.node, &vt, 4) || vt != 0x109B5AC || NodeParent(id.node) != id.parent || !Rd(id.node + 8, &nm, 4) || nm != id.name) return false;
	}
	return true;
}

// returns false when the session is finished
static bool Step(Session& s) {
	uint32_t vt = 0, rid = 0;
	if (!Rd(s.actor, &vt, 4) || vt != s.vt0 || !Rd(s.actor + 0xC, &rid, 4) || rid != s.refId) { if (s.verbose) Log("observe[%u]: the actor's memory now holds something else, session over", s.id); return false; }
	if (s.knock && ++s.lifeCheck % 4 == 0) {                                                  // a knocked-down actor: hand it back as soon as the game starts getting it up
		uint32_t ls = 0xFFFF; Rd(s.actor + 0x108, &ls, 4);
		if (ls == 1 || ls == 2) { s.knock = false; if (s.verbose) Log("observe[%u]: the knocked-down actor died, carrying on as a death", s.id); }
		else {
			uint32_t pr = 0; uint8_t ks = 0xFF; Rd(s.actor + 0x68, &pr, 4); if (pr) Rd(pr + 0x13C, &ks, 1);
			if (s.unconscious ? ls != 3 : (ls != 0 || ks < 1 || ks > 4)) { Log("observe[%u]: knockdown over (knock state %d, life state %u), the game takes the body back", s.id, (int)(signed char)ks, ls); if (s.mode == 1) StopDrive(s); return false; }
		}
	}
	if (!s.knock && ++s.lifeCheck % 15 == 0 && g_requireDead) {                              // revived, reloaded or replaced by a living actor: stop touching it
		uint32_t ls = 0xFFFF; Rd(s.actor + 0x108, &ls, 4);
		if (ls != 1 && ls != 2) { if (s.verbose) Log("observe[%u]: actor is no longer dying or dead (life state %u), session over", s.id, ls); return false; }
	}
	if (ActorRoot(s.actor) != s.root) { if (s.verbose) Log("observe[%u]: actor or its 3D is gone, session over", s.id); return false; }
	if (s.mode != 1 && g_skipDismembered && (s.frame & 3) == 0) { int hid = HiddenLimb(s); if (hid >= 0) { Log("observe[%u]: a limb was cut off ('%s' scaled away), handing the corpse back to the game", s.id, g_rig.part[hid].ref.c_str()); return false; } }
	if (s.frame > 5 && !s.cos.empty()) {                                                      // the game woke one of the bodies we keyframed (grabbing, an impulse): give the corpse back
		LONG w = g_ww; if (w - s.wakeSeen > 32) s.wakeSeen = w - 32;
		for (; s.wakeSeen < w; s.wakeSeen++) { uint32_t co = (uint32_t)g_wake[s.wakeSeen & 31]; for (uint32_t k : s.cos) if (k == co) { Log("observe[%u]: the game switched body %08X to a simulated motion type (grab or impulse), handing the corpse back", s.id, co); return false; } }
	}
	if (!NodesStillValid(s)) { if (s.verbose) Log("observe[%u]: a bone of this actor was replaced or freed (dismemberment or unload), session over", s.id); return false; }
	if (s.phase == 4) {                                                                      // discovery: raw velocity area of a simulated game body
		s.frame++;
		if (s.frame == 2 || s.frame == 6 || s.frame == 12 || s.frame == 24) {
			auto bones = CollectBones(s.root); auto it = bones.find("Bip01 Pelvis"); uint32_t co = 0, body = 0, hkb = 0;
			if (it != bones.end() && Rd(it->second + 0x1C, &co, 4) && co && Rd(co + 0x10, &body, 4) && body && Rd(body + 8, &hkb, 4) && hkb) {
				float f[48]; if (Rd(hkb + 0x190, f, sizeof f)) { char line[600]; size_t k = 0; for (int i = 0; i < 48 && k + 12 < sizeof line; i++) k += snprintf(line + k, sizeof line - k, "%s%.3f", (i % 4 == 0) ? (i ? " | " : "") : " ", f[i]); Log("probe[%u] after release f%u pelvis hkp %08X +0x190..+0x24F: %s", s.id, s.frame, hkb, line); }
				uint32_t t = 0; Rd(hkb + 0xE8, &t, 4); Log("probe[%u] after release f%u pelvis motion header +0xE8: %08X", s.id, s.frame, t);
			}
		}
		return s.frame < 26;
	}
	if (s.phase == 1) {                                                                      // let the game's own ragdoll run a few frames so its kill impulse shows up as velocity
		V3 p[21]; Q q[21]; LARGE_INTEGER c; QueryPerformanceCounter(&c);
		if (s.ground && !s.ground->done()) GroundWork(*s.ground, 120, GroundRayCb, &s);          // the grid is sampled over the warm-up frames to avoid a hitch
		if (!CapturePose(s, p, q)) return false;
		ProbeBodies(s, p, c);
		if (++s.warm < 4) { memcpy(s.p0, p, sizeof p); memcpy(s.q0, q, sizeof q); s.t0 = c; return true; }
		LARGE_INTEGER f; QueryPerformanceFrequency(&f); float wall = (float)((double)(c.QuadPart - s.t0.QuadPart) / (double)f.QuadPart);
		const double st0 = NowMs(); const bool okStart = StartAgent(s, p, q, c, GameDt(wall)); s.startMs = NowMs() - st0; return okStart;
	}
	if (s.phase == 2) {
		LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
		float wall = (float)((double)(c.QuadPart - s.last.QuadPart) / (double)f.QuadPart); s.last = c;
		s.frame++; float wallMs = wall * 1000, dt = GameDt(wall);       // dt is game seconds: scaled by VATS and slow motion
		s.simTime += dt; s.rt += dt;
		uint32_t worstN = 0; V3 worstD{ 0, 0, 0 }; float dev = (s.verbose && (s.frame == 1 || s.frame % 60 == 0)) ? Deviation(s, &worstN, &worstD) : 0;
		if (s.balancing && !s.relaxed) {
			s.balanceT += dt;
			if (s.balanceT >= s.balanceFor || g_api->nm_agent_has_fallen(s.agent)) {
				s.relaxed = true;
				g_api->nm_msg_begin(s.agent, "stayUpright"); g_api->nm_msg_set_bool(s.agent, "start", 0); g_api->nm_msg_send(s.agent, s.world);
				g_api->nm_msg_begin(s.agent, "shotRelax"); g_api->nm_msg_set_float(s.agent, "relaxPeriod", g_balanceFall); g_api->nm_msg_send(s.agent, s.world);
				if (s.verbose) Log("observe[%u]: out of strength after %.1f s, going down", s.id, s.balanceT);
			}
		}
		while (s.tlNext < s.tl.size() && s.rt >= s.tl[s.tlNext].t) { DoAct(s, s.tl[s.tlNext].a, s.tl[s.tlNext].v); s.tlNext++; }   // scripted flailing, curling, bracing
		if (s.lodFrame-- <= 0) {                                                                       // step rate by distance to the player, every 20 frames
			s.lodFrame = 20; const float d = DistToPlayer(s.actor); s.lodRate = 0;
			if (g_x.lod && d >= 0) s.lodRate = d < g_x.lodNear ? 0.0f : d < g_x.lodMid ? g_x.lodRateMid : g_x.lodRateFar;
			if (g_x.maxDistance > 0 && d > g_x.maxDistance * 1.3f) { Log("observe[%u]: %.0f units from the player, handing it back to the game", s.id, d); if (s.mode == 1) StopDrive(s); return false; }
		}
		float rate = g_x.simRate > 0 ? g_x.simRate : 120.0f; if (s.lodRate > 0) rate = std::min(rate, s.lodRate);
		if (g_x.frameBudgetMs > 0) rate = std::max(12.0f, rate * g_lodScale);
		s.simAcc += dt; const bool stepNow = (g_x.simRate <= 0 && s.lodRate <= 0 && g_lodScale >= 0.999f) || s.frame <= 2 || s.simAcc >= 1.0f / rate;               // the engine need not run at 144 Hz
		float sdt = dt;
		if (stepNow) { sdt = std::min(s.simAcc, 0.1f); s.simAcc = 0; const double q0 = NowMs(); g_api->nm_agent_step(s.agent, s.world, sdt); if (s.verbose && s.frame <= 2) Log("observe[%u]: engine step on frame %u took %.1f ms", s.id, (unsigned)s.frame, NowMs() - q0); if (g_api->nm_agent_get_part_transforms(s.agent, s.tr, 21) < 0) return false; }
		if (s.mode == 1) {
			if (s.balancing && !s.headGone && (s.frame & 3) == 0 && (HiddenMask(s) & HeadMask())) {   // the head was just shot off: no more balancing
				s.headGone = true; s.balancing = false; s.relaxed = true; s.tl.clear(); s.tlNext = 0;
				const float at[3] = { s.tr[70], s.tr[71], s.tr[72] }, dir[3] = { 0, 0, 0 }; g_api->nm_agent_task_shot(s.agent, s.world, nullptr, 10, at, dir, g_shotHealth);
				Log("observe[%u]: head cut off, collapsing", s.id);
			}
			if (s.balancing && !s.relaxed && !s.legGone && (s.frame & 3) == 0 && (HiddenMask(s) & LegMask())) {      // a leg was just cut off: nothing left to stand on
				s.legGone = true; s.balancing = false; s.relaxed = true; s.tl.clear(); s.tlNext = 0;
				g_api->nm_msg_begin(s.agent, "stayUpright"); g_api->nm_msg_set_bool(s.agent, "start", 0); g_api->nm_msg_send(s.agent, s.world);
				g_api->nm_msg_begin(s.agent, "shotRelax"); g_api->nm_msg_set_float(s.agent, "relaxPeriod", 0.2f); g_api->nm_msg_send(s.agent, s.world);
				Log("observe[%u]: a leg was cut off, going down at once", s.id);
			}
			DriveBodies(s, s.tr);
			if (s.simTime >= g_x.driveGiveUpSeconds && s.driveErr > g_x.driveGiveUpDistance) {                                              // Delirium and the game's bodies have parted ways (blocked, or the bodies are not following): the game carries on alone
				Log("observe[%u]: the game's bodies are %.0f units from the target after %u frames, leaving the corpse to the game", s.id, s.driveErr, s.frame);
				StopDrive(s); Free(&s); return false;
			}
		} else if (g_drive) Drive(s, s.tr);
		float fastest = s.lastFastest;
		if (stepNow) { fastest = 0;
		for (int i = 0; i < 21; i++) { V3 np{ s.tr[i * 7], s.tr[i * 7 + 1], s.tr[i * 7 + 2] }, op{ s.prev[i * 3], s.prev[i * 3 + 1], s.prev[i * 3 + 2] }; fastest = std::max(fastest, len(np - op) / sdt); s.prev[i * 3] = np.x; s.prev[i * 3 + 1] = np.y; s.prev[i * 3 + 2] = np.z; }
		const bool down = (g_api->nm_agent_has_fallen(s.agent) != 0 || g_api->nm_agent_is_settled(s.agent) != 0) && (!s.balancing || s.relaxed);   // a body that is still balancing (and has not run out of strength) is not at rest, however still it stands
		if (s.simTime >= g_x.restMinSeconds && fastest < g_restSpeed && down && s.tlNext >= s.tl.size()) s.calmT += sdt; else s.calmT = 0;   // calm, fallen, and no flailing still scripted
		s.lastFastest = fastest; }
		if (s.verbose && (s.frame == 1 || s.frame % 60 == 0))
		{ Log("observe[%u] f%03u dt=%.1fms (wall %.1f) pelvis z=%.2f head z=%.2f fastest part %.2f m/s | wrote %u bones + %u below | game moved them %.2f units", s.id, s.frame, dt * 1000, wallMs, s.tr[2], s.tr[12 * 7 + 2], fastest, s.nWritten, s.nPropagated, s.mode == 1 ? s.driveErr : dev);
		  if (s.mode == 1) Log("observe[%u]   body drive: mean distance to the target %.1f units", s.id, s.driveErr);
		  if (dev > 1.0f && s.mode == 0) { std::string nm; NodeName(worstN, nm); Log("observe[%u]   worst bone '%s' off by (%.1f %.1f %.1f)", s.id, nm.c_str(), worstD.x, worstD.y, worstD.z); } }
		const bool agony = s.knock && !s.unconscious && g_x.knockAgony && (s.knockState != 3 || g_x.knockOutAgony);
		if (agony ? s.simTime >= 60.0f : (s.calmT >= g_x.restSeconds || s.simTime >= g_simSeconds)) {
			Free(&s);
			if (s.mode == 1) { StopDrive(s); Log("observe[%u]: at rest after %u frames, the game's bodies carry on from here", s.id, s.frame); return false; }
			if ((g_release || (g_probe && s.id <= 2)) && !s.cos.empty()) {                                    // hand the resting corpse to the game: its bodies become simulated again, so it can be grabbed and dragged like a vanilla one
				int n = 0; g_ours = 1; for (uint32_t co : s.cos) if (GameSetMotionType(co, 1)) n++; g_ours = 0;
				Log("observe[%u]: at rest after %u frames, released %d game bodies back to simulation", s.id, s.frame, n);
				if (g_probe && s.id <= 2) { s.phase = 4; s.frame = 0; return true; }       // keep the session a few frames to watch the now simulated bodies
				return false;
			}
			s.phase = 3; s.hold = 0;
			if (s.verbose) Log("observe[%u]: body at rest after %u frames, now only holding the final pose", s.id, s.frame);
		}
		return true;
	}
	s.hold++;                                                      // phase 3: no simulation; re-assert the pose only if the game moved the bones
	if (Deviation(s) > 0.05f) { if (g_drive) Drive(s, s.tr); s.redrives++; }
	if (s.hold >= (unsigned)(g_holdSeconds * 60)) { if (s.verbose) Log("observe[%u]: hold finished (%u re-writes)", s.id, s.redrives); return false; }
	return true;
}

void ObserveReset() {
	g_recentKnock.clear();                                          // save load, new game, exit to menu: every actor pointer we hold becomes invalid
	for (Session* s : g_active) { Free(s); delete s; }
	if (!g_active.empty()) Log("observe: game state changed, released %u sessions", (unsigned)g_active.size());
	g_active.clear(); g_recent.clear(); g_qr = g_qw; g_lastQ = 0; g_lastDmgActor = 0;
}

static void PerfLog(const Session& s) {
	if (g_x.profile && s.costN) Log("perf[%u]: %u frames, plugin cost avg %.3f ms, max %.2f ms; start-up: Begin %.1f ms, engine %.1f ms", s.id, s.costN, s.costSum / s.costN, s.costMax, s.beginMs, s.startMs);
}
static void PollReloadKey() {
	static bool wasDown = false;
	if (!g_reloadVk || !g_reloadFn) return;
	DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_reloadVk) & 0x8000) != 0;   // only while the game is the front window
	if (down && !wasDown) g_reloadFn();
	wasDown = down;
}


// The game's list of actors it simulates at high detail: ProcessLists (static, 0x11E0E80), its array of actor pointers at +8 with, per detail level,
// the end index at +0x24 (level 0 starts at 0) and the start index of the next level at +0x18.
static int ProcessActors(uint32_t* out, int cap) {
	const uint32_t X = 0x11E0E84; uint32_t arr = 0, c0 = 0, s1 = 0, c1 = 0;
	if (!Rd(X + 4, &arr, 4) || !arr || !Rd(X + 0x20, &c0, 4) || !Rd(X + 0x14, &s1, 4) || !Rd(X + 0x24, &c1, 4)) return 0;
	int n = 0;
	auto take = [&](uint32_t from, uint32_t to) { for (uint32_t i = from; i < to && i < 400 && n < cap; i++) { uint32_t a = 0; if (Rd(arr + i * 4, &a, 4) && a) out[n++] = a; } };
	if (c0 < 400) take(0, c0);
	if (c1 < 400 && s1 <= c1) take(s1, c1);
	return n;
}

// Living actors knocked down (explosion, power attack, push): the process keeps a knock state byte (+0x13C): 1 explode, 3 knocked out. Actors that go
// unconscious (life state 3) are caught when their life state changes. Every few frames the game's actor list is scanned.
static void KnockWatch() {
	if (!g_x.knockdown && !g_x.unconscious) return;
	static unsigned tick = 0; if (++tick % 6) return;
	static std::map<uint32_t, uint32_t> lastLs; static int said = 0;
	uint32_t list[256]; const int n = ProcessActors(list, 256);
	if (said < 3 && n > 0) { said++; Log("observe: scanning %d actors from the game's actor list", n); }
	if (lastLs.size() > 800) lastLs.clear();
	for (int k = 0; k < n; k++) {
		const uint32_t a = list[k]; uint32_t vt = 0, ls = 0xFFFF, pr = 0;
		if (!Rd(a, &vt, 4) || (vt != 0x1086A6C && vt != 0x10870AC) || !Rd(a + 0x108, &ls, 4)) continue;
		auto it = lastLs.find(a); const uint32_t prev = it == lastLs.end() ? 0xFFFE : it->second; lastLs[a] = ls;
		bool busy = false; for (Session* s : g_active) if (s->actor == a) { busy = true; break; }
		if (busy) continue;
		if (ls == 3) { if (g_x.unconscious && prev != 3 && prev != 0xFFFE) Begin(a, true, 3); continue; }     // went unconscious just now (not one found lying there)
		if (ls != 0 || !g_x.knockdown || !Rd(a + 0x68, &pr, 4) || pr < 0x10000) continue;
		uint8_t ks = 0; if (!Rd(pr + 0x13C, &ks, 1) || (ks != 1 && ks != 3)) continue;
		Begin(a, true, (int)ks);
	}
}

// The player: log what the game does when the player dies, and start Delirium from the life state when no kill burst was seen.
static void PlayerWatch() {
	static uint32_t lastLs = 0xFFFF; static DWORD deathTick = 0; static bool reported = false, forced = false;
	uint32_t pl = 0; if (!Rd(0x011DEA3C, &pl, 4) || !pl) return;
	uint32_t ls = 0xFFFF; if (!Rd(pl + 0x108, &ls, 4)) return;
	const DWORD now = GetTickCount();
	if (ls != lastLs) {
		uint8_t tp = 0; Rd(pl + 0x64A, &tp, 1);
		Log("player: life state %u -> %u (third-person flag %u, player damage hits so far %ld)", lastLs, ls, (unsigned)tp, (long)g_plDmg);
		if ((ls == 1 || ls == 2) && lastLs != 1 && lastLs != 2) { deathTick = now; reported = false; forced = false; }
		if (ls == 0) deathTick = 0;
		lastLs = ls;
	}
	if (!deathTick) return;
	const DWORD dt = now - deathTick;
	if (!forced && g_x.playerDeath && g_x.playerFallback && dt > 300) {
		forced = true;
		if (g_recent.find(pl) == g_recent.end()) { Log("player: no kill burst was seen, starting Delirium from the life state"); Begin(pl); }
	}
	if (!reported && dt > 1500) {
		reported = true;
		std::map<uint32_t, bool> mine; uint32_t root = ActorRoot(pl); int bodies = 0;
		if (root) for (auto& kv : CollectBones(root)) { uint32_t co = 0, vt = 0; if (Rd(kv.second + 0x1C, &co, 4) && co && Rd(co, &vt, 4) && vt == 0x10C53DC) { mine[co] = true; bodies++; } }
		int hit[8] = {}; int others = 0; LONG top = g_mtRingN;
		for (LONG i = top > 128 ? top - 128 : 0; i < top; i++) {
			if (now - (DWORD)g_mtTick[i & 127] > 5000) continue;
			if (mine.count((uint32_t)g_mtPtr[i & 127])) { LONG v = g_mtVal[i & 127]; if (v >= 0 && v < 8) hit[v]++; } else others++;
		}
		Log("player: %d s after dying: root %08X, %d of the player's skeleton nodes carry a collision body; motion-type calls on them in the last 5 s by new type 0..6: %d %d %d %d %d %d %d (other objects: %d); player damage hits %ld",
			(int)(dt / 1000), root, bodies, hit[0], hit[1], hit[2], hit[3], hit[4], hit[5], hit[6], others, (long)g_plDmg);
	}
}
void ObserveTick() {
	if (!g_enabled) return;
	const double t0 = NowMs();
	{ static double last = 0; if (last > 0) { const double d = t0 - last; if (d > 0.1 && d < 1000.0) { const float inst = (float)(1000.0 / d); g_fps = g_fps <= 0 ? inst : g_fps * 0.97f + inst * 0.03f; } } last = t0; }
	PollReloadKey();
	if (g_x.playerDeath) PlayerWatch();
	KnockWatch();
	while (g_qr != g_qw) { uint32_t a = (uint32_t)g_q[g_qr & 15]; g_qr++; Begin(a); }
	g_frameMs = 0;
	for (size_t i = 0; i < g_active.size();) {
		const double s0 = NowMs(); const bool keep = Step(*g_active[i]); const double c = NowMs() - s0;
		g_frameMs += c; g_active[i]->costSum += c; g_active[i]->costMax = std::max(g_active[i]->costMax, c); g_active[i]->costN++;
		if (keep) { i++; continue; }
		PerfLog(*g_active[i]); Free(g_active[i]); delete g_active[i]; g_active.erase(g_active.begin() + i);
	}
	if (g_x.frameBudgetMs > 0) {                                                                   // over budget: step slower, back up when there is room again
		if (g_active.empty()) g_lodScale = 1.0f;
		else if (g_frameMs > g_x.frameBudgetMs) g_lodScale = std::max(0.25f, g_lodScale * 0.95f);
		else g_lodScale = std::min(1.0f, g_lodScale * 1.02f);
	}
	if (g_x.agentPool > 0 && g_api && g_fit && (int)g_pool.size() < g_x.agentPool && GetTickCount() - (DWORD)g_lastDmgTick > 2500) {   // build agents in advance while nothing is going on
		static int made = 0; const float zero[3] = { 0, 0, 0 }; const double c0 = NowMs(); NmAgent* a = g_api->nm_agent_create(g_fit, zero, 0.0f, 0.3f);
		if (a) { g_pool.push_back(a); if (made++ < 3) Log("pool: built an agent in advance in %.1f ms", NowMs() - c0); }
	}
}

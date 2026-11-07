#include <cmath>
#include "observe.h"
#include "actor.h"
#include "rig.h"
#include "mathx.h"
#include "ingame.h"
#include "pick.h"
#include "ground.h"
#include "log.h"
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
static bool g_driveBodies = true; static float g_driveGain = 1.0f, g_driveMax = 12.0f;
static bool g_worldCollision, g_calibrate, g_groundAuto, g_groundReady = true, g_release = true; static unsigned g_calibTries; static float g_restSpeed = 1.5f; static unsigned g_restFrames = 30; static uint32_t g_groundFilter = 0x011F0025; static float g_groundCell = 0.35f; static int g_groundHalf = 10;
static float g_velScale = 1.0f, g_shotHealth = 1.0f; static bool g_keyframe = true, g_skipDismembered = true;
static int g_started;                                         // sessions begun this run (verbose logging for the first few only)

void ObserveSetDrive(bool on) { g_drive = on; }
void ObserveSetRequireDead(bool on) { g_requireDead = on; }
void ObserveSetDriveBodies(bool on, float gain, float maxSpeed) { g_driveBodies = on; g_driveGain = gain; g_driveMax = maxSpeed; }
void ObserveSetRelease(bool on, float restSpeed, unsigned restFrames) { g_release = on; g_restSpeed = restSpeed; g_restFrames = restFrames; }
void ObserveSetBodies(bool keyframeGameBodies, bool skipDismembered) { g_keyframe = keyframeGameBodies; g_skipDismembered = skipDismembered; }
void ObserveSetTuning(float velocityScale, float shotHealth) { g_velScale = std::max(0.0f, std::min(velocityScale, 4.0f)); g_shotHealth = std::max(0.1f, shotHealth); }
void ObserveSetWorld(bool collision, bool calibrate, uint32_t filter, float cell, int half) { g_worldCollision = collision; g_calibrate = calibrate; g_groundAuto = filter == 0; g_groundReady = filter != 0; g_groundFilter = filter ? filter : 0x011F0025; g_groundCell = std::max(0.15f, cell); g_groundHalf = std::max(2, std::min(half, 16)); }
void ObserveSetLimits(int maxActors, float simSeconds, float holdSeconds) { g_maxActors = std::max(1, std::min(maxActors, 8)); g_simSeconds = simSeconds; g_holdSeconds = holdSeconds; }
void ObserveInit(EngineApi* api, void* fit, const char* dir) {
	g_api = api; g_fit = (NmBuild*)fit; g_enabled = true;
	std::string rp = std::string(dir) + "\\fnv_character_rig.txt";
	if (RigLoad(g_rig, rp)) Log("observe: rig loaded (21 parts)"); else Log("observe: rig %s NOT loaded; nothing will be driven", rp.c_str());
}

// ---- death detection (runs inside the probe thunks: tiny and lock-free) ----
static volatile LONG g_worldCalls, g_oddVt, g_ours;          // g_ours: set while we make our own motion-type calls
static volatile LONG g_wake[32], g_ww;                      // collision objects the game switched back to a simulated motion type (a grab, an impulse)
static volatile LONG g_lastDmgActor, g_lastDmgTick;
static volatile LONG g_q[16], g_qw, g_qr; static volatile LONG g_lastQ, g_lastQTick;
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack) {
	if (!g_enabled) return;
	DWORD now = GetTickCount();
	if (role == kRoleWorld || role == kRolePick) { uint32_t vt = 0; InterlockedIncrement(&g_worldCalls); if (Rd(thisPtr, &vt, 4) && vt == 0x10C40B4) PickSetWorld(thisPtr); else g_oddVt = (LONG)vt; return; }   // any call on a bhkWorld tells us the physics world
	if (role == kRoleMotionType && g_ours) return;
	if (role == kRoleDamage) { g_lastDmgActor = (LONG)thisPtr; g_lastDmgTick = (LONG)now; }
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
	int phase = 0;                                              // 1 pose sampled, 2 simulating, 3 holding the final pose
	uint32_t actor = 0, root = 0, vt0 = 0, refId = 0; WorldXform rootW{}; bool verbose = false; unsigned id = 0;
	uint32_t bone[21]{}, boneA[21]{}, boneB[21]{};
	std::vector<uint32_t> order; std::map<uint32_t, int> rotPart, posPart;
	struct NodeId { uint32_t node, parent, name; }; std::vector<NodeId> ids; unsigned warm = 0;
	GroundGrid* ground = nullptr; bool groundFailed = false; unsigned groundHits = 0;
	V3 p0[21]{}; Q q0[21]{}; LARGE_INTEGER t0{}, last{};
	NmWorld* world = nullptr; NmAgent* agent = nullptr;
	unsigned frame = 0, calm = 0, hold = 0, redrives = 0, nWritten = 0, nPropagated = 0, lifeCheck = 0;
	float tr[21 * 7]{}, prev[21 * 3]{};
	std::map<uint32_t, V3> wrote;                               // game-world bone translation we last wrote
	std::vector<uint32_t> cos; LONG wakeSeen = 0;
	int mode = 0;                                               // 1 = the game's own bodies are driven by velocity; 0 = bones are written (old way)
	struct DBody { uint32_t node, body; }; std::vector<DBody> dbodies; float firstErr = -1;                // the game's collision objects we keyframed; wake calls already looked at
};
static std::vector<Session*> g_active; static std::map<uint32_t, DWORD> g_recent;

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

static unsigned g_worldFails; static bool g_calibDone;
static void EnsureWorld(const Session& s) {   // the physics world of this corpse's own cell, found from the actor (recomputed per kill: cells change)
	uint32_t co0 = 0;
	for (auto& kv : CollectBones(s.root)) { uint32_t co = 0, vt = 0; if (Rd(kv.second + 0x1C, &co, 4) && co && Rd(co, &vt, 4) && vt == 0x10C53DC) { co0 = co; break; } }
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

static int HiddenLimb(const Session& s) {                        // index of a rig part whose bone is scaled to (almost) nothing, or -1
	const float base = s.rootW.S != 0 ? s.rootW.S : 1.0f;
	for (int i = 0; i < 21; i++) { WorldXform w; if (NodeWorld(s.bone[i], w) && w.S < 0.2f * base) return i; }
	return -1;
}

static void Begin(uint32_t actor) {
	DWORD now = GetTickCount();
	for (Session* a : g_active) if (a->actor == actor) return;
	auto rc = g_recent.find(actor); if (rc != g_recent.end() && now - rc->second < 90000) return;         // dying then dead fire two bursts
	int sim = 0; for (Session* a : g_active) if (a->phase < 3) sim++;
	if (sim >= g_maxActors) return;                                                        // only bodies still being simulated count against the limit
	if (g_active.size() >= 24) for (size_t i = 0; i < g_active.size(); i++) if (g_active[i]->phase == 3) { Free(g_active[i]); delete g_active[i]; g_active.erase(g_active.begin() + i); break; }   // drop the oldest held pose
	g_recent[actor] = now;
	Session* s = new Session(); s->actor = actor; s->id = ++g_started; s->verbose = s->id <= 4;
	if (s->verbose) Log("observe[%u]: death burst, actor %08X", s->id, actor);
	s->root = ActorRoot(actor); Rd(actor, &s->vt0, 4); Rd(actor + 0xC, &s->refId, 4);   // identity, to notice the memory being reused by another actor
	if (!s->root || !NodeWorld(s->root, s->rootW)) { if (s->verbose) Log("observe[%u]: cannot read the actor's 3D", s->id); delete s; return; }
	if (!g_rig.ok || !g_api || !g_fit) { delete s; return; }
	auto bones = CollectBones(s->root);
	auto find = [&](const std::string& n) -> uint32_t { auto it = bones.find(n); return it == bones.end() ? 0 : it->second; };
	for (int i = 0; i < 21; i++) {
		s->bone[i] = find(g_rig.part[i].ref); s->boneA[i] = find(g_rig.part[i].jointA); s->boneB[i] = find(g_rig.part[i].jointB);
		if (!s->bone[i] || !s->boneA[i] || !s->boneB[i]) { Log("observe[%u]: bone '%s' not found on this actor (not a humanoid?), skipping", s->id, g_rig.part[i].ref.c_str()); delete s; return; }
	}
	if (g_skipDismembered) {                                                                // a cut-off limb is hidden by scaling its bone to nothing; only those actors are left to the game
		int hid = HiddenLimb(*s);
		if (hid >= 0) { Log("observe: actor %08X has a cut-off limb ('%s' scaled away), left to the game", actor, g_rig.part[hid].ref.c_str()); delete s; return; }
	}
	for (int i = 0; i < 21; i++) {
		if (!g_rig.part[i].write) continue;
		uint32_t n = s->bone[i]; s->rotPart[n] = i; int pp = i;
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
	if ((g_calibrate || (g_worldCollision && g_groundAuto)) && !g_calibDone && PickWorld()) { bool done = CalibrateRays(*s); if (done || ++g_calibTries >= 3) g_calibDone = true; }   // retried on later kills until it has run
	
	if (g_worldCollision && PickWorld() && g_groundReady) { s->ground = new GroundGrid(); GroundInit(*s->ground, 0, 0, g_groundCell, g_groundHalf, -1.0f, 0.9f); }
	LARGE_INTEGER c; QueryPerformanceCounter(&c); s->t0 = c; s->last = c; s->phase = 1;
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
	if (s.verbose) Log("observe[%u]: driving %u of the game's bodies by velocity", s.id, (unsigned)s.dbodies.size());
	return s.dbodies.size() >= 10;
}
static void DriveBodies(Session& s, const float* tr) {
	V3 P[21]; Q Qp[21];
	for (int i = 0; i < 21; i++) { P[i] = { tr[i * 7], tr[i * 7 + 1], tr[i * 7 + 2] }; Qp[i] = norm({ tr[i * 7 + 3], tr[i * 7 + 4], tr[i * 7 + 5], tr[i * 7 + 6] }); }
	const float tau = 0.04f, maxLin = g_driveMax / kUnit / 7.0f, maxAng = 25.0f;    // Havok units per second = game units per second / 7
	float errSum = 0; unsigned n = 0;
	for (auto& d : s.dbodies) {
		WorldXform tw, now; if (!TargetWorld(s, P, Qp, d.node, tw) || !NodeWorld(d.node, now)) continue;
		V3 e{ tw.T[0] - now.T[0], tw.T[1] - now.T[1], tw.T[2] - now.T[2] }; errSum += len(e); n++;
		V3 lin = e * (g_driveGain / tau / 7.0f); float ll = len(lin); if (ll > maxLin) lin = lin * (maxLin / ll);
		float Re[9]; for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) Re[r * 3 + c] = tw.R[r * 3] * now.R[c * 3] + tw.R[r * 3 + 1] * now.R[c * 3 + 1] + tw.R[r * 3 + 2] * now.R[c * 3 + 2];   // target * current^T
		normaliseCols(Re); Q q = norm(fromMat(Re)); if (q.w < 0) { q = { -q.x, -q.y, -q.z, -q.w }; }
		float sh = std::sqrt(std::max(0.0f, 1.0f - q.w * q.w)); float ang = 2.0f * std::atan2(sh, q.w);
		V3 axis = sh > 1e-4f ? V3{ q.x / sh, q.y / sh, q.z / sh } : V3{ 0, 0, 0 }; V3 w = axis * (ang * g_driveGain / tau); float wl = len(w); if (wl > maxAng) w = w * (maxAng / wl);
		const float lv[3] = { lin.x, lin.y, lin.z }, av[3] = { w.x, w.y, w.z }; GameSetBodyVelocity(d.body, lv, av);
	}
	s.nWritten = n; if (n) { float m = errSum / n; if (s.firstErr < 0) s.firstErr = m; s.prev[63] = m; }
}

static bool StartAgent(Session& s, const V3* p, const Q* q, LARGE_INTEGER c, float dt) {
	uint32_t ls = 0xFFFF; Rd(s.actor + 0x108, &ls, 4);                    // Actor::lifeState: 1 dying, 2 dead
	if (s.verbose) Log("observe[%u]: actor %08X life state %u one frame after the burst", s.id, s.actor, ls);
	if (g_requireDead && ls != 1 && ls != 2) { if (s.verbose) Log("observe[%u]: not dying or dead, leaving it to the game", s.id); return false; }
	float st[21 * 13], zp[21 * 7]; V3 vel[21];
	for (int i = 0; i < 21; i++) {
		vel[i] = (p[i] - s.p0[i]) * (g_velScale / dt);
		Q dq = mul(q[i], conj(s.q0[i])); if (dq.w < 0) dq = { -dq.x, -dq.y, -dq.z, -dq.w };
		float w = std::min(1.0f, dq.w), ang = 2 * std::acos(w), sn = std::sqrt(std::max(0.0f, 1 - w * w));
		V3 av = sn > 1e-5f ? V3{ dq.x / sn, dq.y / sn, dq.z / sn } * (ang * g_velScale / dt) : V3{ 0, 0, 0 };
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
	if (!meshed) g_api->nm_world_add_floor(s.world, 0, 0, 0, 100.0f);
	s.agent = g_api->nm_agent_create(g_fit, zero, 0.0f, 0.3f);
	if (!s.agent) return false;
	g_api->nm_agent_set_mode(s.agent, NM_MODE_LEAD);
	g_api->nm_agent_set_part_states(s.agent, st, 21); g_api->nm_agent_set_zero_pose(s.agent, zp, 21);
	V3 chest = p[10], cv = vel[10]; float hs = std::sqrt(cv.x * cv.x + cv.y * cv.y);
	float at[3] = { chest.x, chest.y, chest.z }, dir[3] = { hs > 0.05f ? cv.x / hs : 0.0f, hs > 0.05f ? cv.y / hs : 1.0f, 0.0f };
	g_api->nm_agent_task_shot(s.agent, s.world, nullptr, 10, at, dir, g_shotHealth);
	s.last = c; s.frame = 0; s.phase = 2;
	s.mode = (g_driveBodies && SetupDriveBodies(s)) ? 1 : 0;
	if (!s.mode) KeyframeGameBodies(s);
	if (s.verbose) Log("observe[%u]: Delirium body started (pelvis %.2f m/s, chest %.2f m/s)", s.id, len(vel[0]), len(vel[10]));
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
	if (++s.lifeCheck % 15 == 0 && g_requireDead) {                                          // revived, reloaded or replaced by a living actor: stop touching it
		uint32_t ls = 0xFFFF; Rd(s.actor + 0x108, &ls, 4);
		if (ls != 1 && ls != 2) { if (s.verbose) Log("observe[%u]: actor is no longer dying or dead (life state %u), session over", s.id, ls); return false; }
	}
	if (ActorRoot(s.actor) != s.root) { if (s.verbose) Log("observe[%u]: actor or its 3D is gone, session over", s.id); return false; }
	if (g_skipDismembered && (s.frame & 3) == 0) { int hid = HiddenLimb(s); if (hid >= 0) { Log("observe[%u]: a limb was cut off ('%s' scaled away), handing the corpse back to the game", s.id, g_rig.part[hid].ref.c_str()); return false; } }
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
		return StartAgent(s, p, q, c, GameDt(wall));
	}
	if (s.phase == 2) {
		LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
		float wall = (float)((double)(c.QuadPart - s.last.QuadPart) / (double)f.QuadPart); s.last = c;
		s.frame++; float wallMs = wall * 1000, dt = GameDt(wall);       // dt is game seconds: scaled by VATS and slow motion
		uint32_t worstN = 0; V3 worstD{ 0, 0, 0 }; float dev = (s.verbose && (s.frame == 1 || s.frame % 60 == 0)) ? Deviation(s, &worstN, &worstD) : 0;
		g_api->nm_agent_step(s.agent, s.world, dt);
		if (g_api->nm_agent_get_part_transforms(s.agent, s.tr, 21) < 0) return false;
		if (s.mode == 1) {
			DriveBodies(s, s.tr);
			if (s.frame == 60 && s.prev[63] > 140.0f) {                                              // the bodies are not following (still animation driven?): go back to writing bones
				Log("observe[%u]: the game's bodies did not follow (mean error %.0f units after 60 frames), falling back to writing bones", s.id, s.prev[63]);
				s.mode = 0; KeyframeGameBodies(s);
			}
		} else if (g_drive) Drive(s, s.tr);
		float fastest = 0;
		for (int i = 0; i < 21; i++) { V3 np{ s.tr[i * 7], s.tr[i * 7 + 1], s.tr[i * 7 + 2] }, op{ s.prev[i * 3], s.prev[i * 3 + 1], s.prev[i * 3 + 2] }; fastest = std::max(fastest, len(np - op) / dt); s.prev[i * 3] = np.x; s.prev[i * 3 + 1] = np.y; s.prev[i * 3 + 2] = np.z; }
		s.calm = (s.frame >= 90 && fastest < g_restSpeed) ? s.calm + 1 : 0;
		if (s.verbose && (s.frame == 1 || s.frame % 60 == 0))
		{ Log("observe[%u] f%03u dt=%.1fms (wall %.1f) pelvis z=%.2f head z=%.2f fastest part %.2f m/s | wrote %u bones + %u below | game moved them %.2f units", s.id, s.frame, dt * 1000, wallMs, s.tr[2], s.tr[12 * 7 + 2], fastest, s.nWritten, s.nPropagated, s.mode == 1 ? s.prev[63] : dev);
		  if (s.mode == 1) Log("observe[%u]   body drive: mean distance to the target %.1f units", s.id, s.prev[63]);
		  if (dev > 1.0f && s.mode == 0) { std::string nm; NodeName(worstN, nm); Log("observe[%u]   worst bone '%s' off by (%.1f %.1f %.1f)", s.id, nm.c_str(), worstD.x, worstD.y, worstD.z); } }
		if (s.calm >= g_restFrames || s.frame >= (unsigned)(g_simSeconds * 60)) {
			Free(&s);
			if (s.mode == 1) { Log("observe[%u]: at rest after %u frames, the game's bodies carry on from here", s.id, s.frame); return false; }
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

void ObserveReset() {                                          // save load, new game, exit to menu: every actor pointer we hold becomes invalid
	for (Session* s : g_active) { Free(s); delete s; }
	if (!g_active.empty()) Log("observe: game state changed, released %u sessions", (unsigned)g_active.size());
	g_active.clear(); g_recent.clear(); g_qr = g_qw; g_lastQ = 0; g_lastDmgActor = 0;
}

void ObserveTick() {
	if (!g_enabled) return;
	while (g_qr != g_qw) { uint32_t a = (uint32_t)g_q[g_qr & 15]; g_qr++; Begin(a); }
	for (size_t i = 0; i < g_active.size();) {
		if (Step(*g_active[i])) { i++; continue; }
		Free(g_active[i]); delete g_active[i]; g_active.erase(g_active.begin() + i);
	}
}

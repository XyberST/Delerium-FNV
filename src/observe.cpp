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
static bool g_worldCollision, g_calibrate; static uint32_t g_groundFilter = 0x011F0025; static float g_groundCell = 0.35f; static int g_groundHalf = 10;
static float g_velScale = 1.0f, g_shotHealth = 1.0f; static bool g_keyframe = true, g_skipDismembered = true;
static int g_started;                                         // sessions begun this run (verbose logging for the first few only)

void ObserveSetDrive(bool on) { g_drive = on; }
void ObserveSetRequireDead(bool on) { g_requireDead = on; }
void ObserveSetBodies(bool keyframeGameBodies, bool skipDismembered) { g_keyframe = keyframeGameBodies; g_skipDismembered = skipDismembered; }
void ObserveSetTuning(float velocityScale, float shotHealth) { g_velScale = std::max(0.0f, std::min(velocityScale, 4.0f)); g_shotHealth = std::max(0.1f, shotHealth); }
void ObserveSetWorld(bool collision, bool calibrate, uint32_t filter, float cell, int half) { g_worldCollision = collision; g_calibrate = calibrate; g_groundFilter = filter; g_groundCell = std::max(0.15f, cell); g_groundHalf = std::max(2, std::min(half, 16)); }
void ObserveSetLimits(int maxActors, float simSeconds, float holdSeconds) { g_maxActors = std::max(1, std::min(maxActors, 8)); g_simSeconds = simSeconds; g_holdSeconds = holdSeconds; }
void ObserveInit(EngineApi* api, void* fit, const char* dir) {
	g_api = api; g_fit = (NmBuild*)fit; g_enabled = true;
	std::string rp = std::string(dir) + "\\fnv_character_rig.txt";
	if (RigLoad(g_rig, rp)) Log("observe: rig loaded (21 parts)"); else Log("observe: rig %s NOT loaded; nothing will be driven", rp.c_str());
}

// ---- death detection (runs inside the probe thunks: tiny and lock-free) ----
static volatile LONG g_worldCalls, g_oddVt;
static volatile LONG g_wake[32], g_ww;                      // collision objects the game switched back to a simulated motion type (a grab, an impulse)
static volatile LONG g_lastDmgActor, g_lastDmgTick;
static volatile LONG g_q[16], g_qw, g_qr; static volatile LONG g_lastQ, g_lastQTick;
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack) {
	if (!g_enabled) return;
	DWORD now = GetTickCount();
	if (role == kRoleWorld || role == kRolePick) { uint32_t vt = 0; InterlockedIncrement(&g_worldCalls); if (Rd(thisPtr, &vt, 4) && vt == 0x10C40B4) PickSetWorld(thisPtr); else g_oddVt = (LONG)vt; return; }   // any call on a bhkWorld tells us the physics world
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
	std::vector<uint32_t> cos; LONG wakeSeen = 0;                // the game's collision objects we keyframed; wake calls already looked at
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

// One-off calibration on the first corpse: which ray layers see the floor, and which also see the actor itself?
static void CalibrateRays(const Session& s) {
	if (!PickWorld()) { Log("ray: no physics world known yet, calibration skipped"); return; }
	WorldXform pel; if (!NodeWorld(s.bone[0], pel)) return;
	const float* r = s.rootW.T; Log("ray: bhkWorld %08X, root (%.1f %.1f %.1f), pelvis (%.1f %.1f %.1f)", PickWorld(), r[0], r[1], r[2], pel.T[0], pel.T[1], pel.T[2]);
	for (uint32_t layer = 0; layer < 64; layer++) {
		uint32_t filter = 0x011F0000u | layer; bool h1 = false, h2 = false; float f1 = 1, f2 = 1, n1[3], n2[3];
		const float d0[3] = { r[0], r[1], r[2] + 60 }, d1[3] = { r[0], r[1], r[2] - 60 };                         // straight down through the feet
		const float b0[3] = { pel.T[0] + 80, pel.T[1], pel.T[2] }, b1[3] = { pel.T[0] - 80, pel.T[1], pel.T[2] };   // sideways through the pelvis
		bool ok1 = PickCast(d0, d1, filter, h1, f1, n1), ok2 = PickCast(b0, b1, filter, h2, f2, n2);
		if (!ok1 || !ok2) { Log("ray: layer %02X call not possible", layer); continue; }
		Log("ray: layer %02X  floor %s (z %.1f, normal z %.2f)  body-height %s (fraction %.2f)", layer, h1 ? "HIT " : "miss", r[2] + 60 - 120 * f1, n1[2], h2 ? "HIT " : "miss", f2);
	}
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

static void Begin(uint32_t actor) {
	DWORD now = GetTickCount();
	for (Session* a : g_active) if (a->actor == actor) return;
	auto rc = g_recent.find(actor); if (rc != g_recent.end() && now - rc->second < 90000) return;         // dying then dead fire two bursts
	int sim = 0; for (Session* a : g_active) if (a->phase < 3) sim++;
	if (sim >= g_maxActors) return;                                                        // only bodies still being simulated count against the limit
	if (g_active.size() >= 24) for (size_t i = 0; i < g_active.size(); i++) if (g_active[i]->phase == 3) { Free(g_active[i]); delete g_active[i]; g_active.erase(g_active.begin() + i); break; }   // drop the oldest held pose
	if (g_skipDismembered && ActorHasDismemberedLimbs(actor)) { if (g_started < 6) Log("observe: actor %08X has dismembered limbs, left to the game", actor); g_recent[actor] = now; return; }
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
	if (g_calibrate && !g_calibDone && PickWorld()) { g_calibDone = true; CalibrateRays(*s); }   // retried on every kill until the world is known
	
	if (g_worldCollision && PickWorld()) { s->ground = new GroundGrid(); GroundInit(*s->ground, 0, 0, g_groundCell, g_groundHalf, -1.0f, 0.9f); }
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
		if (GameSetMotionType(co, 4)) { n++; s.cos.push_back(co); } else bad++;
	}
	s.wakeSeen = g_ww;
	if (s.verbose) Log("observe[%u]: %d of the game's bodies switched to keyframed%s", s.id, n, bad ? " (some failed)" : "");
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
	KeyframeGameBodies(s);
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
	if (g_skipDismembered && ActorHasDismemberedLimbs(s.actor)) { if (s.verbose) Log("observe[%u]: a limb was dismembered, handing the corpse back to the game", s.id); return false; }
	if (s.frame > 5 && !s.cos.empty()) {                                                      // the game woke one of the bodies we keyframed (grabbing, an impulse): give the corpse back
		LONG w = g_ww; if (w - s.wakeSeen > 32) s.wakeSeen = w - 32;
		for (; s.wakeSeen < w; s.wakeSeen++) { uint32_t co = (uint32_t)g_wake[s.wakeSeen & 31]; for (uint32_t k : s.cos) if (k == co) { Log("observe[%u]: the game switched body %08X to a simulated motion type (grab or impulse), handing the corpse back", s.id, co); return false; } }
	}
	if (!NodesStillValid(s)) { if (s.verbose) Log("observe[%u]: a bone of this actor was replaced or freed (dismemberment or unload), session over", s.id); return false; }
	if (s.phase == 1) {                                                                      // let the game's own ragdoll run a few frames so its kill impulse shows up as velocity
		V3 p[21]; Q q[21]; LARGE_INTEGER c; QueryPerformanceCounter(&c);
		if (s.ground && !s.ground->done()) GroundWork(*s.ground, 120, GroundRayCb, &s);          // the grid is sampled over the warm-up frames to avoid a hitch
		if (!CapturePose(s, p, q)) return false;
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
		if (g_drive) Drive(s, s.tr);
		float fastest = 0;
		for (int i = 0; i < 21; i++) { V3 np{ s.tr[i * 7], s.tr[i * 7 + 1], s.tr[i * 7 + 2] }, op{ s.prev[i * 3], s.prev[i * 3 + 1], s.prev[i * 3 + 2] }; fastest = std::max(fastest, len(np - op) / dt); s.prev[i * 3] = np.x; s.prev[i * 3 + 1] = np.y; s.prev[i * 3 + 2] = np.z; }
		s.calm = fastest < 0.05f ? s.calm + 1 : 0;
		if (s.verbose && (s.frame == 1 || s.frame % 60 == 0))
		{ Log("observe[%u] f%03u dt=%.1fms (wall %.1f) pelvis z=%.2f head z=%.2f fastest part %.2f m/s | wrote %u bones + %u below | game moved them %.2f units", s.id, s.frame, dt * 1000, wallMs, s.tr[2], s.tr[12 * 7 + 2], fastest, s.nWritten, s.nPropagated, dev);
		  if (dev > 1.0f) { std::string nm; NodeName(worstN, nm); Log("observe[%u]   worst bone '%s' off by (%.1f %.1f %.1f)", s.id, nm.c_str(), worstD.x, worstD.y, worstD.z); } }
		if (s.calm >= 45 || s.frame >= (unsigned)(g_simSeconds * 60)) {
			Free(&s); s.phase = 3; s.hold = 0;
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

#include "observe.h"
#include "actor.h"
#include "rig.h"
#include "mathx.h"
#include "ingame.h"
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
static int g_started;                                         // sessions begun this run (verbose logging for the first few only)

void ObserveSetDrive(bool on) { g_drive = on; }
void ObserveSetRequireDead(bool on) { g_requireDead = on; }
void ObserveSetLimits(int maxActors, float simSeconds, float holdSeconds) { g_maxActors = std::max(1, std::min(maxActors, 8)); g_simSeconds = simSeconds; g_holdSeconds = holdSeconds; }
void ObserveInit(EngineApi* api, void* fit, const char* dir) {
	g_api = api; g_fit = (NmBuild*)fit; g_enabled = true;
	std::string rp = std::string(dir) + "\\fnv_character_rig.txt";
	if (RigLoad(g_rig, rp)) Log("observe: rig loaded (21 parts)"); else Log("observe: rig %s NOT loaded; nothing will be driven", rp.c_str());
}

// ---- phase 3a: record real calls of the game's Havok pick (ray cast) function, to learn its argument block ----
struct PickSample { uint32_t world, data; uint32_t w[44]; };
static PickSample g_pick[6]; static volatile LONG g_pickCount, g_pickCalls; static bool g_pickDumped;
static void PickDump() {
	g_pickDumped = true; Log("pick: %ld calls seen; first %ld samples of the 0xB0-byte argument block follow (bhkWorld::vtable[50], this = world, arg0 = block)", g_pickCalls, g_pickCount);
	for (int i = 0; i < g_pickCount && i < 6; i++) {
		const PickSample& s = g_pick[i]; Log("pick[%d] world=%08X block=%08X", i, s.world, s.data);
		for (int k = 0; k < 44; k += 4) {
			float f[4]; memcpy(f, &s.w[k], 16);
			Log("  +%02X: %08X %08X %08X %08X | %10.3f %10.3f %10.3f %10.3f", k * 4, s.w[k], s.w[k + 1], s.w[k + 2], s.w[k + 3], f[0], f[1], f[2], f[3]);
		}
	}
}

// ---- death detection (runs inside the probe thunks: tiny and lock-free) ----
static volatile LONG g_lastDmgActor, g_lastDmgTick;
static volatile LONG g_q[16], g_qw, g_qr; static volatile LONG g_lastQ, g_lastQTick;
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack) {
	if (!g_enabled) return;
	DWORD now = GetTickCount();
	if (role == kRolePick) {
		LONG n = InterlockedIncrement(&g_pickCalls);
		if (n % 40 == 1 && g_pickCount < 6) { PickSample& s = g_pick[g_pickCount]; s.world = thisPtr; s.data = stack[1]; if (Rd(stack[1], s.w, sizeof s.w)) InterlockedIncrement(&g_pickCount); }
		return;
	}
	if (role == kRoleDamage) { g_lastDmgActor = (LONG)thisPtr; g_lastDmgTick = (LONG)now; }
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
	V3 p0[21]{}; Q q0[21]{}; LARGE_INTEGER t0{}, last{};
	NmWorld* world = nullptr; NmAgent* agent = nullptr;
	unsigned frame = 0, calm = 0, hold = 0, redrives = 0, nWritten = 0, nPropagated = 0, lifeCheck = 0;
	float tr[21 * 7]{}, prev[21 * 3]{};
	std::map<uint32_t, V3> wrote;                               // game-world bone translation we last wrote
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
	if (s->agent) g_api->nm_agent_destroy(s->agent);
	if (s->world) g_api->nm_world_destroy(s->world);
	s->agent = nullptr; s->world = nullptr;
}
static int Depth(uint32_t n) { int d = 0; while (n && d < 64) { n = NodeParent(n); d++; } return d; }
static float Deviation(const Session& s) {                     // how far the game has moved the bones we wrote
	float dev = 0;
	for (auto& kv : s.wrote) { WorldXform w; if (!NodeWorld(kv.first, w)) continue; V3 d{ w.T[0] - kv.second.x, w.T[1] - kv.second.y, w.T[2] - kv.second.z }; dev = std::max(dev, len(d)); }
	return dev;
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
	for (int i = 0; i < 21; i++) {
		if (!g_rig.part[i].write) continue;
		uint32_t n = s->bone[i]; s->rotPart[n] = i; int pp = i;
		for (int j = 0; j < 21; j++) if (g_rig.part[j].ref == g_rig.part[i].ref && g_rig.part[j].jointA == g_rig.part[j].jointB) { pp = j; break; }
		s->posPart[n] = pp; s->order.push_back(n);
	}
	std::stable_sort(s->order.begin(), s->order.end(), [](uint32_t a, uint32_t b) { return Depth(a) < Depth(b); });
	V3 p[21]; Q q[21];
	if (!CapturePose(*s, p, q)) { if (s->verbose) Log("observe[%u]: bone read failed", s->id); delete s; return; }
	memcpy(s->p0, p, sizeof p); memcpy(s->q0, q, sizeof q);
	LARGE_INTEGER c; QueryPerformanceCounter(&c); s->t0 = c; s->last = c; s->phase = 1;
	g_active.push_back(s);
}

static bool StartAgent(Session& s) {
	uint32_t ls = 0xFFFF; Rd(s.actor + 0x108, &ls, 4);                    // Actor::lifeState: 1 dying, 2 dead
	if (s.verbose) Log("observe[%u]: actor %08X life state %u one frame after the burst", s.id, s.actor, ls);
	if (g_requireDead && ls != 1 && ls != 2) { if (s.verbose) Log("observe[%u]: not dying or dead, leaving it to the game", s.id); return false; }
	V3 p[21]; Q q[21]; if (!CapturePose(s, p, q)) return false;
	LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
	float dt = (float)((double)(c.QuadPart - s.t0.QuadPart) / (double)f.QuadPart); dt = std::min(std::max(dt, 1.0f / 200), 1.0f / 10);
	float st[21 * 13], zp[21 * 7]; V3 vel[21];
	for (int i = 0; i < 21; i++) {
		vel[i] = (p[i] - s.p0[i]) * (1.0f / dt);
		Q dq = mul(q[i], conj(s.q0[i])); if (dq.w < 0) dq = { -dq.x, -dq.y, -dq.z, -dq.w };
		float w = std::min(1.0f, dq.w), ang = 2 * std::acos(w), sn = std::sqrt(std::max(0.0f, 1 - w * w));
		V3 av = sn > 1e-5f ? V3{ dq.x / sn, dq.y / sn, dq.z / sn } * (ang / dt) : V3{ 0, 0, 0 };
		float* o = st + i * 13; o[0] = p[i].x; o[1] = p[i].y; o[2] = p[i].z; o[3] = q[i].x; o[4] = q[i].y; o[5] = q[i].z; o[6] = q[i].w;
		o[7] = vel[i].x; o[8] = vel[i].y; o[9] = vel[i].z; o[10] = av.x; o[11] = av.y; o[12] = av.z;
		float* t = zp + i * 7; t[0] = p[i].x; t[1] = p[i].y; t[2] = p[i].z; t[3] = q[i].x; t[4] = q[i].y; t[5] = q[i].z; t[6] = q[i].w;
		s.prev[i * 3] = p[i].x; s.prev[i * 3 + 1] = p[i].y; s.prev[i * 3 + 2] = p[i].z;
	}
	const float zero[3] = { 0, 0, 0 };
	s.world = g_api->nm_world_create(); g_api->nm_world_add_floor(s.world, 0, 0, 0, 100.0f);
	s.agent = g_api->nm_agent_create(g_fit, zero, 0.0f, 0.3f);
	if (!s.agent) return false;
	g_api->nm_agent_set_mode(s.agent, NM_MODE_LEAD);
	g_api->nm_agent_set_part_states(s.agent, st, 21); g_api->nm_agent_set_zero_pose(s.agent, zp, 21);
	V3 chest = p[10], cv = vel[10]; float hs = std::sqrt(cv.x * cv.x + cv.y * cv.y);
	float at[3] = { chest.x, chest.y, chest.z }, dir[3] = { hs > 0.05f ? cv.x / hs : 0.0f, hs > 0.05f ? cv.y / hs : 1.0f, 0.0f };
	g_api->nm_agent_task_shot(s.agent, s.world, nullptr, 10, at, dir, 1.0f);
	s.last = c; s.frame = 0; s.phase = 2;
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
		WorldXform now; if (!NodeWorld(n, now)) continue;
		V3 joint = P[pp] + rot(Qp[pp], g_rig.part[pp].anchor);
		float Rb[9]; toMat(norm(mul(Qp[rp], g_rig.part[rp].offset)), Rb);
		WorldXform w{}; mulMM(s.rootW.R, Rb, w.R);
		V3 t = mulMV(s.rootW.R, joint * (s.rootW.S / kUnit)); w.T[0] = s.rootW.T[0] + t.x; w.T[1] = s.rootW.T[1] + t.y; w.T[2] = s.rootW.T[2] + t.z; w.S = now.S;
		uint32_t par = NodeParent(n); WorldXform pw{};
		auto it = cur.find(par); if (it != cur.end()) pw = it->second; else if (!par || !NodeWorld(par, pw)) continue;
		WorldXform l{}; mulMTM(pw.R, w.R, l.R);
		V3 d{ w.T[0] - pw.T[0], w.T[1] - pw.T[1], w.T[2] - pw.T[2] }; float ips = pw.S != 0 ? 1.0f / pw.S : 1.0f;
		V3 lt = mulMTV(pw.R, d) * ips; l.T[0] = lt.x; l.T[1] = lt.y; l.T[2] = lt.z; l.S = pw.S != 0 ? w.S / pw.S : 1.0f;
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

// returns false when the session is finished
static bool Step(Session& s) {
	uint32_t vt = 0, rid = 0;
	if (!Rd(s.actor, &vt, 4) || vt != s.vt0 || !Rd(s.actor + 0xC, &rid, 4) || rid != s.refId) { if (s.verbose) Log("observe[%u]: the actor's memory now holds something else, session over", s.id); return false; }
	if (++s.lifeCheck % 15 == 0 && g_requireDead) {                                          // revived, reloaded or replaced by a living actor: stop touching it
		uint32_t ls = 0xFFFF; Rd(s.actor + 0x108, &ls, 4);
		if (ls != 1 && ls != 2) { if (s.verbose) Log("observe[%u]: actor is no longer dying or dead (life state %u), session over", s.id, ls); return false; }
	}
	if (ActorRoot(s.actor) != s.root) { if (s.verbose) Log("observe[%u]: actor or its 3D is gone, session over", s.id); return false; }
	if (s.phase == 1) { if (!StartAgent(s)) return false; return true; }
	if (s.phase == 2) {
		LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
		float dt = (float)((double)(c.QuadPart - s.last.QuadPart) / (double)f.QuadPart); s.last = c; dt = std::min(std::max(dt, 1.0f / 120), 1.0f / 20);
		s.frame++;
		float dev = (s.verbose && (s.frame == 1 || s.frame % 60 == 0)) ? Deviation(s) : 0;
		g_api->nm_agent_step(s.agent, s.world, dt);
		if (g_api->nm_agent_get_part_transforms(s.agent, s.tr, 21) < 0) return false;
		if (g_drive) Drive(s, s.tr);
		float fastest = 0;
		for (int i = 0; i < 21; i++) { V3 np{ s.tr[i * 7], s.tr[i * 7 + 1], s.tr[i * 7 + 2] }, op{ s.prev[i * 3], s.prev[i * 3 + 1], s.prev[i * 3 + 2] }; fastest = std::max(fastest, len(np - op) / dt); s.prev[i * 3] = np.x; s.prev[i * 3 + 1] = np.y; s.prev[i * 3 + 2] = np.z; }
		s.calm = fastest < 0.05f ? s.calm + 1 : 0;
		if (s.verbose && (s.frame == 1 || s.frame % 60 == 0))
			Log("observe[%u] f%03u dt=%.1fms pelvis z=%.2f head z=%.2f fastest part %.2f m/s | wrote %u bones + %u below | game moved them %.2f units", s.id, s.frame, dt * 1000, s.tr[2], s.tr[12 * 7 + 2], fastest, s.nWritten, s.nPropagated, dev);
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
	if (g_pickCount >= 6 && !g_pickDumped) PickDump();
	while (g_qr != g_qw) { uint32_t a = (uint32_t)g_q[g_qr & 15]; g_qr++; Begin(a); }
	for (size_t i = 0; i < g_active.size();) {
		if (Step(*g_active[i])) { i++; continue; }
		Free(g_active[i]); delete g_active[i]; g_active.erase(g_active.begin() + i);
	}
}

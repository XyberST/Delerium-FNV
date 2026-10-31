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
static EngineApi* g_api; static NmBuild* g_fit; static bool g_enabled, g_drive = true; static Rig g_rig;
static volatile LONG g_lastDmgActor, g_lastDmgTick, g_pending, g_pendingTick;
static int g_sessions; static DWORD g_lastSession;

void ObserveSetDrive(bool on) { g_drive = on; }
void ObserveInit(EngineApi* api, void* fit, const char* dir) {
	g_api = api; g_fit = (NmBuild*)fit; g_enabled = true;
	std::string rp = std::string(dir) + "\\fnv_character_rig.txt";
	if (RigLoad(g_rig, rp)) Log("observe: rig loaded (21 parts)"); else Log("observe: rig %s NOT loaded; the drive step is disabled", rp.c_str());
}
// Called from a probe thunk; must stay tiny and lock-free.
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack) {
	if (!g_enabled) return;
	DWORD now = GetTickCount();
	if (role == kRoleDamage) { g_lastDmgActor = (LONG)thisPtr; g_lastDmgTick = (LONG)now; }
	else if (role == kRoleMotionType && stack[1] == 1 /* motion type: dynamic */ && now - (DWORD)g_lastDmgTick <= 100 && !g_pending) {
		g_pending = g_lastDmgActor; g_pendingTick = (LONG)now;
	}
}

// ---- one session: sample the NPC's pose twice, start a Delirium body from it, then drive the bones from the body ----
struct Session {
	int phase = 0;                                              // 0 idle, 1 first pose sampled, 2 running
	uint32_t actor = 0, root = 0; WorldXform rootW{};
	uint32_t bone[21]{}, boneA[21]{}, boneB[21]{};
	std::vector<uint32_t> order; std::map<uint32_t, int> rotPart, posPart;   // written bones, top-down; which part gives rotation / position
	V3 p0[21]{}; Q q0[21]{}; LARGE_INTEGER t0{}, last{};
	NmWorld* world = nullptr; NmAgent* agent = nullptr; unsigned frame = 0;
	std::map<uint32_t, V3> wrote;                               // game-world translation we wrote last tick, to detect the game overwriting it
	unsigned nWritten = 0, nPropagated = 0;
};
static Session S;

static bool BoneRoot(uint32_t node, V3& pos, Q& rq) {          // metres and rotation in the actor's root frame
	WorldXform w; if (!NodeWorld(node, w)) return false;
	V3 d{ w.T[0] - S.rootW.T[0], w.T[1] - S.rootW.T[1], w.T[2] - S.rootW.T[2] };
	pos = mulMTV(S.rootW.R, d) * ((S.rootW.S != 0 ? 1.0f / S.rootW.S : 1.0f) * kUnit);
	float RR[9]; mulMTM(S.rootW.R, w.R, RR); normaliseCols(RR); rq = fromMat(RR); return true;
}
// part pose from the bones: partRot = boneRot * conj(offset); partPos = joint - partRot * anchor
static bool CapturePose(V3* pos, Q* rq) {
	for (int i = 0; i < 21; i++) {
		V3 pr, pa, pb; Q br, qa, qb;
		if (!BoneRoot(S.bone[i], pr, br) || !BoneRoot(S.boneA[i], pa, qa) || !BoneRoot(S.boneB[i], pb, qb)) return false;
		rq[i] = norm(mul(br, conj(g_rig.part[i].offset)));
		pos[i] = (pa + pb) * 0.5f - rot(rq[i], g_rig.part[i].anchor);
	}
	return true;
}
static void End() {
	if (S.agent) g_api->nm_agent_destroy(S.agent);
	if (S.world) g_api->nm_world_destroy(S.world);
	S = Session();
}
static int Depth(uint32_t n) { int d = 0; while (n && d < 64) { n = NodeParent(n); d++; } return d; }

static void Begin(uint32_t actor) {
	DWORD now = GetTickCount();
	if (g_sessions >= 8 || now - g_lastSession < 4000) return;
	g_lastSession = now; g_sessions++;
	Log("observe: death burst, actor %08X (session %d)", actor, g_sessions);
	S = Session(); S.actor = actor; S.root = ActorRoot(actor);
	if (!S.root) { Log("observe: could not read the actor's 3D root"); return; }
	if (!NodeWorld(S.root, S.rootW)) { Log("observe: root %08X unreadable", S.root); return; }
	auto bones = CollectBones(S.root);
	Log("observe: root %08X, %u named nodes", S.root, (unsigned)bones.size());
	if (!g_rig.ok || !g_api || !g_fit) { Log("observe: rig or engine body missing, nothing to drive"); return; }
	auto find = [&](const std::string& n) -> uint32_t { auto it = bones.find(n); return it == bones.end() ? 0 : it->second; };
	for (int i = 0; i < 21; i++) {
		S.bone[i] = find(g_rig.part[i].ref); S.boneA[i] = find(g_rig.part[i].jointA); S.boneB[i] = find(g_rig.part[i].jointB);
		if (!S.bone[i] || !S.boneA[i] || !S.boneB[i]) { Log("observe: part %d (%s): bone '%s' not found on this actor, skipping it", i, g_rig.part[i].name.c_str(), g_rig.part[i].ref.c_str()); return; }
	}
	for (int i = 0; i < 21; i++) {                                // which bone each writing part drives, and where its position comes from
		if (!g_rig.part[i].write) continue;
		uint32_t n = S.bone[i]; S.rotPart[n] = i; int pp = i;
		for (int j = 0; j < 21; j++) if (g_rig.part[j].ref == g_rig.part[i].ref && g_rig.part[j].jointA == g_rig.part[j].jointB) { pp = j; break; }
		S.posPart[n] = pp; S.order.push_back(n);
	}
	std::stable_sort(S.order.begin(), S.order.end(), [](uint32_t a, uint32_t b) { return Depth(a) < Depth(b); });
	V3 p[21]; Q q[21];
	if (!CapturePose(p, q)) { Log("observe: bone read failed at the first sample"); return; }
	memcpy(S.p0, p, sizeof p); memcpy(S.q0, q, sizeof q);
	LARGE_INTEGER c; QueryPerformanceCounter(&c); S.t0 = c; S.last = c; S.phase = 1;
	Log("observe: first pose sampled, pelvis part at (%.3f %.3f %.3f) m", p[0].x, p[0].y, p[0].z);
}

static void StartAgent() {
	V3 p[21]; Q q[21];
	if (!CapturePose(p, q)) { Log("observe: bone read failed at the second sample"); End(); return; }
	LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
	float dt = (float)((double)(c.QuadPart - S.t0.QuadPart) / (double)f.QuadPart); dt = std::min(std::max(dt, 1.0f / 200), 1.0f / 10);
	float st[21 * 13], zp[21 * 7]; V3 vel[21];
	for (int i = 0; i < 21; i++) {
		vel[i] = (p[i] - S.p0[i]) * (1.0f / dt);
		Q dq = mul(q[i], conj(S.q0[i])); if (dq.w < 0) dq = { -dq.x, -dq.y, -dq.z, -dq.w };
		float w = std::min(1.0f, dq.w), ang = 2 * std::acos(w), s = std::sqrt(std::max(0.0f, 1 - w * w));
		V3 av = s > 1e-5f ? V3{ dq.x / s, dq.y / s, dq.z / s } * (ang / dt) : V3{ 0, 0, 0 };
		float* o = st + i * 13; o[0] = p[i].x; o[1] = p[i].y; o[2] = p[i].z; o[3] = q[i].x; o[4] = q[i].y; o[5] = q[i].z; o[6] = q[i].w;
		o[7] = vel[i].x; o[8] = vel[i].y; o[9] = vel[i].z; o[10] = av.x; o[11] = av.y; o[12] = av.z;
		float* t = zp + i * 7; t[0] = p[i].x; t[1] = p[i].y; t[2] = p[i].z; t[3] = q[i].x; t[4] = q[i].y; t[5] = q[i].z; t[6] = q[i].w;
	}
	const float zero[3] = { 0, 0, 0 };
	S.world = g_api->nm_world_create(); g_api->nm_world_add_floor(S.world, 0, 0, 0, 100.0f);
	S.agent = g_api->nm_agent_create(g_fit, zero, 0.0f, 0.3f);
	if (!S.agent) { Log("observe: agent create failed"); End(); return; }
	g_api->nm_agent_set_mode(S.agent, NM_MODE_LEAD);
	int r1 = g_api->nm_agent_set_part_states(S.agent, st, 21), r2 = g_api->nm_agent_set_zero_pose(S.agent, zp, 21);
	V3 chest = p[10], cv = vel[10]; float hs = std::sqrt(cv.x * cv.x + cv.y * cv.y);
	float at[3] = { chest.x, chest.y, chest.z }, dir[3] = { hs > 0.05f ? cv.x / hs : 0.0f, hs > 0.05f ? cv.y / hs : 1.0f, 0.0f };
	int r3 = g_api->nm_agent_task_shot(S.agent, S.world, nullptr, 10, at, dir, 1.0f);
	Log("observe: Delirium body started from the NPC's pose (set states %d, zero pose %d, shot %d); pelvis speed %.2f m/s, chest speed %.2f m/s", r1, r2, r3, len(vel[0]), len(vel[10]));
	S.last = c; S.frame = 0; S.phase = 2;
	IngameNotify("DeliriumFNV: took over a death%s", g_drive ? "" : " (watching only)");
}

// Write the Delirium pose into the bones: world transform, local transform, then the world transforms of everything below.
static void Drive(const float* tr) {
	V3 P[21]; Q Qp[21];
	for (int i = 0; i < 21; i++) { P[i] = { tr[i * 7], tr[i * 7 + 1], tr[i * 7 + 2] }; Qp[i] = norm({ tr[i * 7 + 3], tr[i * 7 + 4], tr[i * 7 + 5], tr[i * 7 + 6] }); }
	std::map<uint32_t, WorldXform> cur; unsigned ok = 0;
	for (uint32_t n : S.order) {
		int rp = S.rotPart[n], pp = S.posPart[n];
		WorldXform now; if (!NodeWorld(n, now)) continue;
		V3 joint = P[pp] + rot(Qp[pp], g_rig.part[pp].anchor);                          // bone origin, root frame, metres
		float Rb[9]; toMat(norm(mul(Qp[rp], g_rig.part[rp].offset)), Rb);
		WorldXform w{}; mulMM(S.rootW.R, Rb, w.R);
		V3 t = mulMV(S.rootW.R, joint * (S.rootW.S / kUnit)); w.T[0] = S.rootW.T[0] + t.x; w.T[1] = S.rootW.T[1] + t.y; w.T[2] = S.rootW.T[2] + t.z; w.S = now.S;
		uint32_t par = NodeParent(n); WorldXform pw{};
		auto it = cur.find(par); if (it != cur.end()) pw = it->second; else if (!par || !NodeWorld(par, pw)) continue;
		WorldXform l{}; mulMTM(pw.R, w.R, l.R);
		V3 d{ w.T[0] - pw.T[0], w.T[1] - pw.T[1], w.T[2] - pw.T[2] }; float ips = pw.S != 0 ? 1.0f / pw.S : 1.0f;
		V3 lt = mulMTV(pw.R, d) * ips; l.T[0] = lt.x; l.T[1] = lt.y; l.T[2] = lt.z; l.S = pw.S != 0 ? w.S / pw.S : 1.0f;
		if (NodeWriteWorld(n, w) && NodeWriteLocal(n, l)) { cur[n] = w; S.wrote[n] = { w.T[0], w.T[1], w.T[2] }; ok++; }
	}
	S.nWritten = ok;
	std::deque<uint32_t> q; for (auto& kv : cur) q.push_back(kv.first); unsigned prop = 0;
	std::map<uint32_t, bool> done; for (auto& kv : cur) done[kv.first] = true;
	while (!q.empty() && prop < 900) {                                                    // children not driven directly follow their parent
		uint32_t n = q.front(); q.pop_front(); const WorldXform pw = cur[n];
		std::vector<uint32_t> kids; if (!NodeIsPlainNode(n) || !NodeChildren(n, kids)) continue;
		for (uint32_t c : kids) {
			if (done[c] || !NodeIsPlainNode(c)) continue;
			done[c] = true;
			WorldXform l; if (!NodeLocal(c, l)) continue;
			WorldXform w{}; mulMM(pw.R, l.R, w.R); w.S = pw.S * l.S;
			V3 t = mulMV(pw.R, V3{ l.T[0], l.T[1], l.T[2] } * pw.S); w.T[0] = pw.T[0] + t.x; w.T[1] = pw.T[1] + t.y; w.T[2] = pw.T[2] + t.z;
			if (NodeWriteWorld(c, w)) { cur[c] = w; q.push_back(c); prop++; }
		}
	}
	S.nPropagated = prop;
}

void ObserveTick() {
	if (!g_enabled) return;
	if (S.phase == 0) { if (g_pending) { uint32_t a = (uint32_t)g_pending; g_pending = 0; Begin(a); } return; }
	if (ActorRoot(S.actor) != S.root) { Log("observe: the actor or its 3D is gone, ending the session"); End(); return; }
	if (S.phase == 1) { StartAgent(); return; }
	LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
	float dt = (float)((double)(c.QuadPart - S.last.QuadPart) / (double)f.QuadPart); S.last = c; dt = std::min(std::max(dt, 1.0f / 120), 1.0f / 20);
	S.frame++;
	float dev = 0; int cnt = 0;                                                            // did the game overwrite what we wrote last tick?
	for (auto& kv : S.wrote) { WorldXform w; if (!NodeWorld(kv.first, w)) continue; V3 d{ w.T[0] - kv.second.x, w.T[1] - kv.second.y, w.T[2] - kv.second.z }; dev = std::max(dev, len(d)); cnt++; }
	g_api->nm_agent_step(S.agent, S.world, dt);
	float tr[21 * 7] = {}; bool have = g_api->nm_agent_get_part_transforms(S.agent, tr, 21) >= 0;
	if (have && g_drive) Drive(tr);
	if (S.frame == 1 || S.frame % 30 == 0)
		Log("observe f%03u dt=%.1fms | delirium pelvis z=%.2f head z=%.2f | wrote %u bones + %u below | game moved them since our last write by up to %.2f units (%d checked)%s",
			S.frame, dt * 1000, tr[2], tr[12 * 7 + 2], S.nWritten, S.nPropagated, dev, cnt, g_drive ? "" : " [drive off]");
	if (S.frame >= 900) { Log("observe: session over"); End(); }
}

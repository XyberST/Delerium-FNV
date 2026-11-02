// Offline test: the ground mesh against the real engine (Linux x86-64 build of the engine). Not part of the plugin.
// g++ -std=c++17 -O1 -I../src ground_test.cpp ../src/ground.cpp -ldl -o ground_test && ./ground_test <libdelirium_physics.so> <data dir>
#include "engine.h"
#include "ground.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
static EngineApi A;
struct Scene { float bx0 = 0.5f, bx1 = 2.0f, by0 = -0.6f, by1 = 0.6f, top = 0.55f; };
static bool Ray(void* c, float x, float y, float& z) { auto* s = (Scene*)c; z = (x >= s->bx0 && x <= s->bx1 && y >= s->by0 && y <= s->by1) ? s->top : 0.0f; return true; }
static NmBuild* Fit(const char* dir) {
	std::string d = dir, js; { std::ifstream f(d + "/skyrim_character.json"); std::stringstream ss; ss << f.rdbuf(); js = ss.str(); }
	if (A.nm_init_character_file((d + "/skyrim_character.json").c_str()) < 0) return nullptr;
	NmBuild* base = A.nm_build_from_character(js.c_str()); std::vector<float> j; { std::ifstream f(d + "/fnv_joints.txt"); float v; while (f >> v) j.push_back(v); }
	std::vector<float> r(84); for (int i = 0; i < 21; i++) { float p[3], q[4]; A.nm_build_part_frame(base, i, p, q); memcpy(&r[i * 4], q, 16); }
	return A.nm_build_remap(base, j.data(), r.data(), 0x1FFFFF);
}
// start the standing body at (x,y,z0) with a velocity, let it fall, report where the pelvis and head rest
static void Run(NmBuild* fit, NmWorld* w, float x, float y, float z0, float vx, const char* label) {
	const float zero[3] = { 0, 0, 0 }; NmAgent* a = A.nm_agent_create(fit, zero, 0.0f, 0.3f);
	std::vector<float> st(21 * 13); float tr0[21 * 7]; A.nm_agent_get_part_transforms(a, tr0, 21);
	for (int i = 0; i < 21; i++) { float* o = &st[i * 13]; memcpy(o, tr0 + i * 7, 28); o[0] += x; o[1] += y; o[2] += z0; o[7] = vx; o[8] = 0; o[9] = 0; o[10] = o[11] = o[12] = 0; }
	A.nm_agent_set_mode(a, 0); A.nm_agent_set_part_states(a, st.data(), 21);
	const float at[3] = { x, y, z0 + 1.3f }, d[3] = { 1, 0, 0 }; A.nm_agent_task_shot(a, w, nullptr, 10, at, d, 1.0f);
	for (int i = 0; i < 480; i++) A.nm_agent_step(a, w, 1.0f / 60);
	float tr[21 * 7]; A.nm_agent_get_part_transforms(a, tr, 21);
	printf("%-34s pelvis (%.2f %.2f %.2f)  head z %.2f  lowest part z %.2f\n", label, tr[0], tr[1], tr[2], tr[12 * 7 + 2], std::min(tr[2], std::min(tr[3 * 7 + 2], tr[6 * 7 + 2])));
	A.nm_agent_destroy(a);
}
int main(int argc, char** argv) {
	void* h = dlopen(argv[1], RTLD_NOW); if (!h) { puts("dlopen failed"); return 1; }
#define NM_FN(ret, name, args) *(void**)&A.name = dlsym(h, #name);
#include "engine_api.inc"
#undef NM_FN
	NmBuild* fit = Fit(argv[2]); if (!fit) { puts("fit failed"); return 1; }
	Scene sc; GroundGrid g; GroundInit(g, 1.0f, 0.0f, 0.3f, 10, -1.0f, 0.9f);
	while (!g.done()) GroundWork(g, 100, Ray, &sc);
	std::vector<float> pos; std::vector<uint32_t> tri; float low = GroundMesh(g, 0.0f, pos, tri);
	printf("grid %dx%d, %zu triangles, lowest %.2f\n", g.side(), g.side(), tri.size() / 3, low);
	NmWorld* flat = A.nm_world_create(); A.nm_world_add_floor(flat, 0, 0, 0, 100.0f);
	NmWorld* mesh = A.nm_world_create(); A.nm_world_add_mesh(mesh, pos.data(), (int)pos.size() / 3, tri.data(), (int)tri.size() / 3, nullptr); A.nm_world_add_floor(mesh, 0, 0, low - 0.02f, 100.0f);
	Run(fit, flat, -0.3f, 0, 0.0f, 0.0f, "flat floor, standing");
	Run(fit, flat, -0.3f, 0, 0.0f, 2.5f, "flat floor, thrown +x");
	{ Scene none; none.top = 0.0f; GroundGrid g2; GroundInit(g2, 1.0f, 0.0f, 0.3f, 10, -1.0f, 0.9f); while (!g2.done()) GroundWork(g2, 100, Ray, &none);
	  std::vector<float> p2; std::vector<uint32_t> t2; float lo2 = GroundMesh(g2, 0.0f, p2, t2); NmWorld* m2 = A.nm_world_create();
	  A.nm_world_add_mesh(m2, p2.data(), (int)p2.size() / 3, t2.data(), (int)t2.size() / 3, nullptr); A.nm_world_add_floor(m2, 0, 0, lo2 - 0.02f, 100.0f);
	  Run(fit, m2, -0.3f, 0, 0.0f, 0.0f, "flat MESH (no bed), standing"); Run(fit, m2, -0.3f, 0, 0.0f, 2.5f, "flat MESH (no bed), thrown +x"); }
	Run(fit, flat, 1.2f, 0, 0.55f, 0.0f, "flat floor, started on the bed");
	Run(fit, mesh, 1.2f, 0, 0.55f, 0.0f, "ground mesh, started on the bed");
	Run(fit, mesh, 1.2f, 0, 0.55f, 1.5f, "ground mesh, pushed along the bed");
	Run(fit, mesh, -0.3f, 0, 0.0f, 0.0f, "ground mesh, standing on the floor");
	Run(fit, mesh, -0.3f, 0, 0.0f, 2.5f, "ground mesh, thrown towards the bed");
	return 0;
}

#include "actor.h"
#include <windows.h>
#include <deque>
#include <cstring>

bool Rd(uint32_t addr, void* out, size_t n) {
	if (addr < 0x10000) return false;
	SIZE_T got = 0; return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)addr, out, n, &got) && got == n;
}
static bool Rd32(uint32_t a, uint32_t& v) { return Rd(a, &v, 4); }
uint32_t ActorRoot(uint32_t actor) {
	uint32_t rs = 0, root = 0;
	if (!Rd32(actor + 0x64, rs) || !rs) return 0;
	if (!Rd32(rs + 0x14, root)) return 0;
	return root;
}
bool NodeName(uint32_t node, std::string& out) {
	uint32_t p = 0; if (!Rd32(node + 0x08, p) || !p) return false;
	char buf[64] = {}; if (!Rd(p, buf, 63)) { for (int i = 0; i < 63; i++) if (!Rd(p + i, &buf[i], 1)) { buf[i] = 0; break; } }
	buf[63] = 0; out = buf; return !out.empty();
}
bool NodeWorld(uint32_t node, WorldXform& w) {
	return Rd(node + 0x68, w.R, 36) && Rd(node + 0x8C, w.T, 12) && Rd(node + 0x98, &w.S, 4);
}
std::map<std::string, uint32_t> CollectBones(uint32_t root) {
	std::map<std::string, uint32_t> out; std::deque<uint32_t> q; q.push_back(root); size_t seen = 0;
	const uint32_t kNiNodeVtbl = 0x109B5AC, kFaceGenVtbl = 0x10660DC;
	while (!q.empty() && seen < 600) {
		uint32_t n = q.front(); q.pop_front(); seen++;
		std::string name; if (NodeName(n, name)) out.emplace(name, n);
		uint32_t vt = 0; if (!Rd32(n, vt)) continue;
		if (n != root && vt != kNiNodeVtbl && vt != kFaceGenVtbl) continue;        // geometry and other leaf types have no child array
		uint32_t data = 0; uint16_t end = 0; if (!Rd32(n + 0xA0, data) || !Rd(n + 0xA6, &end, 2) || !data || end > 512) continue;
		for (uint32_t i = 0; i < end; i++) { uint32_t c = 0; if (Rd32(data + 4 * i, c) && c) q.push_back(c); }
	}
	return out;
}

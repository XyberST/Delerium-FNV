#include "ground.h"
#include <algorithm>
#include <cmath>

void GroundInit(GroundGrid& g, float cx, float cy, float cell, int half, float zMin, float zMax) {
	g.cx = cx; g.cy = cy; g.cell = cell; g.half = std::max(1, std::min(half, 24)); g.zMin = zMin; g.zMax = zMax;
	g.h.assign(g.total(), 0.0f); g.ok.assign(g.total(), 0); g.next = 0;
}
void GroundWork(GroundGrid& g, int maxSamples, GroundRay ray, void* ctx) {
	const int n = g.side();
	for (int k = 0; k < maxSamples && g.next < g.total(); k++, g.next++) {
		int i = g.next % n, j = g.next / n;
		float x = g.cx + (i - g.half) * g.cell, y = g.cy + (j - g.half) * g.cell, z = 0;
		if (ray(ctx, x, y, z) && z >= g.zMin && z <= g.zMax) { g.h[g.next] = z; g.ok[g.next] = 1; }
	}
}
float GroundMesh(const GroundGrid& g, float fallbackZ, std::vector<float>& pos, std::vector<uint32_t>& tri) {
	const int n = g.side(); std::vector<float> h = g.h; std::vector<uint8_t> ok = g.ok;
	for (int pass = 0; pass < 6; pass++) {                                     // grow valid samples into holes, average of the valid neighbours
		std::vector<float> nh = h; std::vector<uint8_t> nok = ok; bool any = false;
		for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
			if (ok[j * n + i]) continue;
			float s = 0; int c = 0;
			for (int dj = -1; dj <= 1; dj++) for (int di = -1; di <= 1; di++) {
				int ii = i + di, jj = j + dj; if (ii < 0 || jj < 0 || ii >= n || jj >= n || (!di && !dj)) continue;
				if (ok[jj * n + ii]) { s += h[jj * n + ii]; c++; }
			}
			if (c) { nh[j * n + i] = s / c; nok[j * n + i] = 1; any = true; }
		}
		h.swap(nh); ok.swap(nok); if (!any) break;
	}
	float low = 1e9f; pos.clear(); tri.clear(); pos.reserve(n * n * 3);
	for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
		float z = ok[j * n + i] ? h[j * n + i] : fallbackZ; low = std::min(low, z);
		pos.push_back(g.cx + (i - g.half) * g.cell); pos.push_back(g.cy + (j - g.half) * g.cell); pos.push_back(z);
	}
	for (int j = 0; j + 1 < n; j++) for (int i = 0; i + 1 < n; i++) {         // counter-clockwise seen from above: the normal points up
		uint32_t a = j * n + i, b = a + 1, c = a + n, d = c + 1;
		tri.push_back(a); tri.push_back(b); tri.push_back(d);
		tri.push_back(a); tri.push_back(d); tri.push_back(c);
	}
	return low;
}

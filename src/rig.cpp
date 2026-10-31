#include "rig.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>
bool RigLoad(Rig& rig, const std::string& path) {
	std::ifstream in(path); if (!in) return false; std::string line; int n = 0;
	while (std::getline(in, line)) {
		if (line.empty() || line.rfind("yaw", 0) == 0) continue;
		if (!line.empty() && line.back() == '\r') line.pop_back();
		std::vector<std::string> f; std::stringstream ss(line); std::string t; while (std::getline(ss, t, '|')) f.push_back(t);
		if (f.size() < 8 || n >= 21) return false;
		RigPart& p = rig.part[n++]; p.name = f[0]; p.ref = f[1]; p.write = f[2] == "1"; p.jointA = f[3]; p.jointB = f[4];
		if (sscanf(f[5].c_str(), "%f %f %f %f", &p.offset.x, &p.offset.y, &p.offset.z, &p.offset.w) != 4) return false;
		if (sscanf(f[6].c_str(), "%f %f %f", &p.anchor.x, &p.anchor.y, &p.anchor.z) != 3) return false;
		if (sscanf(f[7].c_str(), "%f %f %f", &p.inParent.x, &p.inParent.y, &p.inParent.z) != 3) return false;
	}
	rig.ok = n == 21; return rig.ok;
}

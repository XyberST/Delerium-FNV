// The FNV rig (data/fnv_character_rig.txt): per part, the game bone it maps to, where the part's joint is, and the
// offset between the bone's rotation and the part's:  boneRot = partRot * offset,  joint = partPos + partRot * anchor.
#pragma once
#include "mathx.h"
#include <string>
struct RigPart { std::string name, ref, jointA, jointB; bool write = false; Q offset{ 0, 0, 0, 1 }; V3 anchor{}, inParent{}; };
struct Rig { RigPart part[21]; bool ok = false; };
bool RigLoad(Rig& rig, const std::string& path);

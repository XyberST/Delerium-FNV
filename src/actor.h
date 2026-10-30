// Read-only access to a game actor's skeleton. Every read goes through ReadProcessMemory, so a wrong offset or a stale
// pointer produces a failed read and a log line instead of a crash.
// Layout (FalloutNV.exe 1.4.0.525; transform offsets verified against NiAVObject's stream loader at 0xA5A510):
//   TESObjectREFR+0x64 -> renderState, renderState+0x14 -> root NiNode
//   NiObjectNET: name char* @0x08 | NiAVObject: parent @0x18, local R@0x34 T@0x58 S@0x64, world R@0x68 T@0x8C S@0x98
//   NiNode: children NiTArray at 0x9C (data @0xA0, end index u16 @0xA6, count u16 @0xA8)
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
bool Rd(uint32_t addr, void* out, size_t n);
struct WorldXform { float R[9]; float T[3]; float S; };
uint32_t ActorRoot(uint32_t actor);                                   // 0 on failure
bool NodeName(uint32_t node, std::string& out);
bool NodeWorld(uint32_t node, WorldXform& w);
// Breadth-first walk of plain NiNode children (max 600 nodes). Returns name -> node address.
std::map<std::string, uint32_t> CollectBones(uint32_t root);

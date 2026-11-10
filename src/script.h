#pragma once
#include <string>
#include <vector>

// Ragdoll reaction scripts: DeliriumFNV_reactions.txt (messages on a timeline, per reaction) and the optional import of messages from a GTA V
// physicstasks.ymt. Messages are checked against the engine's own registry (names and parameter types) when the file is read.
struct ScriptParam {
	std::string name; char kind = 'f';          // f float, i int, b bool, v vec3, s text
	float f = 0, f2 = 0; bool rnd = false;     // rnd: a random value between f and f2 each time the message is sent
	int b = 0; float v[3] = { 0, 0, 0 }; std::string s;
};
struct ScriptMsg {
	std::string tx;                             // time expression: 1.5, $fall, $balance+0.3
	std::string name;                           // engine message, or @macro for one of the built-in actions
	bool macro = false;
	std::vector<ScriptParam> p;
};
struct ScriptBlock { std::string name; std::vector<ScriptMsg> msgs; };

void ScriptClear();
// Reads the reaction file (missing file = no overrides). ymtPath is the physicstasks.ymt the import lines read (may be empty). Returns the number of blocks.
int ScriptLoad(const std::string& path, const std::string& ymtPath);
// All blocks named base or base2, base3 ... (one is picked at random by the caller).
std::vector<const ScriptBlock*> ScriptBlocks(const char* base);

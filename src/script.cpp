#include "script.h"
#include "log.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include "msgdb.inc"

namespace {
struct ParamInfo { std::string name; char kind; };
struct MsgInfo { std::string name; std::map<std::string, ParamInfo> params; };

std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string Trim(const std::string& s) { size_t a = 0, b = s.size(); while (a < b && isspace((unsigned char)s[a])) a++; while (b > a && isspace((unsigned char)s[b - 1])) b--; return s.substr(a, b - a); }

const std::map<std::string, MsgInfo>& Registry() {
	static std::map<std::string, MsgInfo> reg;
	if (!reg.empty()) return reg;
	for (const auto& e : kMsgDb) {
		MsgInfo mi; mi.name = e.msg;
		std::string ps = e.params; size_t i = 0;
		while (i <= ps.size()) { size_t j = ps.find('|', i); if (j == std::string::npos) j = ps.size(); std::string one = ps.substr(i, j - i); i = j + 1; if (one.empty()) continue; size_t c = one.rfind(':'); if (c == std::string::npos) continue; ParamInfo pi{ one.substr(0, c), one[c + 1] }; mi.params[Lower(pi.name)] = pi; }
		mi.params["start"] = ParamInfo{ "start", 'b' };               // every message takes start
		reg[Lower(mi.name)] = mi;
	}
	return reg;
}

std::vector<ScriptBlock> g_blocks;

// One "key=value" turned into a typed parameter for message m. Returns false (and says why) when the engine would refuse it.
bool MakeParam(const MsgInfo& m, const std::string& key, const std::string& val, ScriptParam& out, std::string& why) {
	auto it = m.params.find(Lower(key));
	if (it == m.params.end()) { why = "'" + m.name + "' has no parameter '" + key + "'"; return false; }
	out.name = it->second.name; out.kind = it->second.kind; std::string v = Trim(val);
	if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
	const std::string lv = Lower(v);
	switch (out.kind) {
	case 'f': case 'i': {
		const size_t d = v.find("..");                                  // 0.5..1.5 = a random value in that range
		char* e = nullptr;
		if (d != std::string::npos) { out.f = (float)strtod(v.substr(0, d).c_str(), &e); out.f2 = (float)strtod(v.substr(d + 2).c_str(), nullptr); out.rnd = true; return true; }
		out.f = (float)strtod(v.c_str(), &e); if (e == v.c_str()) { why = "'" + key + "' needs a number, got '" + v + "'"; return false; } return true; }
	case 'b': if (lv == "true" || lv == "1" || lv == "yes") out.b = 1; else if (lv == "false" || lv == "0" || lv == "no") out.b = 0; else { why = "'" + key + "' needs true or false, got '" + v + "'"; return false; } return true;
	case 'v': { float a = 0, b = 0, c = 0; if (sscanf(v.c_str(), "%f,%f,%f", &a, &b, &c) != 3) { why = "'" + key + "' needs x,y,z, got '" + v + "'"; return false; } out.v[0] = a; out.v[1] = b; out.v[2] = c; return true; }
	default: out.s = v; return true;
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------------- XML
struct XNode { std::string tag, text; std::map<std::string, std::string> at; std::vector<XNode> kids; };

bool XParse(const std::string& s, size_t& i, XNode& n, int depth = 0) {
	if (depth > 60 || i >= s.size() || s[i] != '<') return false;
	i++; size_t j = i; while (j < s.size() && !isspace((unsigned char)s[j]) && s[j] != '/' && s[j] != '>') j++;
	n.tag = s.substr(i, j - i); i = j;
	for (;;) {                                                                                 // attributes
		while (i < s.size() && isspace((unsigned char)s[i])) i++;
		if (i >= s.size()) return false;
		if (s[i] == '/') { i++; if (i < s.size() && s[i] == '>') { i++; return true; } return false; }
		if (s[i] == '>') { i++; break; }
		size_t k = i; while (k < s.size() && s[k] != '=' && !isspace((unsigned char)s[k]) && s[k] != '>' && s[k] != '/') k++;
		std::string an = s.substr(i, k - i); i = k; while (i < s.size() && isspace((unsigned char)s[i])) i++;
		if (i < s.size() && s[i] == '=') { i++; while (i < s.size() && isspace((unsigned char)s[i])) i++; if (i < s.size() && (s[i] == '"' || s[i] == '\'')) { const char q = s[i++]; size_t e = s.find(q, i); if (e == std::string::npos) return false; n.at[an] = s.substr(i, e - i); i = e + 1; } }
		else if (i == k) return false;
	}
	for (;;) {                                                                                 // content
		if (i >= s.size()) return false;
		if (s[i] != '<') { size_t e = s.find('<', i); if (e == std::string::npos) return false; n.text += s.substr(i, e - i); i = e; continue; }
		if (s.compare(i, 4, "<!--") == 0) { size_t e = s.find("-->", i); if (e == std::string::npos) return false; i = e + 3; continue; }
		if (s.compare(i, 2, "</") == 0) { size_t e = s.find('>', i); if (e == std::string::npos) return false; i = e + 1; return true; }
		XNode c; if (!XParse(s, i, c, depth + 1)) return false; n.kids.push_back(std::move(c));
	}
}
const XNode* Child(const XNode& n, const std::string& tag, int nth = 0) { int k = 0; for (const auto& c : n.kids) if (c.tag == tag) { if (k++ == nth) return &c; } return nullptr; }
std::string AtOr(const XNode* n, const char* a, const std::string& d = "") { if (!n) return d; auto it = n->at.find(a); return it == n->at.end() ? d : it->second; }
const XNode* FindMessages(const XNode& n) { if (const XNode* m = Child(n, "Messages")) return m; for (const auto& c : n.kids) if (const XNode* m = FindMessages(c)) return m; return nullptr; }

// Messages the engine knows, from one task state of a physicstasks.ymt, as "send at time 0" script messages; the rest is counted and named.
int ImportYmt(const std::string& ymtPath, const std::string& path, std::vector<ScriptMsg>& out, std::string& report) {
	static std::string cache, cacheFor;
	if (cacheFor != ymtPath) { cache.clear(); cacheFor = ymtPath; FILE* f = fopen(ymtPath.c_str(), "rb"); if (f) { char buf[1 << 16]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) cache.append(buf, n); fclose(f); } }
	if (cache.empty()) { report = "cannot read " + ymtPath; return -1; }
	std::vector<std::string> tok; { size_t i = 0; while (i <= path.size()) { size_t j = path.find('/', i); if (j == std::string::npos) j = path.size(); tok.push_back(path.substr(i, j - i)); i = j + 1; } }
	const std::string key = "<Name>" + tok[0] + "</Name>"; size_t at = cache.find(key); if (at == std::string::npos) { report = "task '" + tok[0] + "' not in the ymt"; return -1; }
	size_t st = cache.rfind("<Item ", at); if (st == std::string::npos) { report = "task element not found"; return -1; }
	XNode root; size_t pos = st; if (!XParse(cache, pos, root)) { report = "the ymt could not be read at that task"; return -1; }
	const XNode* cur = &root;
	for (size_t k = 1; k < tok.size() && cur; k++) { std::string t = tok[k]; int nth = 0; size_t h = t.find('#'); if (h != std::string::npos) { nth = atoi(t.c_str() + h + 1); t = t.substr(0, h); } cur = Child(*cur, t, nth); }
	if (!cur) { report = "path '" + path + "' not found"; return -1; }
	const XNode* msgs = FindMessages(*cur); if (!msgs) { report = "no messages under '" + path + "'"; return -1; }
	int total = 0, kept = 0; std::set<std::string> skipped;
	for (const auto& it : msgs->kids) {
		if (it.tag != "Item" || AtOr(&it, "type") != "CNmMessage") continue; total++;
		const XNode* nm = Child(it, "Name"); std::string mname = nm ? Trim(nm->text) : "";
		auto r = Registry().find(Lower(mname)); if (r == Registry().end()) { skipped.insert(mname); continue; }
		ScriptMsg sm; sm.tx = "0"; sm.name = r->second.name;
		if (const XNode* ps = Child(it, "Params")) for (const auto& pi : ps->kids) {
			if (pi.tag != "Item") continue; const XNode* pn = Child(pi, "Name"); if (!pn) continue; std::string pname = Trim(pn->text), ptype = AtOr(&pi, "type"), val;
			const XNode* v = Child(pi, "Value"); ScriptParam sp; std::string why;
			if (ptype.find("Random") != std::string::npos) {
				const XNode* mn = Child(pi, "Min"); const XNode* mx = Child(pi, "Max"); if (!mn || !mx) continue; if (ptype.find("Bool") != std::string::npos) continue;
				val = AtOr(mn, "value") + ".." + AtOr(mx, "value");
			} else if (ptype == "CNmParameterVector") val = AtOr(v, "x", "0") + "," + AtOr(v, "y", "0") + "," + AtOr(v, "z", "0");
			else if (ptype == "CNmParameterString") val = v ? Trim(v->text) : "";
			else val = AtOr(v, "value");
			if (MakeParam(r->second, pname, val, sp, why)) sm.p.push_back(sp);
		}
		out.push_back(sm); kept++;
	}
	char b[96]; snprintf(b, sizeof b, "%d of %d messages understood by the engine", kept, total); report = b;
	if (!skipped.empty()) { report += "; skipped: "; bool first = true; for (auto& s : skipped) { if (!first) report += ", "; report += s; first = false; } }
	return kept;
}

std::vector<std::string> Tokens(const std::string& line) {
	std::vector<std::string> t; size_t i = 0;
	while (i < line.size()) {
		while (i < line.size() && isspace((unsigned char)line[i])) i++; if (i >= line.size()) break;
		std::string cur; bool q = false;
		while (i < line.size() && (q || !isspace((unsigned char)line[i]))) { if (line[i] == '"') q = !q; cur += line[i++]; }
		t.push_back(cur);
	}
	return t;
}
}  // namespace

void ScriptClear() { g_blocks.clear(); }

int ScriptLoad(const std::string& path, const std::string& ymtPath) {
	g_blocks.clear();
	FILE* f = fopen(path.c_str(), "rb"); if (!f) return 0;
	std::string all; { char buf[1 << 14]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n); fclose(f); }
	size_t pos = 0; int lineNo = 0, problems = 0; ScriptBlock* cur = nullptr;
	auto bad = [&](const std::string& what) { if (problems++ < 25) Log("script: %s line %d: %s", path.c_str(), lineNo, what.c_str()); };
	while (pos <= all.size()) {
		size_t e = all.find('\n', pos); if (e == std::string::npos) e = all.size(); std::string line = all.substr(pos, e - pos); pos = e + 1; lineNo++;
		size_t sc = line.find(';'); if (sc != std::string::npos) { bool inq = false; for (size_t k = 0; k < sc; k++) if (line[k] == '"') inq = !inq; if (!inq) line = line.substr(0, sc); }
		line = Trim(line); if (line.empty()) continue;
		if (line[0] == '[') { size_t r = line.find(']'); if (r == std::string::npos) { bad("missing ]"); continue; } g_blocks.push_back(ScriptBlock{ Trim(line.substr(1, r - 1)), {} }); cur = &g_blocks.back(); continue; }
		if (!cur) { bad("a message outside any [Block]"); continue; }
		std::vector<std::string> t = Tokens(line); if (t.empty()) continue;
		if (Lower(t[0]) == "import") {                                                            // import Task/State [time]
			if (t.size() < 2) { bad("import needs Task/State"); continue; }
			if (ymtPath.empty()) { bad("import needs the ymt file (Ymt= in the ini [Scripts] section)"); continue; }
			std::vector<ScriptMsg> got; std::string rep; const int n = ImportYmt(ymtPath, t[1], got, rep);
			Log("script: [%s] import %s: %s", cur->name.c_str(), t[1].c_str(), rep.c_str());
			if (n > 0) { const std::string tx = t.size() > 2 ? t[2] : "0"; for (auto& m : got) { m.tx = tx; cur->msgs.push_back(m); } }
			continue;
		}
		if (t.size() < 2) { bad("expected: time message [key=value ...]"); continue; }
		ScriptMsg m; m.tx = t[0];
		if (t[1][0] == '@') { m.macro = true; m.name = Lower(t[1]); cur->msgs.push_back(m); continue; }
		auto r = Registry().find(Lower(t[1])); if (r == Registry().end()) { bad("the engine has no message '" + t[1] + "' (see DeliriumFNV_messages.txt)"); continue; }
		m.name = r->second.name; bool ok = true;
		for (size_t k = 2; k < t.size(); k++) {
			size_t eq = t[k].find('='); if (eq == std::string::npos) { bad("'" + t[k] + "' is not key=value (ignored)"); continue; }
			ScriptParam sp; std::string why; if (!MakeParam(r->second, t[k].substr(0, eq), t[k].substr(eq + 1), sp, why)) { bad(why + " (that parameter is ignored)"); continue; }
			m.p.push_back(sp);
		}
		if (ok) cur->msgs.push_back(m);
	}
	Log("script: %s: %d blocks%s", path.c_str(), (int)g_blocks.size(), problems ? " (some lines were skipped, see above)" : "");
	return (int)g_blocks.size();
}

std::vector<const ScriptBlock*> ScriptBlocks(const char* base) {
	std::vector<const ScriptBlock*> out; const std::string b = Lower(base);
	for (const auto& blk : g_blocks) {
		const std::string n = Lower(blk.name); if (n.compare(0, b.size(), b) != 0) continue;
		bool digits = true; for (size_t i = b.size(); i < n.size(); i++) if (!isdigit((unsigned char)n[i])) digits = false;
		if (digits && !blk.msgs.empty()) out.push_back(&blk);
	}
	return out;
}

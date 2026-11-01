#include "probe.h"
#include "log.h"
#include "ingame.h"
#include "observe.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

struct TraceRec { uint32_t ms, thisPtr, ret, a0, a1, a2; };
struct Probe {
	char label[56]; uint32_t addr; bool vtable; uint32_t original;
	volatile LONG count; volatile LONG firstTick;
	uint32_t lastThis, lastRet, lastArg[3];
	TraceRec* trace; uint32_t traceCap; volatile LONG traceSeq;
	int role;                                                       // ProbeRole, for the observer     // optional ring buffer of the last calls
};
static const size_t kMaxProbes = 2048;
static Probe g_probes[kMaxProbes]; static size_t g_n;
static uint8_t* g_mem; static size_t g_used, g_cap = 1 << 20;
static volatile LONG g_markTick;

// Called from every thunk with the original this (ecx), and a pointer to the original return address / stack args.
extern "C" void __cdecl LogHit(uint32_t idx, uint32_t thisPtr, uint32_t* stack) {
	Probe& p = g_probes[idx];
	if (InterlockedIncrement(&p.count) == 1) p.firstTick = (LONG)(GetTickCount() - (DWORD)g_markTick);
	p.lastThis = thisPtr; p.lastRet = stack[0]; p.lastArg[0] = stack[1]; p.lastArg[1] = stack[2]; p.lastArg[2] = stack[3];
	if (p.role) ObserveOnHit(p.role, thisPtr, stack + 0);
	if (p.trace) {
		LONG s = InterlockedIncrement(&p.traceSeq) - 1; TraceRec& r = p.trace[(uint32_t)s % p.traceCap];
		r.ms = GetTickCount() - (DWORD)g_markTick; r.thisPtr = thisPtr; r.ret = stack[0]; r.a0 = stack[1]; r.a1 = stack[2]; r.a2 = stack[3];
	}
}

static uint8_t* Alloc(size_t n) {
	if (!g_mem) g_mem = (uint8_t*)VirtualAlloc(nullptr, g_cap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!g_mem || g_used + n > g_cap) return nullptr;
	uint8_t* p = g_mem + g_used; g_used += (n + 15) & ~size_t(15); return p;
}
static void Put32(uint8_t*& p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
static void Jmp(uint8_t*& p, uint32_t to) { *p++ = 0xE9; Put32(p, to - ((uint32_t)(uintptr_t)p + 4)); }

// pushad; pushfd; lea eax,[esp+24h]; push eax; push ecx; push idx; call LogHit; add esp,0Ch; popfd; popad; jmp continueTo
static uint8_t* EmitThunk(uint32_t idx, uint32_t continueTo) {
	uint8_t* start = Alloc(48); if (!start) return nullptr; uint8_t* p = start;
	*p++ = 0x60; *p++ = 0x9C;
	*p++ = 0x8D; *p++ = 0x44; *p++ = 0x24; *p++ = 0x24;
	*p++ = 0x50; *p++ = 0x51; *p++ = 0x68; Put32(p, idx);
	*p++ = 0xB8; Put32(p, (uint32_t)(uintptr_t)&LogHit); *p++ = 0xFF; *p++ = 0xD0;
	*p++ = 0x83; *p++ = 0xC4; *p++ = 0x0C; *p++ = 0x9D; *p++ = 0x61;
	Jmp(p, continueTo);
	return start;
}
static bool Writable(void* a, size_t n, DWORD* old) { return VirtualProtect(a, n, PAGE_EXECUTE_READWRITE, old) != 0; }

static bool AddVtableSlot(const char* label, uint32_t slotAddr) {
	if (g_n >= kMaxProbes) return false;
	uint32_t orig = *(volatile uint32_t*)slotAddr;
	if (orig < 0x401000 || orig > 0xFDE58B) return false;                      // not a code pointer: leave it alone
	uint32_t idx = (uint32_t)g_n; Probe& pr = g_probes[idx]; memset(&pr, 0, sizeof pr);
	snprintf(pr.label, sizeof pr.label, "%s", label); pr.addr = slotAddr; pr.vtable = true; pr.original = orig;
	uint8_t* t = EmitThunk(idx, orig); if (!t) return false;
	DWORD old; if (!Writable((void*)slotAddr, 4, &old)) return false;
	*(volatile uint32_t*)slotAddr = (uint32_t)(uintptr_t)t; VirtualProtect((void*)slotAddr, 4, old, &old);
	g_n++; return true;
}
static bool AddFunction(const char* label, uint32_t addr, const std::vector<uint8_t>& expect) {
	size_t len = expect.size();
	if (len < 5 || len > 16 || g_n >= kMaxProbes) { Log("probe %s: bad steal length %u", label, (unsigned)len); return false; }
	if (memcmp((void*)addr, expect.data(), len) != 0) { Log("probe %s @%08X: bytes differ from the expected prologue, NOT hooked", label, addr); return false; }
	uint32_t idx = (uint32_t)g_n; Probe& pr = g_probes[idx]; memset(&pr, 0, sizeof pr);
	snprintf(pr.label, sizeof pr.label, "%s", label); pr.addr = addr; pr.vtable = false; pr.original = addr;
	uint8_t* tramp = Alloc(len + 5); if (!tramp) return false;
	memcpy(tramp, expect.data(), len); uint8_t* q = tramp + len; Jmp(q, addr + (uint32_t)len);
	uint8_t* t = EmitThunk(idx, (uint32_t)(uintptr_t)tramp); if (!t) return false;
	DWORD old; if (!Writable((void*)addr, len, &old)) return false;
	uint8_t* w = (uint8_t*)addr; *w++ = 0xE9; Put32(w, (uint32_t)(uintptr_t)t - (addr + 5));
	for (size_t i = 5; i < len; i++) *w++ = 0x90;
	VirtualProtect((void*)addr, len, old, &old); FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
	g_n++; return true;
}

static uint32_t ParseU32(const char* s) { return (uint32_t)strtoul(s, nullptr, 0); }
int ProbesInstall(const char* ini) {
	char sect[16384]; int ok = 0;
	DWORD n = GetPrivateProfileSectionA("Vtables", sect, sizeof sect, ini);
	for (const char* e = sect; n && *e; e += strlen(e) + 1) {                    // Label=0xVTABLE,slots
		char key[64]; const char* eq = strchr(e, '='); if (!eq) continue;
		snprintf(key, sizeof key, "%.*s", (int)(eq - e), e);
		char* comma; uint32_t vt = strtoul(eq + 1, &comma, 0); uint32_t slots = (*comma == ',') ? ParseU32(comma + 1) : 0;
		int added = 0;
		for (uint32_t s = 0; s < slots && s < 600; s++) {
			char lab[64]; snprintf(lab, sizeof lab, "%s[%u]", key, s);
			if (AddVtableSlot(lab, vt + 4 * s)) added++;
		}
		Log("vtable probe %s @%08X: %d/%u slots hooked", key, vt, added, slots); ok += added;
	}
	n = GetPrivateProfileSectionA("SlotProbes", sect, sizeof sect, ini);          // Label=0xVTABLE,slot : single slots, the cheap way
	for (const char* e = sect; n && *e; e += strlen(e) + 1) {
		char key[64]; const char* eq = strchr(e, '='); if (!eq) continue;
		snprintf(key, sizeof key, "%.*s", (int)(eq - e), e);
		char* comma; uint32_t vt = strtoul(eq + 1, &comma, 0); if (*comma != ',') continue; uint32_t slot = ParseU32(comma + 1);
		if (AddVtableSlot(key, vt + 4 * slot)) { ok++; Log("slot probe %s @%08X hooked", key, vt + 4 * slot); } else Log("slot probe %s NOT hooked", key);
	}
	n = GetPrivateProfileSectionA("Functions", sect, sizeof sect, ini);
	for (const char* e = sect; n && *e; e += strlen(e) + 1) {                    // Label=0xADDR|55 8B EC ...
		char key[64]; const char* eq = strchr(e, '='); if (!eq) continue;
		snprintf(key, sizeof key, "%.*s", (int)(eq - e), e);
		char* bar; uint32_t addr = strtoul(eq + 1, &bar, 0); if (*bar != '|') continue;
		std::vector<uint8_t> bytes; for (char* t = bar + 1; *t;) { char* end; unsigned long v = strtoul(t, &end, 16); if (end == t) break; bytes.push_back((uint8_t)v); t = end; }
		if (AddFunction(key, addr, bytes)) { ok++; Log("function probe %s @%08X hooked (%u bytes stolen)", key, addr, (unsigned)bytes.size()); }
	}
	n = GetPrivateProfileSectionA("Trace", sect, sizeof sect, ini);                // Label=ring_size : keep the last calls of that probe
	for (const char* e = sect; n && *e; e += strlen(e) + 1) {
		char key[64]; const char* eq = strchr(e, '='); if (!eq) continue;
		snprintf(key, sizeof key, "%.*s", (int)(eq - e), e); uint32_t cap = ParseU32(eq + 1); if (cap < 8 || cap > 4096) cap = 64;
		for (size_t i = 0; i < g_n; i++) if (strcmp(g_probes[i].label, key) == 0) { g_probes[i].trace = new TraceRec[cap](); g_probes[i].traceCap = cap; Log("trace enabled on %s (last %u calls)", key, cap); }
	}
	for (size_t i = 0; i < g_n; i++) {                                              // roles used by the observer
		if (strcmp(g_probes[i].label, "Character[206]") == 0) g_probes[i].role = kRoleDamage;
		if (strcmp(g_probes[i].label, "bhkBlendCollisionObject[44]") == 0) g_probes[i].role = kRoleMotionType;
	}
	return ok;
}

void ProbesMark() {
	g_markTick = (LONG)GetTickCount();
	for (size_t i = 0; i < g_n; i++) { g_probes[i].count = 0; g_probes[i].firstTick = 0; g_probes[i].traceSeq = 0; }
	Log("---- MARK: counters reset. Do the thing now (kill an NPC), then press the dump key.");
	IngameNotify("DeliriumFNV: MARK set - do the event now, then press the dump key");
}
void ProbesDump() {
	// The game keeps incrementing counters while we read them, so sort a private snapshot: sorting live values with a
	// comparator that can change mid-sort is undefined behaviour and can crash the process.
	struct Row { char label[56]; uint32_t addr, original, thisPtr, ret, a[3]; bool vtable; long count, first; };
	std::vector<Row> rows;
	for (size_t i = 0; i < g_n; i++) {
		const Probe& p = g_probes[i]; long c = p.count; if (c <= 0) continue;
		Row r{}; memcpy(r.label, p.label, sizeof r.label); r.addr = p.addr; r.original = p.original; r.thisPtr = p.lastThis; r.ret = p.lastRet;
		r.a[0] = p.lastArg[0]; r.a[1] = p.lastArg[1]; r.a[2] = p.lastArg[2]; r.vtable = p.vtable; r.count = c; r.first = p.firstTick;
		rows.push_back(r);
	}
	std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count != b.count ? a.count < b.count : a.first < b.first; });
	Log("---- DUMP: %u of %u probes were hit since the mark (rarest first)", (unsigned)rows.size(), (unsigned)g_n);
	for (const Row& r : rows)
		Log("%-34s %s%08X orig=%08X n=%-6ld first=+%ldms this=%08X ret=%08X a0=%08X a1=%08X a2=%08X", r.label, r.vtable ? "slot@" : "fn@", r.addr, r.original,
			r.count, r.first, r.thisPtr, r.ret, r.a[0], r.a[1], r.a[2]);
	for (size_t i = 0; i < g_n; i++) {                                              // call-by-call traces, oldest first
		const Probe& pr = g_probes[i]; if (!pr.trace || pr.count <= 0) continue;
		uint32_t seq = (uint32_t)pr.traceSeq, cnt = seq < pr.traceCap ? seq : pr.traceCap, start = seq < pr.traceCap ? 0 : seq % pr.traceCap;
		std::vector<TraceRec> t(cnt); for (uint32_t k = 0; k < cnt; k++) t[k] = pr.trace[(start + k) % pr.traceCap];
		Log("TRACE %s: %u calls since mark, showing %u", pr.label, seq, cnt);
		for (uint32_t k = 0; k < cnt; k++) Log("  #%-3u +%ums this=%08X ret=%08X a0=%08X a1=%08X a2=%08X", k, t[k].ms, t[k].thisPtr, t[k].ret, t[k].a0, t[k].a1, t[k].a2);
	}
	Log("---- end of dump");
	IngameNotify("DeliriumFNV: DUMP saved - %u of %u probes were hit. Log: %s", (unsigned)rows.size(), (unsigned)g_n, LogPath());
}
static int g_markVk, g_dumpVk;
static DWORD WINAPI HotkeyThread(LPVOID) {
	bool m = false, d = false;
	for (;;) {
		bool mk = (GetAsyncKeyState(g_markVk) & 0x8000) != 0, dk = (GetAsyncKeyState(g_dumpVk) & 0x8000) != 0;
		if (mk && !m) ProbesMark();
		if (dk && !d) ProbesDump();
		m = mk; d = dk; Sleep(40);
	}
}
void ProbesStartHotkeys(int markVk, int dumpVk) { g_markVk = markVk; g_dumpVk = dumpVk; CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr); }

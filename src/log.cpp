#include "log.h"
#include <windows.h>
#include <cstdio>
#include <cstdarg>
static char g_path[520];
const char* LogPath() { return g_path; }
static FILE* g_f; static CRITICAL_SECTION g_cs; static bool g_init;
void LogOpen(const char* path) {
	if (!g_init) { InitializeCriticalSection(&g_cs); g_init = true; }
	// keep the previous session's log as <name>.prev: a crash is usually read after the next start
	char prev[540]; snprintf(prev, sizeof prev, "%s.prev", path); remove(prev); rename(path, prev);
	snprintf(g_path, sizeof g_path, "%s", path);
	g_f = fopen(path, "w");
}
void Log(const char* fmt, ...) {
	if (!g_f) return;
	char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	EnterCriticalSection(&g_cs);
	fprintf(g_f, "[%8lu] %s\n", (unsigned long)GetTickCount(), buf); fflush(g_f);
	LeaveCriticalSection(&g_cs);
}

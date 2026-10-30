#include "ingame.h"
#include "log.h"
#include "observe.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

typedef bool (__cdecl* QueueUIMessage_t)(const char* msg, UInt32 emotion, const char* ddsPath, const char* soundName, float msgTime, bool maybeNextToDisplay);
static const uint32_t kQueueUIMessage = 0x007052F0;      // FalloutNV.exe 1.4.0.525; prologue checked before use
static CRITICAL_SECTION g_cs; static bool g_csInit;
static std::deque<std::string> g_queue;
static bool g_ready, g_canShow; static unsigned g_frames, g_lastShown;

void IngameNotify(const char* fmt, ...) {
	char buf[200]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
	Log("onscreen: %s", buf);
	if (!g_csInit) return;
	EnterCriticalSection(&g_cs); if (g_queue.size() < 16) g_queue.push_back(buf); LeaveCriticalSection(&g_cs);
}

static void OnMessage(NVSEMessagingInterface::Message* m) {
	switch (m->type) {
	case kMsg_PostLoadGame: case kMsg_NewGame: g_ready = true; g_frames = 0; g_lastShown = 0; break;   // a game world exists now
	case kMsg_ExitToMainMenu: g_ready = false; break;
	case kMsg_MainGameLoop:
		if (g_ready) ObserveTick();
		if (!g_ready || !g_canShow) break;
		g_frames++;
		if (g_frames < 150 || g_frames - g_lastShown < 200) break;                                       // wait ~2 s after load, then one message per ~3 s
		std::string s; EnterCriticalSection(&g_cs); if (!g_queue.empty()) { s = g_queue.front(); g_queue.pop_front(); } LeaveCriticalSection(&g_cs);
		if (s.empty()) break;
		g_lastShown = g_frames;
		((QueueUIMessage_t)kQueueUIMessage)(s.c_str(), 0, nullptr, nullptr, 3.0f, false);
		break;
	}
}

void IngameInit(NVSEInterface* nvse) {
	InitializeCriticalSection(&g_cs); g_csInit = true;
	const uint8_t* f = (const uint8_t*)kQueueUIMessage;
	g_canShow = f[0] == 0x55 && f[1] == 0x8B && f[2] == 0xEC;                                           // push ebp; mov ebp,esp
	if (!g_canShow) Log("onscreen: QueueUIMessage prologue not as expected, on-screen messages disabled");
	NVSEMessagingInterface* msg = nvse->QueryInterface ? (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging) : nullptr;
	if (!msg || !nvse->GetPluginHandle) { Log("onscreen: NVSE messaging interface unavailable"); g_canShow = false; return; }
	bool ok = msg->RegisterListener(nvse->GetPluginHandle(), "NVSE", OnMessage);
	Log("onscreen: messaging listener %s (interface v%u)", ok ? "registered" : "FAILED", (unsigned)msg->version);
}

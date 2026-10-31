// DeliriumFNV - phase 1: discovery probes + engine smoke test. No gameplay changes.
#include "nvse_min.h"
#include "log.h"
#include "probe.h"
#include "engine.h"
#include "ingame.h"
#include "observe.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <string>

static EngineApi g_engine;
static std::string g_pluginDir;   // <dir of this dll>\DeliriumFNV  (independent of the game's working directory)
static std::string g_root;        // <dir of this dll>
static std::string g_logDir;      // where logs and breadcrumbs go (must be writable: Program Files folders often are not)

static bool CanWrite(const std::string& dir) {
	std::string t = dir + "\\DeliriumFNV_write_test.tmp"; FILE* f = fopen(t.c_str(), "w");
	if (!f) return false; fclose(f); DeleteFileA(t.c_str()); return true;
}
// Preference order: Documents\My Games\FalloutNV\NVSE (next to nvse.log), the plugin folder, then %TEMP%.
static void PickLogDir() {
	char docs[MAX_PATH] = {};
	if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
		std::string d = std::string(docs) + "\\My Games"; CreateDirectoryA(d.c_str(), nullptr);
		d += "\\FalloutNV"; CreateDirectoryA(d.c_str(), nullptr); d += "\\NVSE"; CreateDirectoryA(d.c_str(), nullptr);
		if (CanWrite(d)) { g_logDir = d; return; }
	}
	if (CanWrite(g_pluginDir)) { g_logDir = g_pluginDir; return; }
	char tmp[MAX_PATH] = {}; GetTempPathA(MAX_PATH, tmp); g_logDir = tmp; if (!g_logDir.empty() && g_logDir.back() == '\\') g_logDir.pop_back();
}

// Plain breadcrumb files so a "nothing happened" report can be told apart: not loaded / refused / loaded.
static void Crumb(const char* name, const char* fmt, unsigned a = 0, unsigned b = 0) {
	if (g_logDir.empty()) PickLogDir();
	std::string p = g_logDir + "\\" + name; FILE* f = fopen(p.c_str(), "a");
	if (f) { fprintf(f, fmt, a, b); fputc('\n', f); fclose(f); }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {                       // loader lock: only remember paths here, do no real work
		DisableThreadLibraryCalls(h);
		char path[MAX_PATH] = {}; GetModuleFileNameA(h, path, MAX_PATH);
		g_root = path; g_root.erase(g_root.find_last_of("\\/")); g_pluginDir = g_root + "\\DeliriumFNV";
	}
	return TRUE;
}

extern "C" {
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = NVSE_INFO_VERSION; info->name = "DeliriumFNV"; info->version = 1;
	CreateDirectoryA(g_pluginDir.c_str(), nullptr);
	Crumb("DeliriumFNV_2_query.txt", "Query: nvse=%08X runtime=%08X (want runtime 040020D0)", nvse->nvseVersion, nvse->runtimeVersion);
	if (nvse->isEditor) return false;
	return nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;      // the addresses in the ini are for 1.4.0.525 only
}
__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
	Crumb("DeliriumFNV_3_load.txt", "Load called: nvse=%08X runtime=%08X", nvse->nvseVersion, nvse->runtimeVersion);
	LogOpen((g_logDir + "\\DeliriumFNV.log").c_str());
	Log("DeliriumFNV build phase2a-observer loading; runtime %08X, nvse %08X; plugin dir %s; log dir %s", nvse->runtimeVersion, nvse->nvseVersion, g_pluginDir.c_str(), g_logDir.c_str());
	IngameInit(const_cast<NVSEInterface*>(nvse));
	std::string ini = g_pluginDir + "\\DeliriumFNV.ini";
	if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) ini = g_root + "\\DeliriumFNV.ini";   // also accept the ini beside the dll
	Log("ini %s", GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES ? "NOT FOUND (put the data files in the DeliriumFNV folder)" : "found");
	if (GetPrivateProfileIntA("General", "SmokeTest", 1, ini.c_str())) {
		if (EngineLoad(g_engine, g_pluginDir)) IngameNotify(EngineSmokeTest(g_engine, g_pluginDir) ? "DeliriumFNV: engine smoke test finished (see log)" : "DeliriumFNV: engine smoke test FAILED (see log)");
		else IngameNotify("DeliriumFNV: engine DLL not loaded - smoke test skipped");
	}
	if (GetPrivateProfileIntA("General", "Observe", 1, ini.c_str())) { ObserveInit(&g_engine, EngineFittedBuild(), g_pluginDir.c_str()); Log("observer: armed, engine body %s. Kill an NPC to trigger it.", EngineFittedBuild() ? "ready" : "MISSING (skeleton logging only)"); }
	if (GetPrivateProfileIntA("General", "Probes", 1, ini.c_str())) {
		int n = ProbesInstall(ini.c_str());
		Log("probes installed: %d", n);
		IngameNotify("DeliriumFNV running: %d probes installed. Insert = mark, Delete = dump. Log: %s", n, LogPath());
		if (n) ProbesStartHotkeys(GetPrivateProfileIntA("General", "MarkKey", 0x2D, ini.c_str()), GetPrivateProfileIntA("General", "DumpKey", 0x2E, ini.c_str()));
	}
	return true;
}
}

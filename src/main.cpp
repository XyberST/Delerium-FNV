// DeliriumFNV: Delirium (Euphoria-style active ragdoll) physics for dying NPCs in Fallout New Vegas 1.4.0.525 (xNVSE plugin).
#include "nvse_min.h"
#include "log.h"
#include "probe.h"
#include "engine.h"
#include "ingame.h"
#include "observe.h"
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static EngineApi g_engine;
static std::string g_pluginDir;   // <dir of this dll>\DeliriumFNV  (data files, engine DLL, optional ini)
static std::string g_root;        // <dir of this dll>
static std::string g_logDir;      // must be writable: Program Files folders often are not

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

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {                       // loader lock: only remember paths here
		DisableThreadLibraryCalls(h);
		char path[MAX_PATH] = {}; GetModuleFileNameA(h, path, MAX_PATH);
		g_root = path; g_root.erase(g_root.find_last_of("\\/")); g_pluginDir = g_root + "\\DeliriumFNV";
	}
	return TRUE;
}

extern "C" {
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = NVSE_INFO_VERSION; info->name = "DeliriumFNV"; info->version = 1;
	if (nvse->isEditor) return false;
	return nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;      // every address in this plugin is for 1.4.0.525
}
__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
	PickLogDir();
	LogOpen((g_logDir + "\\DeliriumFNV.log").c_str());
	Log("DeliriumFNV 0.13 loading; runtime %08X, nvse %08X; plugin dir %s; log dir %s", nvse->runtimeVersion, nvse->nvseVersion, g_pluginDir.c_str(), g_logDir.c_str());
	IngameInit(const_cast<NVSEInterface*>(nvse));
	std::string ini = g_pluginDir + "\\DeliriumFNV.ini";
	if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) ini = g_root + "\\DeliriumFNV.ini";   // also accept the ini beside the dll
	if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { Log("DeliriumFNV.ini not found, plugin disabled"); return true; }
	if (!GetPrivateProfileIntA("General", "Enabled", 1, ini.c_str())) { Log("disabled in the ini"); return true; }

	bool engineOk = EngineLoad(g_engine, g_pluginDir) && EngineBuildFit(g_engine, g_pluginDir);
	if (engineOk && GetPrivateProfileIntA("General", "SmokeTest", 0, ini.c_str())) EngineSmokeTest(g_engine, g_pluginDir);
	if (!engineOk) { IngameNotify("DeliriumFNV: engine failed to load, see the log"); return true; }

	ObserveSetDrive(GetPrivateProfileIntA("General", "Drive", 1, ini.c_str()) != 0);
	ObserveSetRequireDead(GetPrivateProfileIntA("General", "RequireDead", 1, ini.c_str()) != 0);
	char b[32]; GetPrivateProfileStringA("General", "SimSeconds", "8", b, sizeof b, ini.c_str()); float sim = (float)atof(b);
	GetPrivateProfileStringA("General", "HoldSeconds", "300", b, sizeof b, ini.c_str());
	ObserveSetLimits(GetPrivateProfileIntA("General", "MaxActors", 4, ini.c_str()), sim, (float)atof(b));
	{ char rs[32]; GetPrivateProfileStringA("General", "RestSpeed", "1.5", rs, sizeof rs, ini.c_str()); ObserveSetRelease(GetPrivateProfileIntA("General", "ReleaseAtRest", 1, ini.c_str()) != 0, (float)atof(rs), (unsigned)GetPrivateProfileIntA("General", "RestFrames", 30, ini.c_str())); }
	{ char g[32], v[32], ag[32]; GetPrivateProfileStringA("General", "DriveAngularGain", "0.4", ag, sizeof ag, ini.c_str()); GetPrivateProfileStringA("General", "DriveGain", "1.0", g, sizeof g, ini.c_str()); GetPrivateProfileStringA("General", "DriveMaxSpeed", "8", v, sizeof v, ini.c_str()); ObserveSetDriveBodies(GetPrivateProfileIntA("General", "DriveGameBodies", 1, ini.c_str()) != 0, (float)atof(g), (float)atof(v), (float)atof(ag)); }
	{ char bh[32], bs[32], bm[32]; GetPrivateProfileStringA("General", "BalanceHealth", "120", bh, sizeof bh, ini.c_str()); GetPrivateProfileStringA("General", "BalanceSeconds", "2.5", bs, sizeof bs, ini.c_str()); GetPrivateProfileStringA("General", "BalanceMaxSpeed", "6", bm, sizeof bm, ini.c_str());
	  { char bf[32], bv[32]; GetPrivateProfileStringA("General", "BalanceFallSeconds", "1.0", bf, sizeof bf, ini.c_str()); GetPrivateProfileStringA("General", "BalanceVelocityScale", "0.5", bv, sizeof bv, ini.c_str()); ObserveSetBalanceMore((float)atof(bf), GetPrivateProfileIntA("General", "BalanceSteps", 8, ini.c_str()), (float)atof(bv), GetPrivateProfileIntA("General", "BalancePush", 1, ini.c_str()) != 0); }
	  ObserveSetBalance(GetPrivateProfileIntA("General", "BalanceWhenDying", 1, ini.c_str()) != 0, (float)atof(bh), (float)atof(bs), (float)atof(bm), GetPrivateProfileIntA("General", "DismemberedGetDelirium", 1, ini.c_str()) != 0); }
	{ char k[6][32]; const char* nm[6] = { "HitMinKick", "HeadHitBalance", "TorsoHitBalance", "PelvisHitBalance", "LegHitBalance", "ArmHitBalance" }; const char* df[6] = { "1.0", "0.0", "1.0", "0.8", "0.4", "1.3" };
	  for (int j = 0; j < 6; j++) GetPrivateProfileStringA("General", nm[j], df[j], k[j], sizeof k[j], ini.c_str());
	  ObserveSetHitLocation(GetPrivateProfileIntA("General", "HitLocationAware", 1, ini.c_str()) != 0, (float)atof(k[0]), (float)atof(k[1]), (float)atof(k[2]), (float)atof(k[3]), (float)atof(k[4]), (float)atof(k[5])); }
	ObserveSetBodies(GetPrivateProfileIntA("General", "KeyframeGameBodies", 1, ini.c_str()) != 0, GetPrivateProfileIntA("General", "SkipDismembered", 1, ini.c_str()) != 0);
	{ char v[32], hp[32]; GetPrivateProfileStringA("General", "VelocityScale", "1.0", v, sizeof v, ini.c_str()); GetPrivateProfileStringA("General", "ShotHealth", "1.0", hp, sizeof hp, ini.c_str()); ObserveSetTuning((float)atof(v), (float)atof(hp)); }
	{ char f[32]; GetPrivateProfileStringA("World", "GroundFilter", "auto", f, sizeof f, ini.c_str()); char c[32]; GetPrivateProfileStringA("World", "GroundCell", "0.35", c, sizeof c, ini.c_str());
	  ObserveSetWorld(GetPrivateProfileIntA("World", "Collision", 1, ini.c_str()) != 0, GetPrivateProfileIntA("World", "Calibrate", 0, ini.c_str()) != 0, (uint32_t)strtoul(f, nullptr, 0), (float)atof(c), GetPrivateProfileIntA("World", "GroundHalf", 10, ini.c_str())); }
	ObserveInit(&g_engine, EngineFittedBuild(), g_pluginDir.c_str());
	int n = ProbesInstall(ini.c_str());
	Log("%d hooks installed", n);
	IngameNotify("DeliriumFNV active");
	return true;
}
}

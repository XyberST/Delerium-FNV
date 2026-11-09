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

static std::string g_iniPath;
static void ReloadSettings();
static void ApplyIni(const std::string& ini) {
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

	// ---- everything else: drive tuning, timing, performance, reactions
	{
		ExtraCfg x;
		auto F = [&](const char* sec, const char* key, float d) { char def[32], v[32]; snprintf(def, sizeof def, "%g", d); GetPrivateProfileStringA(sec, key, def, v, sizeof v, ini.c_str()); return (float)atof(v); };
		auto I = [&](const char* sec, const char* key, int d) { return (int)GetPrivateProfileIntA(sec, key, d, ini.c_str()); };
		auto B = [&](const char* sec, const char* key, bool d) { return GetPrivateProfileIntA(sec, key, d ? 1 : 0, ini.c_str()) != 0; };
		x.driveTau = F("Drive", "Tau", x.driveTau); x.driveSoftAbove = F("Drive", "SoftAbove", x.driveSoftAbove); x.driveMaxCatchUp = F("Drive", "MaxCatchUp", x.driveMaxCatchUp);
		x.driveMaxAngular = F("Drive", "MaxAngular", x.driveMaxAngular); x.driveGiveUpDistance = F("Drive", "GiveUpDistance", x.driveGiveUpDistance); x.driveGiveUpSeconds = F("Drive", "GiveUpSeconds", x.driveGiveUpSeconds);
		x.restSeconds = F("Timing", "RestSeconds", x.restSeconds); x.restMinSeconds = F("Timing", "RestMinSeconds", x.restMinSeconds); x.hiddenScale = F("Timing", "HiddenScale", x.hiddenScale);
		x.simRate = F("Performance", "SimRate", x.simRate); x.maxDistance = F("Performance", "MaxDistance", x.maxDistance); x.minFps = F("Performance", "MinFps", x.minFps); x.profile = B("Performance", "Profile", x.profile);
		x.burn = B("Reactions", "Burn", x.burn); x.burnByDamageTicks = B("Reactions", "BurnByDamageTicks", x.burnByDamageTicks); x.dotTicks = I("Reactions", "DamageTicks", x.dotTicks); x.dotWindow = F("Reactions", "DamageWindow", x.dotWindow);
		x.burnBalanceSeconds = F("Reactions", "BurnBalanceSeconds", x.burnBalanceSeconds); x.burnFlailSeconds = F("Reactions", "BurnFlailSeconds", x.burnFlailSeconds);
		{ char kw[96]; GetPrivateProfileStringA("Reactions", "BurnKeywords", "", kw, sizeof kw, ini.c_str()); strcpy_s(x.burnKeywords, sizeof x.burnKeywords, kw); }
		x.shock = B("Reactions", "Shock", x.shock); x.shockBalanceSeconds = F("Reactions", "ShockBalanceSeconds", x.shockBalanceSeconds); x.shockSeconds = F("Reactions", "ShockSeconds", x.shockSeconds);
		{ char kw[96]; GetPrivateProfileStringA("Reactions", "ShockKeywords", "", kw, sizeof kw, ini.c_str()); strcpy_s(x.shockKeywords, sizeof x.shockKeywords, kw); }
		x.dumpNodes = B("Reactions", "DumpNodes", x.dumpNodes);
		x.explosion = B("Reactions", "Explosion", x.explosion); x.explosionSeconds = F("Reactions", "ExplosionSeconds", x.explosionSeconds);
		x.explosionRollChance = F("Reactions", "ExplosionRollChance", x.explosionRollChance); x.explosionWritheChance = F("Reactions", "ExplosionWritheChance", x.explosionWritheChance);
		x.playerDeath = B("Reactions", "PlayerDeath", x.playerDeath); x.knockdown = B("Reactions", "Knockdown", x.knockdown); x.playerFallback = B("Reactions", "PlayerFallback", x.playerFallback);
		x.gutCurl = B("Reactions", "GutCurl", x.gutCurl); x.gutCurlChance = F("Reactions", "GutCurlChance", x.gutCurlChance); x.gutCurlSeconds = F("Reactions", "GutCurlSeconds", x.gutCurlSeconds);
		x.fall = B("Reactions", "Fall", x.fall); x.fallMinHeight = F("Reactions", "FallMinHeight", x.fallMinHeight); x.fallCatchChance = F("Reactions", "FallCatchChance", x.fallCatchChance);
		x.flailStiffness = F("Reactions", "FlailStiffness", x.flailStiffness); x.flailAmplitude = F("Reactions", "FlailAmplitude", x.flailAmplitude); x.flailPeriod = F("Reactions", "FlailPeriod", x.flailPeriod);
		ObserveSetExtra(x);
		{ char rk[16]; GetPrivateProfileStringA("General", "ReloadKey", "0x79", rk, sizeof rk, ini.c_str()); ObserveSetReloadHook(&ReloadSettings, (int)strtoul(rk, nullptr, 0)); }      // F10 by default (0x79), 0 = off
	}
	// ---- [EngineParams]: any engine task parameter, as Task.parameter=value (for example NMShot_Standing.shotBodyStiffness=14)
	{
		char buf[16384]; DWORD n = GetPrivateProfileSectionA("EngineParams", buf, sizeof buf, ini.c_str()); int applied = 0;
		for (char* p = buf; n && *p; p += strlen(p) + 1) {
			std::string line = p; size_t eq = line.find('='), dot = line.find('.'); if (eq == std::string::npos || dot == std::string::npos || dot > eq) continue;
			std::string task = line.substr(0, dot), param = line.substr(dot + 1, eq - dot - 1), val = line.substr(eq + 1);
			if (g_engine.nm_set_task_param && g_engine.nm_set_task_param(task.c_str(), "*", param.c_str(), (float)atof(val.c_str())) == 0) applied++;
			else Log("EngineParams: could not set %s", line.c_str());
		}
		if (applied) Log("EngineParams: %d engine parameters applied", applied);
	}
}
static void ReloadSettings() { if (g_iniPath.empty()) return; ApplyIni(g_iniPath); Log("settings reloaded from %s", g_iniPath.c_str()); IngameNotify("DeliriumFNV: settings reloaded"); }

extern "C" {
__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = NVSE_INFO_VERSION; info->name = "DeliriumFNV"; info->version = 1;
	if (nvse->isEditor) return false;
	return nvse->runtimeVersion == RUNTIME_VERSION_1_4_0_525;      // every address in this plugin is for 1.4.0.525
}
__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse) {
	PickLogDir();
	LogOpen((g_logDir + "\\DeliriumFNV.log").c_str());
	Log("DeliriumFNV 0.16 loading; runtime %08X, nvse %08X; plugin dir %s; log dir %s", nvse->runtimeVersion, nvse->nvseVersion, g_pluginDir.c_str(), g_logDir.c_str());
	IngameInit(const_cast<NVSEInterface*>(nvse));
	std::string ini = g_pluginDir + "\\DeliriumFNV.ini";
	if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) ini = g_root + "\\DeliriumFNV.ini";   // also accept the ini beside the dll
	if (GetFileAttributesA(ini.c_str()) == INVALID_FILE_ATTRIBUTES) { Log("DeliriumFNV.ini not found, plugin disabled"); return true; }
	if (!GetPrivateProfileIntA("General", "Enabled", 1, ini.c_str())) { Log("disabled in the ini"); return true; }

	bool engineOk = EngineLoad(g_engine, g_pluginDir) && EngineBuildFit(g_engine, g_pluginDir);
	if (engineOk && GetPrivateProfileIntA("General", "SmokeTest", 0, ini.c_str())) EngineSmokeTest(g_engine, g_pluginDir);
	if (!engineOk) { IngameNotify("DeliriumFNV: engine failed to load, see the log"); return true; }

	g_iniPath = ini; ApplyIni(ini);
	ObserveInit(&g_engine, EngineFittedBuild(), g_pluginDir.c_str());
	int n = ProbesInstall(ini.c_str());
	Log("%d hooks installed", n);
	IngameNotify("DeliriumFNV active");
	return true;
}
}

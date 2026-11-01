// The few xNVSE plugin ABI pieces this plugin needs, so the project builds without the xNVSE source tree.
// Layouts match nvse/PluginAPI.h (xNVSE 6.x): PluginInfo and the head of NVSEInterface.
#pragma once
#include <cstdint>
typedef uint32_t UInt32;
struct PluginInfo { UInt32 infoVersion; const char* name; UInt32 version; };
typedef UInt32 PluginHandle;
struct NVSEInterface {
	UInt32 nvseVersion; UInt32 runtimeVersion; UInt32 editorVersion; UInt32 isEditor;
	void* RegisterCommand; void* SetOpcodeBase;                       // unused placeholders, keep the layout
	void* (*QueryInterface)(UInt32 id);
	PluginHandle (*GetPluginHandle)(void);                            // only valid during Query/Load
	/* remaining members unused */
};
enum { kInterface_Messaging = 2 };
struct NVSEMessagingInterface {
	struct Message { const char* sender; UInt32 type; UInt32 dataLen; void* data; };
	typedef void (*EventCallback)(Message* msg);
	UInt32 version;
	bool (*RegisterListener)(PluginHandle listener, const char* sender, EventCallback handler);
	bool (*Dispatch)(PluginHandle sender, UInt32 messageType, void* data, UInt32 dataLen, const char* receiver);
};
// NVSE message ids (enum order in PluginAPI.h)
enum { kMsg_ExitToMainMenu = 2, kMsg_LoadGame = 3, kMsg_PreLoadGame = 6, kMsg_PostLoadGame = 8, kMsg_NewGame = 14, kMsg_MainGameLoop = 20 };
#define NVSE_INFO_VERSION 1
#define RUNTIME_VERSION_1_4_0_525 0x040020D0u

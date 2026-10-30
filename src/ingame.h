// On-screen confirmation messages (the top-left HUD notification the game uses for "Added X").
// Messages are queued from any thread and shown from NVSE's main-loop message, only while a game is loaded.
#pragma once
#include "nvse_min.h"
void IngameInit(NVSEInterface* nvse);
void IngameNotify(const char* fmt, ...);

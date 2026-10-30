// Discovery probes: log-only hooks on vtable slots and function entries, so the first in-game run tells us which
// address does what. Nothing is changed except that a thunk logs the call and then continues to the original code.
#pragma once
#include <cstdint>
#include <string>
// Reads [Vtables] and [Functions] from the ini and installs probes. Returns the number installed.
int ProbesInstall(const char* iniPath);
// Zero all counters (F9): do this just before the event you want to identify.
void ProbesMark();
// Write every probe hit since the last mark to the log, rarest first (F10).
void ProbesDump();
void ProbesStartHotkeys(int markVk, int dumpVk);

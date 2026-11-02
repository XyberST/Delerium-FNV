// Single-slot vtable hooks used for death detection and for finding the physics world.
#pragma once
// Reads [SlotProbes] (Label=0xVTABLE,slot) from the ini and installs the hooks. Returns how many were installed.
int ProbesInstall(const char* iniPath);

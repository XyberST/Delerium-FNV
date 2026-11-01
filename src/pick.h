// Calls the game's own Havok ray cast (bhkWorld::PickObject, FalloutNV.exe 1.4.0.525 at 0xC696D0).
// Argument block layout, decoded from real calls captured in-game:
//   +00 from (x y z 0)   +10 to (x y z 0)   +20 byte flag   +24 filterInfo (0x011F0025 / 0x011D0025 seen)
//   output: +30 normal   +40 hitFraction (1.0 = nothing hit)   +44 and +50 are initialised to -1
// The call is thiscall (ecx = the bhkWorld), pops one argument, returns the hit flag in al.
#pragma once
#include <cstdint>
void PickSetWorld(uint32_t bhkWorld);            // the world the game is currently picking in; set from the probe on every game call
uint32_t PickWorld();
// from/to in the units the game passes to its own picks. Returns false if the call could not be made.
bool PickCast(const float from[3], const float to[3], uint32_t filter, bool& hit, float& fraction, float normal[3]);

// Calls the game's own Havok ray cast (bhkWorld::PickObject, FalloutNV.exe 1.4.0.525 at 0xC696D0).
// Argument block layout, decoded from real calls captured in-game:
//   +00 from (x y z 0)   +10 to (x y z 0)   +20 byte flag   +24 filterInfo (0x011F0025 / 0x011D0025 seen)
//   output: +30 normal   +40 hitFraction (1.0 = nothing hit)   +44 and +50 are initialised to -1
// The call is thiscall (ecx = the bhkWorld), pops one argument, returns the hit flag in al.
#pragma once
#include <cstdint>
#include <cstddef>
void PickSetWorld(uint32_t bhkWorld);            // the world the game is currently picking in; set from the probe on every game call
uint32_t PickWorld();
// Ray end points are game units multiplied by this before the call (Havok works in metres, so 0.0142875). Calibration decides which scale the game's PickObject wants.
void PickSetScale(float s);
float PickScale();
// The object the world search settled on turned out not to be a bhkWorld-family object (its vtable slot 50 is not PickObject), so these let calibration try its neighbours.
int PickCandidates(uint32_t* out, int max);                    // the found object, its parent, and the objects they point to (code-section vtable)
void PickDescribe(uint32_t obj, char* out, size_t n);          // "addr: vt .. slot50 .. +8 .."
void PickForceWorld(uint32_t w);                               // remember a world that was proven by a real hit
// Finds the bhkWorld of the actor's own cell by walking pointers from TESObjectREFR::parentCell (+0x40), two levels deep. A candidate must have a
// vtable whose slot 50 is the game's PickObject. Returns 0 if none; 'how' receives where it was found (or why not).
uint32_t PickFindWorld(uint32_t actor, uint32_t collisionObject, char* how, size_t howLen);
// from/to in the units the game passes to its own picks. Returns false if the call could not be made.
bool PickCast(const float from[3], const float to[3], uint32_t filter, bool& hit, float& fraction, float normal[3]);

// ---- other game facts the takeover needs (FalloutNV.exe 1.4.0.525) ----
// The game's frame time in seconds, scaled by VATS and slow-motion effects the way xNVSE scales its own script timers. 0 if unreadable.
float GameFrameSeconds();
// Sets a bhkBlendCollisionObject's motion type (4 = keyframed: the body follows its bone instead of simulating). Same call the game makes itself.
bool GameSetMotionType(uint32_t collisionObject, int motionType);
// Sets the Havok linear / angular velocity of a bhkRigidBody through its hkpMaxSizeMotion (FalloutNV 1.4.0.525). Vectors are Havok units per second (game units / 7).
bool GameBodyOk(uint32_t bhkRigidBody);                                      // true if the object's vtable has the expected setters
bool GameSetBodyVelocity(uint32_t bhkRigidBody, const float lin[3], const float ang[3]);

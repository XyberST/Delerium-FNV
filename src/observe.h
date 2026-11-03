// Phase 2a: when an NPC dies, read its skeleton and run a Delirium body beside the game's own ragdoll, logging both.
// Changes nothing in the game. Called from the probe thunks (OnHit, any thread) and from the main loop (Tick, game thread).
#pragma once
#include "engine.h"
#include <cstdint>
enum ProbeRole { kRoleNone = 0, kRoleDamage = 1, kRoleMotionType = 2, kRolePick = 3, kRoleWorld = 4 };
void ObserveInit(EngineApi* api, void* fitBuild, const char* dir);   // fitBuild may be null (engine missing)
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack);
void ObserveTick();
void ObserveReset();   // end every session (save load, new game, exit to menu)
void ObserveSetRequireDead(bool on);   // true: only take over actors whose life state is dying/dead
void ObserveSetDrive(bool on);                                          // false: only watch, never write to the game
void ObserveSetLimits(int maxActors, float simSeconds, float holdSeconds);
// World collision: sample the ground around each corpse with the game's ray cast and give Delirium that surface. Off by default.
// Tuning: VelocityScale multiplies the speed inherited from the game's own ragdoll (1 = as the game did it); ShotHealth is how much
// "health" the dying body is given (lower = collapses faster).
// KeyframeGameBodies=1 switches the game's own ragdoll bodies to keyframed so they follow the pose we write (loot, bullets and the crosshair then hit the visible body).
// SkipDismembered=1 leaves actors with cut-off limbs entirely to the game.
void ObserveSetBodies(bool keyframeGameBodies, bool skipDismembered);
void ObserveSetTuning(float velocityScale, float shotHealth);
void ObserveSetWorld(bool collision, bool calibrate, uint32_t filter, float cell, int half);

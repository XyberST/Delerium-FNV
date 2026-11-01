// Phase 2a: when an NPC dies, read its skeleton and run a Delirium body beside the game's own ragdoll, logging both.
// Changes nothing in the game. Called from the probe thunks (OnHit, any thread) and from the main loop (Tick, game thread).
#pragma once
#include "engine.h"
#include <cstdint>
enum ProbeRole { kRoleNone = 0, kRoleDamage = 1, kRoleMotionType = 2 };
void ObserveInit(EngineApi* api, void* fitBuild, const char* dir);   // fitBuild may be null (engine missing)
void ObserveOnHit(int role, uint32_t thisPtr, const uint32_t* stack);
void ObserveTick();
void ObserveReset();   // end every session (save load, new game, exit to menu)
void ObserveSetRequireDead(bool on);   // true: only take over actors whose life state is dying/dead
void ObserveSetDrive(bool on);                                          // false: only watch, never write to the game
void ObserveSetLimits(int maxActors, float simSeconds, float holdSeconds);

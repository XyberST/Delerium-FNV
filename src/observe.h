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
// ReleaseAtRest=1: when the Delirium body comes to rest, the game's bodies are switched back to simulated so the corpse can be grabbed like a vanilla one.
void ObserveSetRelease(bool on, float restSpeed, unsigned restFrames);   // a corpse counts as at rest after restFrames frames with every part slower than restSpeed (m/s)
void ObserveSetTuning(float velocityScale, float shotHealth);
void ObserveSetWorld(bool collision, bool calibrate, uint32_t filter, float cell, int half);

// DriveGameBodies=1: Delirium's pose is applied as velocities to the game's own bodies instead of writing bones. gain scales the correction, maxSpeed is in m/s.
void ObserveSetDriveBodies(bool on, float gain, float maxSpeed, float angularGain);

// BalanceWhenDying: a dying body that was not thrown hard and still has its head tries to stay on its feet (engine 'health' = healthWhileBalancing, about 'seconds' before it gives up).
// DismemberedGetDelirium: kills with a cut-off limb are driven too (the cut-off parts are left to the game).
void ObserveSetBalance(bool on, float health, float seconds, float maxSpeed, bool dismemberedOk);
// BalanceFallSeconds: how long the body takes to go limp once its strength runs out; BalanceSteps: how many stumbling steps it may take; BalanceVelocityScale: how much of the kill's motion a balancing body inherits; push: use the engine's standard shot push.
void ObserveSetBalanceMore(float fallSeconds, int steps, float velocityScale, bool push);
// HitLocationAware: the part of the body the kill hit is found from which part the game kicked hardest (at least minKick m/s relative to the rest) and given to the engine's shot reaction.
// The multipliers scale how long a body hit there keeps fighting (0 = collapses at once).
void ObserveSetHitLocation(bool on, float minKick, float head, float torso, float pelvis, float leg, float arm);

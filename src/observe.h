// Phase 2a: when an NPC dies, read its skeleton and run a Delirium body beside the game's own ragdoll, logging both.
// Changes nothing in the game. Called from the probe thunks (OnHit, any thread) and from the main loop (Tick, game thread).
#pragma once
#include "engine.h"
#include <cstdint>
enum ProbeRole { kRoleNone = 0, kRoleDamage = 1, kRoleMotionType = 2, kRolePick = 3, kRoleWorld = 4, kRolePush = 5, kRoleImpulse = 6 };
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

// Everything below is read from DeliriumFNV.ini and can be changed live (ReloadKey).
struct ExtraCfg {
	// drive: how the game's own bodies are pulled toward Delirium's pose
	float driveTau = 0.06f;              // time constant of the pull, seconds (bigger = softer)
	float driveSoftAbove = 60.0f;        // mean distance (game units) above which the pull is softened
	float driveMaxCatchUp = 50.0f;       // never ask a body to close more than this many units at once
	float driveMaxAngular = 10.0f;       // rad/s
	float driveGiveUpDistance = 220.0f;  // mean distance at which the corpse is left to the game
	float driveGiveUpSeconds = 0.5f;     // ... but not before this much simulated time
	// timing
	float restSeconds = 0.5f;            // calm for this long (after the body has fallen) = at rest
	float restMinSeconds = 1.5f;         // never earlier than this
	float hiddenScale = 0.2f;            // a bone scaled below this fraction counts as cut off
	// performance
	float simRate = 60.0f;               // engine steps per second at most (0 = every frame)
	float maxDistance = 4000.0f;         // game units; corpses farther from the player are left to the game (0 = no limit)
	float minFps = 25.0f;                // below this frame rate new corpses are left to the game (0 = off)
	bool profile = true;                 // log what each corpse cost
	bool playerFallback = false;         // start the player from the life state when no kill burst was seen (experimental: the game may be tearing the body down)
	bool knockAgony = true, knockOutAgony = true;   // a knocked-down actor keeps writhing, pedalling and curling on the floor until the game gets it up (knocked out too, unless KnockOutAgony=0)
	float knockAgonySeconds = 3.0f, knockAgonyPause = 0.5f;   // length of one writhing spell, limp pause between spells
	bool creatures = true;               // actors of the Creature class (ghouls, mutants, ...) may get Delirium
	bool largeActors = true;             // actors much bigger than a human (super mutants) may get Delirium: the rig was made for a human skeleton
	float largeScale = 1.2f;             // leg bone length relative to the player's above which an actor counts as large
	float impulseAll = 1.0f, impulseBullet = 1.0f, impulseMelee = 1.0f, impulseExplosion = 3.0f, impulseKnock = 1.0f, impulseOther = 1.0f;   // multipliers on the motion the game gives a body when it dies or is knocked down
	bool fetalAgony = true, holdGut = true;      // knocked-down / unconscious actors curl up, kick their legs and hold their gut
	bool drown = true; float drownDepth = 0.9f, drownSeconds = 6.0f, drownBuoyancy = 0.92f, drownDrag = 2.0f;   // actors that die in water: depth of the feet below the surface (m), struggle time, how much of gravity the water cancels, drag
	bool shotReact = true; float shotRapidWindow = 0.35f, shotExtend = 1.5f, shotMaxExtend = 4.0f, shotShorten = 0.35f, shotPush = 12.0f;   // shooting an actor that is dying on its feet
	bool tapImpulses = true; float tapFeed = 1.0f, tapMin = 0.05f;   // impulses the game applies to a Delirium body (bullets, other mods) are fed into the simulation
	bool hitData = true;                 // read the game's hit data (hit location, weapon) when an actor is damaged
	char fireWeapons[160] = "";          // FormIDs (hex, comma separated) of weapons whose hits set an actor on fire; the log lists the weapon of every hit
	char shockWeapons[160] = "";         // FormIDs of weapons whose hits electrocute
	bool unconscious = true;             // actors the game has knocked unconscious (life state 3) get Delirium like knocked-out ones
	float unconsciousAgonySeconds = 6.0f; // how long an unconscious actor keeps writhing before it lies limp and is handed back
	bool stagger = true;                 // a shot actor staggers away from whoever shot it (direction taken from the attacker's position)
	bool woundAgony = true;              // the agony depends on where the actor was hit: curl up for a gut shot, pedal with the good leg for a leg shot
	bool lod = true;                     // far ragdolls are stepped at a lower rate
	float lodNear = 900.0f, lodMid = 2200.0f, lodRateMid = 30.0f, lodRateFar = 20.0f;   // game units, steps per second
	float frameBudgetMs = 2.5f;          // plugin time per frame it aims for; over it the step rates drop and new ragdolls are left to the game (0 = no limit)
	float floorHalfMesh = 12.0f, floorHalfFlat = 20.0f;   // half size (metres) of the safety floor under the ground mesh / of the flat floor used without a mesh; the engine builds it from 2 triangles per square metre, so 100 cost ~30 ms
	bool knockdown = true;               // living actors knocked down by the game (explosion blast, knocked out) get Delirium until the game starts getting them up
	bool playerDeath = true;              // the player's own death ragdoll gets Delirium too (needs the PlayerCharacter[206] slot probe)
	// reactions
	bool burn = true, shock = true, explosion = true, gutCurl = true, fall = true, dumpNodes = true, burnByDamageTicks = true;
	float burnBalanceSeconds = 1.2f, burnFlailSeconds = 4.0f; int dotTicks = 3; float dotWindow = 6.0f;
	float shockBalanceSeconds = 0.6f, shockSeconds = 2.5f;
	float explosionSeconds = 1.6f, explosionRollChance = 0.3f, explosionWritheChance = 0.3f;
	float gutCurlChance = 0.6f, gutCurlSeconds = 2.5f;
	float fallMinHeight = 100.0f, fallCatchChance = 0.5f;
	float flailStiffness = 12.0f, flailAmplitude = 1.3f, flailPeriod = 0.8f;
	char burnKeywords[96] = "";           // node-name fragments (comma separated) that mark a burning actor; empty = off until DumpNodes has shown what the game attaches
	char shockKeywords[96] = "";          // same, for an electrocuted actor
};
void ObserveSetExtra(const ExtraCfg& c);
// ReloadKey: virtual-key code; pressing it while the game is in front calls fn (0 = no hot key).
void ObserveSetReloadHook(void (*fn)(), int vk);

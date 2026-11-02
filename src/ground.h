// World collision for the Delirium body: sample the ground height on a grid around the corpse and turn it into a triangle mesh.
// Pure geometry: the ray cast is passed in, so this is unit-tested against the real engine without the game.
#pragma once
#include <cstdint>
#include <vector>
struct GroundGrid {
	float cx = 0, cy = 0, cell = 0.3f; int half = 8;           // grid centre (m), cell size (m), cells each side: (2*half+1)^2 samples
	float zMin = -1.0f, zMax = 0.9f;                           // accepted heights (m); a hit outside this range is ignored (other actors, ceilings)
	std::vector<float> h; std::vector<uint8_t> ok; int next = 0;
	int side() const { return 2 * half + 1; }
	int total() const { return side() * side(); }
	bool done() const { return next >= total(); }
};
typedef bool (*GroundRay)(void* ctx, float x, float y, float& z);   // true and the ground height if the ray hit something in range
void GroundInit(GroundGrid& g, float cx, float cy, float cell, int half, float zMin, float zMax);
void GroundWork(GroundGrid& g, int maxSamples, GroundRay ray, void* ctx);       // takes up to maxSamples more samples
// Fills holes from neighbours (anything still empty gets fallbackZ) and writes the mesh. Returns the lowest height.
float GroundMesh(const GroundGrid& g, float fallbackZ, std::vector<float>& pos, std::vector<uint32_t>& tri);

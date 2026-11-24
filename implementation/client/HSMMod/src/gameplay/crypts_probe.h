// [mod] crypts_probe=1 (dev only): logs how the Crypts are built, for a shared Crypts battle map. Lines start
// "cryptsprobe:". Game thread only.
//
// The Crypts (Map_Procedural_Block_Test_Main) are rooms: each room class (a child of BP_Procedural_Block_Master_C) streams
// in three levels at its Origin Point (a Module level with the walls, doors and loot, a Lighting level, a Logic level
// whose BP_SpawnerPoint_Willies_C actors are the fighter spawns), and joins its children at its Entrances List. The
// offline decompile cannot read the room classes' defaults (unversioned packages), so this reads them in game:
//   catalog  each room class's defaults: its three level paths, entrances, flags and the classes it may join to
//   rooms    each room the game builds as the player walks: where it sits, its parent, and what its levels hold
//            (doors, spawn points, bounds), in the room's own frame
//   measure  every distinct room Module level, loaded far from the dungeon one at a time with its Lighting and Logic
//            levels, and measured: so every room type is measured once without walking into it
// It only reads, apart from the far loads. The far rooms are 3 to 7 km away and stay loaded until the Crypts close:
// unloading one crashed the game (crypts_probe.cpp, FarTick). The far measure is skipped in a progression run, where a far
// spawn point would take the player.
#pragma once
#include "ue/engine.h"
#include <cstdint>

namespace hsm::game {

void CryptsProbeLevel(bool on, bool cryptsLevel, ue::UObject* world, uint32_t nowMs);   // each level load (resets)
void CryptsProbeTick(uint32_t nowMs);                                   // each session tick

}  // namespace hsm::game

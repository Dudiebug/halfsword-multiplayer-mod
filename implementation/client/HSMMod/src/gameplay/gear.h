// Host-side random loadouts: a fighter spawned at a chosen tier gets a fresh passport from the game's own generator.
#pragma once
#include "gameplay/adapter.h"
#include <cstdint>
#include <vector>

namespace hsm::game {

// Generates a character of `tier` (clamped to 0..7) with BP_Generator_Characters_Random and returns it in the
// ExportPassport blob format, so it can be passed as SpawnOptions.blob. Empty on failure (logged).
std::vector<uint8_t> RandomPassportBlob(Adapter& a, int tier);

}  // namespace hsm::game

// Host-side random loadouts: a fighter spawned at a chosen tier gets a fresh passport from the game's own generator.
#pragma once
#include "gameplay/adapter.h"
#include "match/hsm_match.h"
#include <cstdint>
#include <vector>

namespace hsm::game {

// Generates a character of `tier` (clamped to 0..7) with BP_Generator_Characters_Random and returns it in the
// ExportPassport blob format, so it can be passed as SpawnOptions.blob. Empty on failure (logged).
// weapons != Any: the game's own weapon-type fields (GameWeaponTypeByte) are set for the call and put back after it;
// Any touches nothing. Axes & maces is one of the game's four mace and hafted types, the next one each call.
std::vector<uint8_t> RandomPassportBlob(Adapter& a, int tier, WeaponFilter weapons);

}  // namespace hsm::game

// Co-op Crypts (match IsCoopCrypts): this player's own crypt character, kept between runs in HSMMP_Crypts.sav next to the
// game's saves (never in GameProgress.sav; the save guard keeps the game's own save out of every session). The rule of what a
// run leaves is session/hsm_crypt_character.h; this is the file IO. Game thread only.
#pragma once
#include "gameplay/adapter.h"
#include <cstdint>
#include <vector>

namespace hsm::game::cryptschar {

// The kept character (a passport save, checked), or empty: none yet (a first run starts as the blank peasant), or neither
// the file nor its backup holds one. Logs which it read.
std::vector<uint8_t> Load(Adapter& adapter);
// Writes `passport` as the kept character; the one before is kept as HSMMP_Crypts.prev.sav. False when it could not.
bool Save(Adapter& adapter, const std::vector<uint8_t>& passport);

}  // namespace hsm::game::cryptschar

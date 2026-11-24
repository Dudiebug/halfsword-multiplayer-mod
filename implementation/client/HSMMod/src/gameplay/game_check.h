// The live check of the game names HSMMod uses (shared/session/hsm_game_check.h, generated/hsm_game_manifest.h). Each
// name is looked up in the running game once its class is there; a miss is logged ("game check: ...") and the
// multiplayer page says the game changed. Play and multiplayer go on either way.
#pragma once
#include <cstdint>
#include <string>

namespace hsm::game {

// After the self-test passes. testMiss (game_check_test=1 in HSMMod.ini, testing only) adds two names no build has, so
// the notice can be seen.
void StartGameCheck(bool testMiss);
// At each level load: the classes the new level brought in.
void GameCheckSweep();
// Each tick: a second look at a suspect miss once kGameCheckConfirmMs have passed.
void GameCheckTick();
// The multiplayer page's line; empty while nothing is confirmed changed.
std::string GameCheckNotice(int steamBuild);

}  // namespace hsm::game

// In-game test harness (human-run sensor). Launch the game with -hsmtest=all (or a comma list of probe names),
// enter any fight level; the probes run once, on the game thread, and write HSMTest.json next to the exe.
// Without the switch every call is a no-op.
#pragma once
#include "gameplay/adapter.h"

namespace hsm::test {
void Begin(bool selfTestOk);                              // once, after the engine self-test
void Tick(game::Adapter& adapter, bool inFightLevel, float dt);
}  // namespace hsm::test

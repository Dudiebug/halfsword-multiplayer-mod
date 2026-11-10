// Detects Half Sword Enhancer in the game process (rules: shared/session/hsm_enhancer.h). The mod report polls it
// (gameplay/mod_rules.cpp): lobbies that block it hold its keys (hse_hold.h) or lock it for a game.
#pragma once

namespace hsm::enhancer {
// Scans the process's modules for the Enhancer at most once every few seconds (the UE4SS route loads it after HSMMod),
// until it is found; then it stays found (the DLL is never unloaded). Found by its loader first, the scan goes on until
// HSE's own module is in, for hse_hold.cpp, which Poll also ticks. Game thread. Logs once when found.
void Poll();
// True once a scan found it.
bool Detected();
}  // namespace hsm::enhancer

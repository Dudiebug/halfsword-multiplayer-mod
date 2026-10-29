// Shared dice: the level's Blueprint random numbers (KismetMathLibrary RandomInteger, RandomFloatInRange, RandomBool ...)
// come out the same on every machine of a session. Those functions are native thunks the Blueprint VM reaches through
// UFunction::Func, so Install swaps Func of each for a thunk of ours. The replacement evaluates the call's parameters
// the way the stock thunk does (FFrame::Step on the caller's bytecode) and returns a value drawn from a stream keyed by
// (the level's seed, the calling actor's name, how many times that actor has rolled) instead of the C runtime's rand()
// (session/hsm_dice.h). It answers only for actors that were placed in the level (loaded from the map package, so their
// names agree on every machine) and are not fighters; everything else, and every call while no seed is set, runs the
// original thunk untouched. The host picks the seed per travel and the clients take it from LevelSeed. Game thread only.
#pragma once
#include <cstdint>
#include <string>

namespace hsm::game::shareddice {

// Swaps the random natives. Needs the Kismet classes loaded (after the engine self-test). False: unavailable, logged
// (nothing changed); a function whose thunk or parameters do not look as expected is skipped on its own.
bool Install();
// A new level's seed: rolls start counting again. The seed already set is ignored (a travel re-sent to a joiner).
void SetSeed(uint64_t seed);
// The session ended: back to the game's own randomness.
void Clear();
bool Active();
uint64_t Seed();
// One line for the health log: the seed, rolls answered and actors counted since it was set; empty when idle.
std::string HealthLine();

}  // namespace hsm::game::shareddice

// Crossbows in the lobby (0.9.0, protocol rev 36; experimental: needs the owner's crossbow test). The host's "Crossbows" row
// says how many random players are handed a crossbow at their spawn: none, 1, 2, half of them or all. PickCrossbowHolders is
// the pure draw: deterministic for a seed, distinct fighters only, never more than there are. Game-independent and unit-tested.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace hsm {

enum class Crossbows : uint8_t { Off = 0, One = 1, Two = 2, Half = 3, All = 4 };
constexpr uint8_t kCrossbowModeCount = 5;
inline bool ValidCrossbows(uint8_t m) { return m < kCrossbowModeCount; }
inline const char* CrossbowsName(uint8_t m) {
    switch (m) {
        case 1: return "1 player";
        case 2: return "2 players";
        case 3: return "Half";
        case 4: return "All";
        default: return "Off";
    }
}

// How many of `fighters` fighters get a crossbow: Half rounds up (one fighter alone is half of them), never more than there are.
inline size_t CrossbowCount(uint8_t mode, size_t fighters) {
    switch (mode) {
        case 1: return std::min<size_t>(1, fighters);
        case 2: return std::min<size_t>(2, fighters);
        case 3: return (fighters + 1) / 2;
        case 4: return fighters;
        default: return 0;
    }
}

// The entities (fighters, in any order, repeats ignored) that hold a crossbow this round: CrossbowCount of them, picked by a
// partial shuffle seeded with `seed`, returned in ascending order. The same input and seed give the same answer on every machine.
inline std::vector<uint8_t> PickCrossbowHolders(uint8_t mode, std::vector<uint8_t> fighters, uint32_t seed) {
    std::sort(fighters.begin(), fighters.end());
    fighters.erase(std::unique(fighters.begin(), fighters.end()), fighters.end());
    const size_t want = CrossbowCount(mode, fighters.size());
    uint32_t s = seed ? seed : 0x9E3779B9u;
    auto next = [&s]() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; };   // xorshift32
    for (size_t i = 0; i < want; ++i) std::swap(fighters[i], fighters[i + next() % (fighters.size() - i)]);
    fighters.resize(want);
    std::sort(fighters.begin(), fighters.end());
    return fighters;
}

}  // namespace hsm

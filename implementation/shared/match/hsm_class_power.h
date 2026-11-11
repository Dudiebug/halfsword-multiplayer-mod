// The host's class power limit (#2125, #2144): how far past the plain fighter a class may go in a lobby that uses classes, in power as
// the CLASSES tab shows it. +15 is the limit the mod always had (hsm_class.h kRuleMaxPoints); the host can raise it so stronger classes
// fit. Every machine reads it from the lobby (key hsm_cpow) and checks each class against it, as with the rest of the class rules.
#pragma once
#include <algorithm>
#include <cstdint>
#include <iterator>

namespace hsm {

inline constexpr uint8_t kClassPowerStops[] = {15, 30, 50, 80, 120, 200};
inline constexpr uint8_t kClassPowerDefault = 15;

// A value that is no stop (an old lobby, a bad key) reads as the default.
inline uint8_t SnapClassPower(int shown) {
    for (uint8_t s : kClassPowerStops) if (s == shown) return s;
    return kClassPowerDefault;
}
inline double ClassPowerPoints(int shown) { return SnapClassPower(shown) / 100.0; }
// The stepper stops at either end: the limit is a size, it does not wrap round.
inline uint8_t StepClassPower(int shown, int dir) {
    const int n = int(std::size(kClassPowerStops)), now = SnapClassPower(shown);
    int at = 0;
    for (int i = 0; i < n; ++i) if (kClassPowerStops[i] == now) at = i;
    return kClassPowerStops[std::clamp(at + (dir > 0) - (dir < 0), 0, n - 1)];
}

}  // namespace hsm

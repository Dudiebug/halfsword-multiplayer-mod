#pragma once
// The Win by row (#1407): what loses a fighter the round in the round formats besides death. These are the game's own
// duel rules (BP_LevelManager: "Spill first blood", "Keep your feet on the ground"). Willie_BP checks them on each
// fighter from its own "Current Lose Condition", copied from the game instance at BeginPlay, in Game Mode 1 (every
// lobby fight): 1 = a hit whose scaled added bleeding is 1 or more; 2 = Fallen, and still Fallen 1.5 s later. Either one
// calls Event Lose Match, which the mod's stock flow guard turns into a ring-out (a give-up crediting the last attacker).
// So the mod only has to set that byte on the fighter it simulates. Fenced ring's 3 lives on the game instance and the
// fence bags call Event Lose Match themselves, so it combines with either rule.
#include "match/hsm_match.h"
#include <cstdint>

namespace hsm {

enum class WinBy : uint8_t { Death = 0, FirstBlood = 1, Knockdown = 2 };
inline constexpr uint8_t kWinByCount = 3;

inline const char* WinByName(WinBy w) {
    switch (w) {
        case WinBy::FirstBlood: return "First blood";
        case WinBy::Knockdown: return "Knockdown";
        default: return "Death";
    }
}
inline const char* WinByHint(WinBy w) {
    switch (w) {
        case WinBy::FirstBlood: return "The first fighter to bleed from a hit loses the round, as in the game's own duel.";
        case WinBy::Knockdown: return "A fighter who falls and is still down 1.5 s later loses the round, as in the game's own duel.";
        default: return "A fighter is out of the round when they die or give up.";
    }
}

// The formats the row is for: the round formats but Tag (the fuse decides Tag). Per player sync only: under host
// physics the stock flow guard is off, so Event Lose Match would run the game's own lose screen.
inline bool WinByApplies(Format f, bool hostPhysics) { return IsRoundFormat(f) && !IsTag(f) && !hostPhysics; }
inline bool WinByApplies(const MatchConfig& c) { return WinByApplies(c.format, c.hostPhysics); }
// The rule a match runs: the picked one where it applies, else Death. A stored value out of range reads Death.
inline WinBy EffectiveWinBy(Format f, bool hostPhysics, uint8_t winBy) {
    return WinByApplies(f, hostPhysics) && winBy < kWinByCount ? WinBy(winBy) : WinBy::Death;
}
// The "Current Lose Condition" to put on a fighter this machine simulates (Enum_CombatEvent_LoseConditions), or -1 to
// leave the fighter's own: Death sets nothing, so a lobby without the rule runs exactly as before.
inline int WinByLoseCondition(WinBy w) { return w == WinBy::FirstBlood ? 1 : w == WinBy::Knockdown ? 2 : -1; }
inline WinBy StepWinBy(uint8_t winBy, int dir) {
    const uint8_t w = winBy < kWinByCount ? winBy : 0;
    return WinBy((w + (dir < 0 ? kWinByCount - 1 : 1)) % kWinByCount);
}

}  // namespace hsm

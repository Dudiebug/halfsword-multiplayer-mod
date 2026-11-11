// Chaos: each fighter's stats for the round (#2151). Every machine works them out the same way from what the MatchInfo
// already carries (the round and the fighter's Chaos role id) and the fighter's entity, so nothing new travels. The
// machine that simulates the body applies them in place of a picked class. Game-independent.
#pragma once
#include "match/hsm_class.h"
#include "match/hsm_roles.h"
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>

namespace hsm {

// What a Chaos stat can be: the game's own value times one of these. The jump is kRuleJumpBase times one of its three.
inline constexpr double kChaosStat[5] = {0.75, 0.9, 1.0, 1.1, 1.25};
inline constexpr double kChaosJump[3] = {0.8, 1.0, 1.2};

// One fighter's sheet for one round: run and walk together, strength, grip, kick, knockback and jump, each drawn on its
// own. No perks, no parts and no mass: the roles deal the build (match/hsm_roles.h).
inline ClassSheet ChaosClass(uint8_t roleId, uint8_t entity, uint32_t round) {
    uint32_t h = roles_detail::Mix((uint32_t(round) << 16) ^ (uint32_t(entity) << 8) ^ roleId ^ 0xC4A05u);
    auto draw = [&h](uint32_t n) { const uint32_t i = h % n; h = roles_detail::Mix(h + 0x9E3779B9u); return i; };
    ClassSheet c;
    c.run = c.walk = kChaosStat[draw(5)];
    c.strength = kChaosStat[draw(5)];
    c.grip = kChaosStat[draw(5)];
    c.kick = kChaosStat[draw(5)];
    c.knockback = kChaosStat[draw(5)];
    c.jump = kRuleJumpBase * kChaosJump[draw(3)];
    return c;
}

// The sheet in force for a fighter, or none: in Chaos only, from the countdown to the results, for a fighter the mode
// dealt a build.
inline std::optional<ClassSheet> ChaosClassFor(Format f, MatchPhase phase, uint8_t roleId, uint8_t entity, uint32_t round) {
    if (!IsChaos(f) || phase == MatchPhase::Idle || !IsChaosRole(roleId) || entity == 0) return std::nullopt;
    return ChaosClass(roleId, entity, round);
}

// What the player is told: "run +25%, strength -10%, jump +20%". A stat at the game's own value is left out.
inline std::string ChaosStatsLine(const ClassSheet& c) {
    std::string s;
    auto add = [&s](const char* name, double v) {
        const int pct = int(std::lround((v - 1.0) * 100.0));
        if (pct == 0) return;
        if (!s.empty()) s += ", ";
        s += std::string(name) + (pct > 0 ? " +" : " -") + std::to_string(std::abs(pct)) + "%";
    };
    add("run", c.run); add("strength", c.strength); add("grip", c.grip); add("kick", c.kick); add("knockback", c.knockback);
    add("jump", c.jump / kRuleJumpBase);
    return s.empty() ? std::string("the game's own stats") : s;
}

}  // namespace hsm

// Roles: what a fighter is in a match beyond its name. A role carries the fighter's gear (tier, kind, weapon filter), its
// build (passport Weight and Height) and its team. Every format has the lobby's role (the Fighter: the lobby's gear and tier,
// the game's own build, the team ResolveTeams picks); Riot and Juggernaut deal special ones. Game-independent: the host deals
// them (AssignRoles), MatchInfo carries each fighter's role id, and a client rebuilds the role from the id (RoleSpec).
//   Riot        one Knight (tier 7, the best kit) on team 0 against everyone else, Rioters at tiers 1..4, on team 1
//   Juggernaut  K tall, heavy Juggernauts on team 0 against the Skirmishers, short and light, in tier 4 gear against the Juggernauts' tier 7
// The special roles rotate at every rematch, so everybody gets a turn.
#pragma once
#include "match/hsm_match.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hsm {

// Role ids (the wire byte, FighterEntry::role). A Rioter's id says its tier (kRoleRioterFirst + tier - 1), so a client builds
// the same role from the id alone.
constexpr uint8_t kRoleFighter = 0, kRoleKnight = 1, kRoleRioterFirst = 2, kRoleRioterLast = 5, kRoleJuggernaut = 6, kRoleSkirmisher = 7;
constexpr uint8_t kRoleCount = 8;
inline bool ValidRoleId(uint8_t id) { return id < kRoleCount; }

constexpr double kRoleUnchanged = -1.0;                  // FighterRole::weight / height: the build the fighter would have without a role
constexpr uint8_t kKnightTier = 7;
constexpr uint8_t kRioterMinTier = 1, kRioterMaxTier = 4;
// The Juggernaut / Skirmisher builds. The passport's Weight and Height run 0..1 (BuildWeight clamps the Weight; the spawn's
// Height maps 0..1 to a 0.875..1 body scale and a 0..10 cm lift), so the Juggernaut is at the top of both and the Skirmisher
// at the bottom of the Height and at the lightest bot-table Weight (kTierWeight[1]). ASSUMPTION pending the in-game test: the
// game takes a Height written into the spawned fighter's own passport copy.
constexpr double kJuggernautWeight = 1.0, kJuggernautHeight = 1.0;
constexpr double kSkirmisherWeight = 0.15, kSkirmisherHeight = 0.0;
// The Juggernaut is a boss: the top tier's gear (kTierBudget[7]) and a big body (the spawn scales it past the passport's
// Height, config juggernaut_scale), against Skirmishers in tier 4 gear who have to gang up on it (pre24 tester feedback).
constexpr uint8_t kJuggernautTier = 7, kSkirmisherTier = 4;
// The game's body scale is MapRangeUnclamped(passport Height, 0, 1, 0.875, 1) (Willie_BP UserConstructionScript and Set
// Character Height, both re-run from the passport), so a bigger body is a passport Height past 1.0. The Juggernaut's
// size is given as a scale (config juggernaut_scale, 1.0..1.6) and written as the Height that the game maps to it; the
// game re-derives the same scale whenever it re-runs, on every machine. Side effects of the game reading that Height
// (Height Rate): the body takes less damage, its voice is deeper and louder.
constexpr double kJuggernautScaleMin = 1.0, kJuggernautScaleMax = 1.6, kJuggernautScaleDefault = 1.35;
inline double HeightForScale(double scale) { return (scale - 0.875) / 0.125; }
inline double ScaleForHeight(double height) { return 0.875 + 0.125 * height; }
constexpr double kMaxSpawnHeight = (kJuggernautScaleMax - 0.875) / 0.125;   // 5.8: the largest Height a spawn writes
constexpr int8_t kTeamSpecial = 0, kTeamRest = 1;       // Riot's Knight and Juggernaut's Juggernauts / everybody else

struct FighterRole {
    uint8_t id = kRoleFighter;
    uint8_t tier = 2;                                    // the gear's Enum_Ranks tier
    Gear gear = Gear::Own;
    WeaponFilter weapons = WeaponFilter::Any;
    double weight = kRoleUnchanged;                      // the passport Weight forced at the spawn (0..1); kRoleUnchanged: as without a role
    double height = kRoleUnchanged;                      // the passport Height at the spawn (0..1); kRoleUnchanged: as without a role
    int8_t team = kNoTeam;                               // kNoTeam: ResolveTeams decides (every format without roles)
    bool operator==(const FighterRole&) const = default;
};

inline uint8_t RioterRoleId(int tier) { return uint8_t(kRoleRioterFirst + std::clamp<int>(tier, kRioterMinTier, kRioterMaxTier) - kRioterMinTier); }

// The role an id stands for under `cfg` (the lobby's settings, which only the Fighter reads). An unknown id is the Fighter.
// The team is left to the dealer (AssignRoles).
inline FighterRole RoleSpec(uint8_t id, const MatchConfig& cfg) {
    FighterRole r;
    if (id == kRoleKnight) {
        r.id = id; r.tier = kKnightTier; r.gear = Gear::Mirror; r.weight = TierWeight(kKnightTier);
    } else if (id >= kRoleRioterFirst && id <= kRoleRioterLast) {
        r.id = id; r.tier = uint8_t(id - kRoleRioterFirst + kRioterMinTier); r.gear = Gear::RandomTier; r.weight = TierWeight(r.tier);
    } else if (id == kRoleJuggernaut) {
        r.id = id; r.tier = kJuggernautTier; r.gear = Gear::RandomTier; r.weight = kJuggernautWeight; r.height = kJuggernautHeight;
    } else if (id == kRoleSkirmisher) {
        r.id = id; r.tier = kSkirmisherTier; r.gear = Gear::RandomTier; r.weight = kSkirmisherWeight; r.height = kSkirmisherHeight;
    } else {
        r.tier = cfg.tier; r.gear = cfg.gear; r.weapons = cfg.weapons;
    }
    return r;
}

// How many Juggernauts a Juggernaut match deals for `players` fighters: 3..5 one, 6..8 two, 9 three; never all of them.
inline int JuggernautCount(int players) {
    if (players <= 1) return 0;
    return players <= 5 ? 1 : players <= 8 ? 2 : 3;
}

namespace roles_detail {
inline uint32_t Mix(uint32_t x) {                        // murmur3 finaliser
    x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;
    return x;
}
inline FighterRole Dealt(uint8_t id, const MatchConfig& cfg, int8_t team) { FighterRole r = RoleSpec(id, cfg); r.team = team; return r; }
// A Riot Rioter's tier, 1..4, spread by the seed, the rematch and the fighter's id (so it does not move when others come and go).
inline uint8_t RioterTier(uint32_t seed, uint32_t rematch, uint8_t id) {
    return uint8_t(kRioterMinTier + Mix(Mix(seed ^ (rematch * 0x9E3779B9u)) + id) % (kRioterMaxTier - kRioterMinTier + 1));
}
}  // namespace roles_detail

// The role of a fighter who joins a running match: never a special one. Riot: a Rioter, Juggernaut: a Skirmisher (both on the
// rest's team), else the lobby's.
inline FighterRole LateJoinerRole(Format f, const MatchConfig& cfg, uint8_t id, uint32_t seed, uint32_t rematch = 0) {
    using namespace roles_detail;
    if (f == Format::Riot) return Dealt(RioterRoleId(RioterTier(seed, rematch, id)), cfg, kTeamRest);
    if (f == Format::Juggernaut) return Dealt(kRoleSkirmisher, cfg, kTeamRest);
    return Dealt(kRoleFighter, cfg, kNoTeam);
}

// The roles for a match's fighters (their entities; each id once), by role id. `seed` stays the same for a lobby's matches,
// `rematch` counts them from 0: the Knight (or the Juggernauts) move on to the next fighters by id at every rematch, so after
// that many rematches everybody has had a turn. Every format without roles: everyone the lobby's role, team left to
// ResolveTeams. The same arguments always deal the same roles.
inline std::map<uint8_t, FighterRole> AssignRoles(Format f, const MatchConfig& cfg, const std::vector<uint8_t>& fighters, uint32_t seed, uint32_t rematch = 0) {
    using namespace roles_detail;
    std::vector<uint8_t> ids = fighters;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::map<uint8_t, FighterRole> out;
    const size_t n = ids.size();
    if (!HasRoles(f)) {
        for (uint8_t id : ids) out[id] = LateJoinerRole(f, cfg, id, seed, rematch);
        return out;
    }
    if (n == 0) return out;
    const size_t special = f == Format::Riot ? 1 : size_t(JuggernautCount(int(n)));
    const size_t first = (Mix(seed) + size_t(rematch) * special) % n;      // the special roles go to `special` fighters in a row from here, wrapping
    for (size_t i = 0; i < n; ++i) {
        const bool chosen = (i + n - first) % n < special;
        if (f == Format::Riot)
            out[ids[i]] = chosen ? Dealt(kRoleKnight, cfg, kTeamSpecial) : LateJoinerRole(f, cfg, ids[i], seed, rematch);
        else
            out[ids[i]] = Dealt(chosen ? kRoleJuggernaut : kRoleSkirmisher, cfg, chosen ? kTeamSpecial : kTeamRest);
    }
    return out;
}

// The Crossbows row hands a crossbow to ordinary fighters only: the Knight's and the Juggernaut's kits are balanced as dealt, so the
// special roles never hold one (the Rioters, the Skirmishers and the lobby's Fighters may).
inline bool RoleMayHoldCrossbow(uint8_t id) { return id != kRoleKnight && id != kRoleJuggernaut; }
// The Crossbows draw over `fighters` with their role ids (`roleOf`: entity -> id; an entity not in it is a Fighter): only the fighters
// that may hold one are drawn from, so "Half" is half of them and "All" is all of them.
inline std::vector<uint8_t> PickCrossbowHoldersByRole(uint8_t mode, const std::vector<uint8_t>& fighters, const std::map<uint8_t, uint8_t>& roleOf, uint32_t seed) {
    std::vector<uint8_t> eligible;
    for (uint8_t e : fighters) { auto it = roleOf.find(e); if (RoleMayHoldCrossbow(it == roleOf.end() ? kRoleFighter : it->second)) eligible.push_back(e); }
    return PickCrossbowHolders(mode, std::move(eligible), seed);
}

// What the HUD calls a role: the scoreboard tag (the special roles only), the line at the spawn, and the log's name.
inline const char* RoleTag(uint8_t id) { return id == kRoleKnight ? "KNIGHT" : id == kRoleJuggernaut ? "JUGGERNAUT" : ""; }
inline std::string RoleLine(uint8_t id) {
    if (id == kRoleKnight) return "YOU ARE THE KNIGHT";
    if (id == kRoleJuggernaut) return "YOU ARE A JUGGERNAUT";
    if (id == kRoleSkirmisher) return "YOU ARE A SKIRMISHER";
    if (id >= kRoleRioterFirst && id <= kRoleRioterLast) return "YOU ARE A RIOTER";
    return {};
}
inline const char* RoleName(uint8_t id) {
    if (id == kRoleKnight) return "knight";
    if (id == kRoleJuggernaut) return "juggernaut";
    if (id == kRoleSkirmisher) return "skirmisher";
    return id >= kRoleRioterFirst && id <= kRoleRioterLast ? "rioter" : "fighter";
}

}  // namespace hsm

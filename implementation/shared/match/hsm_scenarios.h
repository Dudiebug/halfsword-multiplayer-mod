// Combat scenarios per map (0.9.0, lobby row Scenario in RULES > WORLD, lobby key hsm_scen): what the host's pick does to the
// match on the map it picked. Every machine reads the pick from the lobby (hsm_scen), so no wire field is needed.
//
// The game's own Free Mode has BP_CombatEvent_{Alley,Cellar,Forest,Hall,Pit,Slums,Yard,Tourney,TourneyBaron}_C, each with a
// Scenario Name, Available Combat Modes, Available Lose Conditions and a Combatants Amount. The game's Free Mode switches for
// Carnage ("Free Mode Carnage": Current Combat Mode 3) and Brawling ("Free Mode Brawling": Start in Pants) only steer the game's
// own computer-fighter spawners (decomp BP_LevelManager Clean_Up_and_Spawn_Actors, BP_SpawnerPoint_Willies), and a lobby match
// turns those off (Session::StopStockFoes), so setting them did nothing (public issue #1407). The mod does the effects itself:
//   Standard      nothing: the host's Free Mode setup stays as it is (today's behaviour).
//   Fenced ring   "Current Lose Condition" = 3 on the game instance (ScenarioWorld, carried by the Travel). The level scripts
//                 of the Pit, Yard and Cellar spawn BP_Fence_Bags for it (audit item 39); no other map reads it, so only
//                 those three offer it. Its behaviour is unchanged; the DLL only logs what the game does (session.cpp).
//   Carnage       the host adds 3 to 5 computer fighters (CarnageBotCount) at the start of a free-for-all round, on no team,
//                 as Survival's host bots (ScenarioEffects::carnageBots). They score nothing.
//   Brawling      every body spawns bare: no armour, no weapons (ScenarioEffects::bare), as the Brawl card's Fists gear does.
// The time of day (Day Time / Current FreeMode Time) is left out on purpose: its enum values are unknown (audit 0.7.0 #2).
// The Abyss (Zombies) and modded maps are not combat-event arenas and offer Standard only. The client logs the game's actual
// combat-event objects at each level load ("scenario: map ..."), the spike output this table is extended from.
// Game-independent and unit-tested (test_hsm_scenarios.cpp).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

enum class Scenario : uint8_t { Standard, FencedRing, Carnage, Brawling };
constexpr uint8_t kScenarioCount = 4;
inline bool ValidScenario(uint8_t s) { return s < kScenarioCount; }

// The setting a scenario sets on the game instance before the travel (the Travel carries it to every client). loseCondition <
// 0: left as it is. Carnage and Brawling set nothing here: the game's switches for them do nothing in a lobby match.
struct ScenarioWorld {
    int loseCondition = -1;     // "Current Lose Condition" (Enum_CombatEvent_LoseConditions)
};
constexpr int kLoseConditionFence = 3;   // BP_Fence_Bags in the Pit, Yard and Cellar (travel.cpp)

inline ScenarioWorld WorldOf(Scenario s) {
    ScenarioWorld w;
    if (s == Scenario::FencedRing) w.loseCondition = kLoseConditionFence;
    return w;
}

// What a match format lets a scenario do. Plain bools: hsm_match.h includes this header, so the Format enum is not known here
// (ScenarioFormatOf in hsm_match.h fills it).
struct ScenarioFormat {
    bool freeForAll = true;     // plain free-for-all: every fighter for himself
    bool botFormat = false;     // Survival, Zombies: the host's wave bots fight the players
};
// false: the scenario does nothing in this format and the match plays as Standard (the row says so).
inline bool ScenarioApplies(Scenario s, const ScenarioFormat& f) {
    switch (s) {
        case Scenario::Carnage: return f.freeForAll;
        case Scenario::Brawling: return !f.botFormat;
        case Scenario::FencedRing:
        case Scenario::Standard: break;
    }
    return true;
}

// Carnage's computer fighters: one more than the players, 3 to 5.
constexpr int kCarnageBotsMin = 3, kCarnageBotsMax = 5;
inline int CarnageBotCount(int players) { return std::clamp(players + 1, kCarnageBotsMin, kCarnageBotsMax); }

// What the mod itself does for a scenario in a match of `players` humans; the DLL only applies it.
struct ScenarioEffects {
    bool bare = false;          // every body spawns with no armour and no weapons (Brawling)
    int carnageBots = 0;        // computer fighters the host adds when the round goes live, on no team (Carnage); 0 = none
};
inline ScenarioEffects EffectsOf(Scenario s, const ScenarioFormat& f, int players) {
    ScenarioEffects e;
    if (!ScenarioApplies(s, f)) return e;
    if (s == Scenario::Brawling) e.bare = true;
    if (s == Scenario::Carnage) e.carnageBots = CarnageBotCount(players);
    return e;
}

inline const char* ScenarioName(Scenario s) {
    switch (s) {
        case Scenario::FencedRing: return "Fenced ring";
        case Scenario::Carnage: return "Carnage";
        case Scenario::Brawling: return "Brawling";
        case Scenario::Standard: break;
    }
    return "Standard";
}
// What the row says a scenario does. It only promises what the mod does (EffectsOf); the Fenced ring is the game's own fence and
// lose condition, which a test run is still checking.
inline const char* ScenarioHint(Scenario s) {
    switch (s) {
        case Scenario::FencedRing: return "Sandbag fences close the ring. Experimental: leaving the ring should put you out.";
        case Scenario::Carnage: return "3 to 5 computer fighters join the arena and attack everyone, you included. They score nothing. Free-for-all only.";
        case Scenario::Brawling: return "Everyone starts every round in pants with no weapons. Not used in Survival or Zombies.";
        case Scenario::Standard: break;
    }
    return "The map as the game's Free Mode setup has it.";
}
// The row's hint for a format: where the scenario does nothing, it says so instead of promising its effect.
inline std::string ScenarioHintFor(Scenario s, const ScenarioFormat& f) {
    if (ScenarioApplies(s, f)) return ScenarioHint(s);
    return s == Scenario::Carnage ? "Carnage works in Free-for-all only. In this mode the match plays as Standard."
                                  : "Brawling does not work in Survival or Zombies. Here the match plays as Standard.";
}

namespace scenario_detail {
constexpr uint8_t Bit(Scenario s) { return uint8_t(1u << uint8_t(s)); }
constexpr uint8_t kBase = Bit(Scenario::Standard) | Bit(Scenario::Carnage) | Bit(Scenario::Brawling);   // every arena
constexpr uint8_t kFenced = kBase | Bit(Scenario::FencedRing);                                           // arenas whose script builds the fence
struct MapScenarios { const char* shortName; uint8_t mask; };
// The arenas of the lobby's map picker (hsm_maps.h kMaps), by the last segment of the package path.
constexpr MapScenarios kTable[] = {
    {"Map_Arena_Alley", kBase},
    {"Map_Arena_Cellar", kFenced},
    {"Map_Arena_EastTower", kBase},
    {"Map_Arena_LordsHall", kBase},
    {"Map_Arena_Pit", kFenced},
    {"Map_Arena_Slums", kBase},
    {"Map_Arena_Yard", kFenced},
};
inline std::string_view Tail(std::string_view path) {
    const size_t dot = path.rfind('.');                       // a soft object path: ".../Map_Arena_Yard.Map_Arena_Yard"
    if (dot != std::string_view::npos) path = path.substr(0, dot);
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}
inline bool SameNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x + 32);
        if (y >= 'A' && y <= 'Z') y = char(y + 32);
        if (x != y) return false;
    }
    return true;
}
inline uint8_t MaskOf(std::string_view mapPath) {
    const std::string_view t = Tail(mapPath);
    for (const MapScenarios& m : kTable) if (SameNoCase(m.shortName, t)) return m.mask;
    return Bit(Scenario::Standard);                           // the Abyss, a modded map, an unknown path
}
}  // namespace scenario_detail

// The scenarios a map offers, Standard first, in enum order. An empty or unknown path: Standard only.
inline std::vector<Scenario> ScenariosFor(std::string_view mapPath) {
    const uint8_t mask = scenario_detail::MaskOf(mapPath);
    std::vector<Scenario> out;
    for (uint8_t i = 0; i < kScenarioCount; ++i) if (mask & scenario_detail::Bit(Scenario(i))) out.push_back(Scenario(i));
    return out;
}

// The scenario that really runs: the host's pick when the map offers it, else Standard (a pick left over from another map,
// an out-of-range value from a lobby key).
inline Scenario EffectiveScenario(std::string_view mapPath, uint8_t pick) {
    if (!ValidScenario(pick)) return Scenario::Standard;
    return (scenario_detail::MaskOf(mapPath) & scenario_detail::Bit(Scenario(pick))) ? Scenario(pick) : Scenario::Standard;
}

// The row's stepper: the next (dir >= 0) or previous scenario the map offers, wrapping; from what runs now.
inline uint8_t StepScenario(std::string_view mapPath, uint8_t pick, int dir) {
    const std::vector<Scenario> all = ScenariosFor(mapPath);
    const Scenario cur = EffectiveScenario(mapPath, pick);
    size_t at = 0;
    for (size_t i = 0; i < all.size(); ++i) if (all[i] == cur) at = i;
    at = (at + (dir < 0 ? all.size() - 1 : 1)) % all.size();
    return uint8_t(all[at]);
}

// Where a map's own level script builds the Fenced ring (BP_Fence_Bags at this location, with this Radius; decomp
// Maps/Arenas/Map_Arena_{Pit,Yard,Cellar}.cpp ReceiveBeginPlay). The script only does it at Play Mode 0, and a session
// fight runs at Play Mode 1 (Adapter::PrepareFreeModeFight), so the mod builds the same fence itself on every machine
// (pre24 tester feedback, #706). false: the map has no fence of its own.
struct FencePlacement { double x = 0, y = 0, z = 0, radius = 0; };
namespace scenario_detail {
struct FenceRow { const char* shortName; FencePlacement p; };
inline constexpr FenceRow kFenceRows[] = {
    {"Map_Arena_Pit", {5.9424, -19.991, -5.5181, 525}},
    {"Map_Arena_Yard", {5.9424, -19.991, -5.0, 525}},
    {"Map_Arena_Cellar", {724, 709, 2, 400}},
};
}  // namespace scenario_detail
inline bool FencePlacementFor(std::string_view mapPath, FencePlacement& out) {
    const std::string_view t = scenario_detail::Tail(mapPath);
    for (const auto& r : scenario_detail::kFenceRows) if (scenario_detail::SameNoCase(r.shortName, t)) { out = r.p; return true; }
    return false;
}

// The bags BP_Fence_Bags builds round a ring of this Radius (cm): Round(2 * pi * Radius * 0.01 / 0.8), one every 0.8 m of the
// circle (decomp Assets/Props/Fence/Blueprints/BP_Fence_Bags.cpp). The DLL logs it beside what it counted, so a short fence shows.
inline int FenceBagsFor(double radius) {
    if (!(radius > 0)) return 0;
    return int(std::floor(2.0 * 3.14159265358979323846 * radius * 0.01 / 0.8 + 0.5));
}
// Outside the ring's lose volume: BP_Fence_Bags' Sphere has the Radius * 0.95 and its EndOverlap calls "Event Lose Match" on the
// fighter that left. (x, y, z) in cm, the level's own coordinates.
constexpr double kFenceSphereScale = 0.95;
inline bool OutsideFence(const FencePlacement& p, double x, double y, double z) {
    const double dx = x - p.x, dy = y - p.y, dz = z - p.z, r = p.radius * kFenceSphereScale;
    return dx * dx + dy * dy + dz * dz > r * r;
}

// The lobby key hsm_scen: the scenario's number as text. A value that is no number or no scenario reads Standard.
inline uint8_t ScenarioFromKey(std::string_view v) {
    if (v.empty() || v.size() > 3) return 0;
    unsigned n = 0;
    for (char c : v) { if (c < '0' || c > '9') return 0; n = n * 10 + unsigned(c - '0'); }
    return n < kScenarioCount ? uint8_t(n) : uint8_t(0);
}

// One log line's worth of the table, for a map: "Standard, Fenced ring, Carnage, Brawling".
inline std::vector<const char*> ScenarioNamesFor(std::string_view mapPath) {
    std::vector<const char*> out;
    for (Scenario s : ScenariosFor(mapPath)) out.push_back(ScenarioName(s));
    return out;
}

}  // namespace hsm

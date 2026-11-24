// Combat scenarios per map (lobby key hsm_scen): what the host's pick does to the match on the map it picked. Every machine
// reads the pick from the lobby (hsm_scen), so no wire field is needed. The lobby has no Scenario row since the modes merge
// (0.9.0): Brawling is the Brawl card and Blossfechten is a mode card of its own (IsBlossfechten, hsm_match.h: the Last man
// standing rounds with scenario 4). The numbers stay as they were, because a host on an older build may still advertise any of
// them (hsm_lobby_scene.h BrowserModeFor, lobby.cpp Lobby::config read them).
//
// The game's own Free Mode has BP_CombatEvent_{Alley,Cellar,Forest,Hall,Pit,Slums,Yard,Tourney,TourneyBaron}_C, each with a
// Scenario Name, Available Combat Modes, Available Lose Conditions and a Combatants Amount. The game's Free Mode switches for
// Carnage ("Free Mode Carnage": Current Combat Mode 3) and Brawling ("Free Mode Brawling": Start in Pants) only steer the game's
// own computer-fighter spawners (decomp BP_LevelManager Clean_Up_and_Spawn_Actors, BP_SpawnerPoint_Willies), and a lobby match
// turns those off (Session::StopStockFoes), so setting them did nothing (public issue #1407). The mod does the effects itself:
//   Standard      nothing: the host's Free Mode setup stays as it is (today's behaviour).
//   Fenced ring   RETIRED. It set "Current Lose Condition" = 3 on the game instance; the level scripts of the Pit, Yard and
//                 Cellar spawn BP_Fence_Bags for it. No map offers it any more (EffectiveScenario reads it as Standard), but a
//                 client still builds the fence when the Travel of an older host carries lose condition 3 (the fence helpers below).
//   Carnage       RETIRED. The host added 3 to 5 computer fighters to a free-for-all round. Reads as Standard.
//   Brawling      every body spawns bare: no armour, no weapons (ScenarioEffects::bare), as the Brawl card's Fists gear does.
//                 No row sets it any more; a lobby of an older host that does is still played that way.
//   Blossfechten  swords only, no armour (ScenarioEffects::blossfechten). Every body spawns with the Willie flag "Blossfechten
//                 Gear", set before the body finishes spawning, as a career Blossfechten fight (combat mode 10,
//                 session/hsm_career.h) does: no armour, the right hand only. The game swaps the weapon for a sword only
//                 outside Free Mode (decomp Willie_BP), and a lobby match is Free Mode, so the host deals the swords itself
//                 (BlossfechtenKit, hsm_match.h). The game's "Free Mode Blossfechten" switch only steers its own spawners,
//                 like Carnage's and Brawling's. A bare body (Brawl, Tag, Brawling, a hider) stays bare. The Blossfechten
//                 mode is offered on the arenas below (ScenariosFor).
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

enum class Scenario : uint8_t { Standard, FencedRing, Carnage, Brawling, Blossfechten };
constexpr uint8_t kScenarioCount = 5;
inline bool ValidScenario(uint8_t s) { return s < kScenarioCount; }

// The setting a scenario sets on the game instance before the travel (the Travel carries it to every client). loseCondition <
// 0: left as it is, which is every scenario now that the Fenced ring is retired: the game's switches for the others do nothing
// in a lobby match.
struct ScenarioWorld {
    int loseCondition = -1;     // "Current Lose Condition" (Enum_CombatEvent_LoseConditions)
};
constexpr int kLoseConditionFence = 3;   // BP_Fence_Bags in the Pit, Yard and Cellar: what an older host's Fenced ring travels with (travel.cpp)

inline ScenarioWorld WorldOf(Scenario) { return {}; }

// What a match format lets a scenario do. Plain bools: hsm_match.h includes this header, so the Format enum is not known here
// (ScenarioFormatOf in hsm_match.h fills it).
struct ScenarioFormat {
    bool freeForAll = true;     // plain free-for-all: every fighter for himself
    bool botFormat = false;     // Survival, Zombies: the host's wave bots fight the players
};
// false: the scenario does nothing in this format and the match plays as Standard (the row says so).
inline bool ScenarioApplies(Scenario s, const ScenarioFormat& f) {
    switch (s) {
        case Scenario::Brawling:
        case Scenario::Blossfechten: return !f.botFormat;
        case Scenario::Carnage:
        case Scenario::FencedRing:
        case Scenario::Standard: break;
    }
    return true;
}

// What the mod itself does for a scenario; the DLL only applies it. (Carnage's computer fighters are gone with the Carnage scenario.)
struct ScenarioEffects {
    bool bare = false;          // every body spawns with no armour and no weapons (Brawling)
    bool blossfechten = false;  // swords only, no armour: the Blossfechten Gear flag on every body, a sword dealt to each (Blossfechten)
};
inline ScenarioEffects EffectsOf(Scenario s, const ScenarioFormat& f) {
    ScenarioEffects e;
    if (!ScenarioApplies(s, f)) return e;
    if (s == Scenario::Brawling) e.bare = true;
    if (s == Scenario::Blossfechten) e.blossfechten = true;
    return e;
}

inline const char* ScenarioName(Scenario s) {
    switch (s) {
        case Scenario::FencedRing: return "Fenced ring";
        case Scenario::Carnage: return "Carnage";
        case Scenario::Brawling: return "Brawling";
        case Scenario::Blossfechten: return "Blossfechten";
        case Scenario::Standard: break;
    }
    return "Standard";
}

namespace scenario_detail {
constexpr uint8_t Bit(Scenario s) { return uint8_t(1u << uint8_t(s)); }
// Every arena offers these. Carnage and the Fenced ring are retired (no map offers them, so a pick of either reads Standard).
constexpr uint8_t kBase = Bit(Scenario::Standard) | Bit(Scenario::Brawling) | Bit(Scenario::Blossfechten);
struct MapScenarios { const char* shortName; uint8_t mask; };
// The arenas of the lobby's map picker (hsm_maps.h kMaps), by the last segment of the package path.
constexpr MapScenarios kTable[] = {
    {"Map_Arena_Alley", kBase},
    {"Map_Arena_Cellar", kBase},
    {"Map_Arena_EastTower", kBase},
    {"Map_Arena_LordsHall", kBase},
    {"Map_Arena_Pit", kBase},
    {"Map_Arena_Slums", kBase},
    {"Map_Arena_Yard", kBase},
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

// Whether a map offers the Blossfechten mode: the arenas of the table above. The mode card's map list (hsm_maps.h) asks this
// instead of keeping a second list.
inline bool OffersBlossfechten(std::string_view mapPath) {
    return (scenario_detail::MaskOf(mapPath) & scenario_detail::Bit(Scenario::Blossfechten)) != 0;
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

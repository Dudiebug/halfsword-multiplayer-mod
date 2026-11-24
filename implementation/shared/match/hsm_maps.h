// Map picker catalog: the persistent levels a match can be started on, and which formats each one supports.
// Paths are the package paths OpenLevel takes (kHubLevel in implementation/client/HSMMod/src/gameplay/session.cpp).
// Every entry is a top-level .umap under HalfswordUE5/Content/Maps in re/out/pak_index.json. Sublevels, level
// instances, VFX and lighting layers, menus and Map_Test_Empty are not listed.
//   arena    - a fight arena: every format (Tournament fights only here)
//   bigArena - a large non-menu level: every format except Tournament
//   abyss    - the Abyss level: Zombies and Infection, and a Crypts battle that takes large levels (MapAllowsMatch)
//   pick     - listed in the lobby's picker; Cutting, Tavern and Smithery are not (the Tavern is Progression's)
//   crypts   - the Crypts map (docs/PLAN_CRYPTS_COOP.md): the game's whole procedural dungeon, Map_Procedural_Block_Test_Main.
//              The two Crypts modes (Grave Robbers, Crypt Royale; Gear::Crypts) are played on it and nowhere else, and no other
//              mode may pick it. The fight after a Crypt Royale run is on another map, the Fight map (MapAllowsFight).
#pragma once
#include "match/hsm_match.h"
#include <cctype>
#include <cstddef>
#include <string_view>

namespace hsm {

struct MapInfo {
    const char* path;      // package path, e.g. "/Game/Maps/Arenas/Map_Arena_Yard"
    const char* display;   // label for the picker
    bool arena;
    bool bigArena;
    bool abyss;
    bool pick;             // offered in the lobby's map picker (the rest stay known to FindMap/MapAllowsFormat)
    bool crypts;           // the Crypts map: the Crypts modes' map and only theirs (MapAllowsMatch)
};

// Order is the cycling order of NextMap.
inline constexpr MapInfo kMaps[] = {
    {"/Game/Maps/Arenas/Map_Arena_Alley",       "Alley",      true,  false, false, true,  false},
    {"/Game/Maps/Arenas/Map_Arena_Cellar",      "Cellar",     true,  false, false, true,  false},
    {"/Game/Maps/Arenas/Map_Arena_EastTower",   "East Tower", true,  false, false, true,  false},
    {"/Game/Maps/Arenas/Map_Arena_LordsHall",   "Lords Hall", true,  false, false, true,  false},
    {"/Game/Maps/Arenas/Map_Arena_Pit",         "Pit",        true,  false, false, true,  false},
    {"/Game/Maps/Arenas/Map_Arena_Slums",       "Slums",      true,  false, false, true,  false},
    {"/Game/Maps/Arenas/Map_Arena_Yard",        "Yard",       true,  false, false, true,  false},
    {"/Game/Maps/Arena_Cutting_Map",            "Cutting",    true,  false, false, false, false},
    {"/Game/Maps/Map_Hub_Tavern_Frank",         "Tavern",     false, true,  false, false, false},
    {"/Game/Maps/Workshop_Smithery_Map",        "Smithery",   false, true,  false, false, false},
    // Forest: the Ambush arena (it has a BP_CombatEvent_Forest_C); an arena like the others, offered to every arena format.
    {"/Game/Maps/Arenas/Map_Arena_Ambush_Test", "Forest",     true,  false, false, true,  false},
    {"/Game/Maps/Abyss/Abyss_Map_Open_EA",      "Abyss",      false, false, true,  true,  false},
    // The Crypts: the game's whole dungeon. The one map of Grave Robbers and Crypt Royale (the Crypt Royale fight is on the Fight map).
    {"/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main", "Crypts", false, false, false, true, true},
};

constexpr size_t MapCount() { return sizeof(kMaps) / sizeof(kMaps[0]); }

// Out-of-range index returns the first map; check MapCount() first.
inline const MapInfo& MapAt(size_t i) { return kMaps[i < MapCount() ? i : 0]; }

namespace map_detail {
inline bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
inline std::string_view ShortName(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}
// "/Game/Maps/Arenas/Map_Arena_Yard.Map_Arena_Yard" (soft object path) -> "/Game/Maps/Arenas/Map_Arena_Yard"
inline std::string_view StripObjectName(std::string_view s) {
    const size_t dot = s.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return s;
    if (!EqualsNoCase(s.substr(dot + 1), ShortName(s.substr(0, dot)))) return s;
    return s.substr(0, dot);
}
}  // namespace map_detail

// Full package path or short name ("Map_Arena_Yard"), case-insensitive. nullptr if unknown.
inline const MapInfo* FindMap(std::string_view pathOrShortName) {
    const std::string_view name = map_detail::StripObjectName(pathOrShortName);
    if (name.empty()) return nullptr;
    const bool full = name.find('/') != std::string_view::npos;
    for (const MapInfo& m : kMaps) {
        const std::string_view p = m.path;
        const bool hit = full ? map_detail::EqualsNoCase(p, name)
                              : map_detail::EqualsNoCase(map_detail::ShortName(p), name);
        if (hit) return &m;
    }
    return nullptr;
}

// The arenas whose lighting follows the time of day (the Time of day row): their level Blueprint sets "Day Time" from GI
// "Current FreeMode Time" and streams the lighting level for it (decomp Map_Arena_Alley, _EastTower, _Ambush_Test (Forest),
// _Pit, _Slums, _Yard). The Cellar and the Lords Hall have one lighting; the Abyss and a modded map are not arenas of the game's.
inline bool MapHasDayTime(std::string_view pathOrShortName) {
    const MapInfo* m = FindMap(pathOrShortName);
    if (!m) return false;
    for (const char* p : {"/Game/Maps/Arenas/Map_Arena_Alley", "/Game/Maps/Arenas/Map_Arena_EastTower", "/Game/Maps/Arenas/Map_Arena_Ambush_Test",
                          "/Game/Maps/Arenas/Map_Arena_Pit", "/Game/Maps/Arenas/Map_Arena_Slums", "/Game/Maps/Arenas/Map_Arena_Yard"})
        if (std::string_view(m->path) == p) return true;
    return false;
}

// Pit Bridge Duels (no lobby card since 0.9.0, an older host can still run them) are fought on the Pit's bridge and nowhere else.
inline constexpr const char* kPitMapPath = "/Game/Maps/Arenas/Map_Arena_Pit";
inline constexpr const char* kSlumsMapPath = "/Game/Maps/Arenas/Map_Arena_Slums";   // Hide and Seek's only stock map (custom maps are every format's)

inline bool MapAllowsFormat(const MapInfo& m, Format f) {
    if (m.crypts) return false;   // the Crypts map is for the Crypts modes alone (MapAllowsMatch)
    switch (f) {
        case Format::Zombies: return m.abyss;
        case Format::Infection: return m.arena || m.bigArena || m.abyss;
        case Format::PitBridgeDuel: return map_detail::EqualsNoCase(m.path, kPitMapPath);
        case Format::HideSeek: return map_detail::EqualsNoCase(m.path, kSlumsMapPath);
        case Format::Tournament: return m.arena;
        case Format::CaptureTheFlag: return false;   // removed in 0.8.0 (Sanitize turns it into Teams)
        case Format::FreeForAll:
        case Format::LastManStanding:
        case Format::Duel:   // one card with Last man standing since 0.9.0: the same maps (the arenas only before)
        case Format::Chaos:
        case Format::OneCrossbow:
        case Format::Roulette:
        case Format::TwoTeams:
        case Format::TeamDuel:
        case Format::Riot:
        case Format::Juggernaut:
        case Format::Tag:
        case Format::CrossbowTDM:
        case Format::ThreeTeams:
        case Format::PlayersVsBots: return m.arena || m.bigArena;
    }
    return false;
}

// A match's maps: the format's own, except that the Crypts modes (Gear::Crypts) are played on the Crypts map and nowhere
// else, and no other mode may pick it. `f` is a Crypt Royale's Battle format (the fight itself is on the Fight map,
// MapAllowsFight). The Blossfechten card (IsBlossfechten, hsm_match.h) fights only on the arenas that offer the scenario
// (OffersBlossfechten, hsm_scenarios.h: Alley, Cellar, East Tower, Lords Hall, Pit, Slums, Yard).
inline bool MapAllowsMatch(const MapInfo& m, Format f, Gear g, bool blossfechten = false) {
    if (m.crypts || g == Gear::Crypts) return m.crypts && g == Gear::Crypts && CryptsBattleFormat(f) && !blossfechten;
    if (blossfechten) return MapAllowsFormat(m, f) && OffersBlossfechten(m.path);
    return MapAllowsFormat(m, f);
}

// The Fight map of a Crypt Royale (the fight after the loot run): the maps the Battle format fights on, which for a battle
// format that takes large levels includes the Abyss (#3199; Tournament stays in the arenas). Never the Crypts map.
inline bool MapAllowsFight(const MapInfo& m, Format f) {
    if (m.crypts) return false;
    return MapAllowsFormat(m, f) || (m.abyss && CryptsBattleFormat(f) && f != Format::Tournament);
}

// Next map after `current` (wrapping) that a match of format `f` with gear `g` may fight on (MapAllowsMatch); `current`
// itself if it is the only one. Returns MapCount() when no map allows it.
inline size_t NextMap(size_t current, Format f, Gear g, bool blossfechten = false) {
    const size_t n = MapCount();
    for (size_t step = 1; step <= n; ++step) {
        const size_t i = (current % n + step) % n;
        if (kMaps[i].pick && MapAllowsMatch(kMaps[i], f, g, blossfechten)) return i;
    }
    return n;
}
// The same for the format alone (own gear: MapAllowsFormat).
inline size_t NextMap(size_t current, Format f) { return NextMap(current, f, Gear::Own); }
// The Fight map's default (the Fight map row, lobby key hsm_fmap): the Abyss where the Battle format may fight there, else the
// format's first map. MapCount() when no map allows it.
inline size_t CryptsDefaultMap(Format f) {
    for (size_t i = 0; i < MapCount(); ++i) if (kMaps[i].pick && kMaps[i].abyss && MapAllowsFight(kMaps[i], f)) return i;
    for (size_t i = 0; i < MapCount(); ++i) if (kMaps[i].pick && MapAllowsFight(kMaps[i], f)) return i;
    return MapCount();
}
// The Crypts map's index in kMaps (MapCount() if the catalog had none).
inline size_t CryptsMapIndex() {
    for (size_t i = 0; i < MapCount(); ++i) if (kMaps[i].crypts) return i;
    return MapCount();
}

}  // namespace hsm

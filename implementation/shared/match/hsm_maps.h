// Map picker catalog: the persistent levels a match can be started on, and which formats each one supports.
// Paths are the package paths OpenLevel takes (kHubLevel in implementation/client/HSMMod/src/gameplay/session.cpp).
// Every entry is a top-level .umap under HalfswordUE5/Content/Maps in re/out/pak_index.json. Sublevels, level
// instances, VFX and lighting layers, menus and Map_Test_Empty are not listed.
//   arena    - a fight arena: Duel, and every other format
//   bigArena - a large non-menu level: every format except Duel
//   abyss    - the Abyss level: Zombies only (its picker entry; the owner's call)
//   pick     - listed in the lobby's picker; Cutting, Tavern and Smithery are not (the Tavern is Progression's)
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
};

// Order is the cycling order of NextMap.
inline constexpr MapInfo kMaps[] = {
    {"/Game/Maps/Arenas/Map_Arena_Alley",       "Alley",      true,  false, false, true},
    {"/Game/Maps/Arenas/Map_Arena_Cellar",      "Cellar",     true,  false, false, true},
    {"/Game/Maps/Arenas/Map_Arena_EastTower",   "East Tower", true,  false, false, true},
    {"/Game/Maps/Arenas/Map_Arena_LordsHall",   "Lords Hall", true,  false, false, true},
    {"/Game/Maps/Arenas/Map_Arena_Pit",         "Pit",        true,  false, false, true},
    {"/Game/Maps/Arenas/Map_Arena_Slums",       "Slums",      true,  false, false, true},
    {"/Game/Maps/Arenas/Map_Arena_Yard",        "Yard",       true,  false, false, true},
    {"/Game/Maps/Arena_Cutting_Map",            "Cutting",    true,  false, false, false},
    {"/Game/Maps/Map_Hub_Tavern_Frank",         "Tavern",     false, true,  false, false},
    {"/Game/Maps/Workshop_Smithery_Map",        "Smithery",   false, true,  false, false},
    {"/Game/Maps/Abyss/Abyss_Map_Open_EA",      "Abyss",      false, false, true,  true},
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

inline bool MapAllowsFormat(const MapInfo& m, Format f) {
    switch (f) {
        case Format::Zombies: return m.abyss;
        case Format::Duel: return m.arena;
        case Format::FreeForAll:
        case Format::TwoTeams:
        case Format::ThreeTeams:
        case Format::PlayersVsBots: return m.arena || m.bigArena;
    }
    return false;
}

// Next map after `current` (wrapping) that allows `f`; `current` itself if it is the only one.
// Returns MapCount() when no map allows `f`.
inline size_t NextMap(size_t current, Format f) {
    const size_t n = MapCount();
    for (size_t step = 1; step <= n; ++step) {
        const size_t i = (current % n + step) % n;
        if (kMaps[i].pick && MapAllowsFormat(kMaps[i], f)) return i;
    }
    return n;
}

}  // namespace hsm

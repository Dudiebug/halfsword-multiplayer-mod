// The multiplayer page's map list (ui/menu.cpp): one row per catalog map (kMaps order), shown only when the match
// format allows that map, and the map a match would start on marked selected.
#pragma once
#include "match/hsm_maps.h"
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

struct MapRow {
    size_t index = 0;          // into kMaps
    bool shown = false;        // the format allows it
    bool selected = false;     // the map Start would open
    std::string label;         // "> Name <" when selected
};

// The map a match of format f starts on: the host's pick when f allows it, else the first map that does.
// MapCount() when no map allows f.
inline size_t EffectiveMap(Format f, std::string_view pick) {
    if (const MapInfo* m = FindMap(pick); m && m->pick && MapAllowsFormat(*m, f)) return size_t(m - kMaps);
    return NextMap(MapCount() - 1, f);
}

inline std::vector<MapRow> MapRows(Format f, std::string_view pick) {
    const size_t chosen = EffectiveMap(f, pick);
    std::vector<MapRow> rows(MapCount());
    for (size_t i = 0; i < rows.size(); ++i) {
        MapRow& r = rows[i];
        r.index = i;
        r.shown = kMaps[i].pick && MapAllowsFormat(kMaps[i], f);
        r.selected = i == chosen;
        r.label = r.selected ? std::string("> ") + kMaps[i].display + " <" : std::string(kMaps[i].display);
    }
    return rows;
}

}  // namespace hsm

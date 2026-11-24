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

// The map a match of format f with gear g starts on: the host's pick when the match may be played there (MapAllowsMatch: the
// Crypts modes on the Crypts map alone; the Blossfechten card only in its arenas), else the first map that allows it. MapCount()
// when no map does.
inline size_t EffectiveMap(Format f, Gear g, std::string_view pick, bool blossfechten = false) {
    if (const MapInfo* m = FindMap(pick); m && m->pick && MapAllowsMatch(*m, f, g, blossfechten)) return size_t(m - kMaps);
    return NextMap(MapCount() - 1, f, g, blossfechten);
}
// The same for the format alone (own gear).
inline size_t EffectiveMap(Format f, std::string_view pick) { return EffectiveMap(f, Gear::Own, pick); }

// The Fight map of a Crypt Royale (the Fight map row, lobby key hsm_fmap): the host's pick while the Battle format may fight there
// (MapAllowsFight: the Abyss too, never the Crypts map), else CryptsDefaultMap (the Abyss where it fits). MapCount() when no map does.
inline size_t EffectiveFightMap(Format f, std::string_view pick) {
    if (const MapInfo* m = FindMap(pick); m && m->pick && MapAllowsFight(*m, f)) return size_t(m - kMaps);
    return CryptsDefaultMap(f);
}

// The map a Crypts gather's battle travels to (#3199): the Fight map picked at START while the battle's format still allows it,
// else the one the format fights on now (the host may change the Battle row during the gather). A level that is not a catalog
// map is kept as it is (no rule to check it against).
inline std::string BattleMapFor(Format f, const std::string& path) {
    const MapInfo* m = FindMap(path);
    if (!m || (m->pick && MapAllowsFight(*m, f))) return path;
    const size_t i = CryptsDefaultMap(f);
    return i < MapCount() ? std::string(kMaps[i].path) : path;
}

// The battle map of a Crypts rematch's new gather: the Fight map the lobby's page shows picked now, as START would take it (the
// host may pick another map or Battle row during a battle), else the last battle's map (no pick, or a modded map's).
inline std::string RematchBattleMap(Format f, std::string_view fightPick, const std::string& last) {
    if (!FindMap(fightPick)) return last;
    const size_t i = EffectiveFightMap(f, fightPick);
    return i < MapCount() ? std::string(kMaps[i].path) : last;
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

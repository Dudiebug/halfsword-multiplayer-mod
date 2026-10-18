// The multiplayer page's map list: one row per catalog map, shown only when the mode allows it, and the map a match
// would start on (the host's pick when the mode allows it, else the first map that does) marked selected.
#include "match/hsm_map_list.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static size_t Idx(const char* display) {
    for (size_t i = 0; i < MapCount(); ++i) if (std::string(MapAt(i).display) == display) return i;
    return MapCount();
}
static int Selected(const std::vector<MapRow>& rows) {
    int n = 0;
    for (const MapRow& r : rows) n += r.selected ? 1 : 0;
    return n;
}
static size_t Shown(const std::vector<MapRow>& rows) {
    size_t n = 0;
    for (const MapRow& r : rows) n += r.shown ? 1 : 0;
    return n;
}

static void TestEffective() {
    CHECK(EffectiveMap(Format::Duel, "/Game/Maps/Arenas/Map_Arena_Yard") == Idx("Yard"));
    CHECK(EffectiveMap(Format::Duel, "map_arena_pit") == Idx("Pit"));                      // short name, any case
    CHECK(EffectiveMap(Format::Duel, "") == Idx("Alley"));                                 // no pick: first allowed
    CHECK(EffectiveMap(Format::Duel, "/Game/Maps/Abyss/Abyss_Map_Open_EA") == Idx("Alley"));  // pick not allowed
    CHECK(EffectiveMap(Format::Zombies, "") == Idx("Abyss"));
    CHECK(EffectiveMap(Format::Zombies, "/Game/Maps/Arenas/Map_Arena_Yard") == Idx("Abyss"));
    CHECK(EffectiveMap(Format::FreeForAll, "/Game/Maps/Map_Hub_Tavern_Frank") == Idx("Tavern"));
    CHECK(EffectiveMap(Format::FreeForAll, "nonsense") == Idx("Alley"));
}

static void TestRows() {
    std::vector<MapRow> d = MapRows(Format::Duel, "/Game/Maps/Arenas/Map_Arena_Yard");
    CHECK(d.size() == MapCount());
    for (size_t i = 0; i < d.size(); ++i) CHECK(d[i].index == i);
    CHECK(Shown(d) == 8);                                               // the 8 arenas
    CHECK(!d[Idx("Tavern")].shown && !d[Idx("Smithery")].shown && !d[Idx("Abyss")].shown);
    CHECK(Selected(d) == 1 && d[Idx("Yard")].selected);
    CHECK(d[Idx("Yard")].label == "> Yard <");
    CHECK(d[Idx("Pit")].label == "Pit");

    std::vector<MapRow> z = MapRows(Format::Zombies, "");
    CHECK(Shown(z) == 1 && z[Idx("Abyss")].shown && z[Idx("Abyss")].selected && Selected(z) == 1);

    std::vector<MapRow> f = MapRows(Format::FreeForAll, "/Game/Maps/Map_Hub_Tavern_Frank");
    CHECK(Shown(f) == MapCount());
    CHECK(Selected(f) == 1 && f[Idx("Tavern")].selected && f[Idx("Tavern")].label == "> Tavern <");
    CHECK(f[Idx("Alley")].label == "Alley");

    std::vector<MapRow> none = MapRows(Format::Duel, "/Game/Maps/Abyss/Abyss_Map_Open_EA");
    CHECK(Selected(none) == 1 && none[Idx("Alley")].selected);         // what Start would use
    for (const MapRow& r : none) CHECK(!r.selected || r.shown);
}

int main() {
    TestEffective();
    TestRows();
    if (g_fail) { std::printf("test_hsm_map_list: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_map_list: all passed\n");
    return 0;
}

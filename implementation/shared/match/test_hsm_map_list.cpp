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
    CHECK(EffectiveMap(Format::FreeForAll, "/Game/Maps/Map_Hub_Tavern_Frank") == Idx("Alley"));   // not pickable
    CHECK(EffectiveMap(Format::FreeForAll, "nonsense") == Idx("Alley"));
}

static void TestRows() {
    std::vector<MapRow> d = MapRows(Format::Duel, "/Game/Maps/Arenas/Map_Arena_Yard");
    CHECK(d.size() == MapCount());
    for (size_t i = 0; i < d.size(); ++i) CHECK(d[i].index == i);
    CHECK(Shown(d) == 8);                                               // Duel is the Last man standing card with two players (0.9.0): its maps; the Crypts map is the Crypts modes'
    CHECK(!d[Idx("Crypts")].shown);
    CHECK(!d[Idx("Tavern")].shown && !d[Idx("Smithery")].shown && !d[Idx("Abyss")].shown);
    CHECK(Selected(d) == 1 && d[Idx("Yard")].selected);
    CHECK(d[Idx("Yard")].label == "> Yard <");
    CHECK(d[Idx("Pit")].label == "Pit");

    std::vector<MapRow> z = MapRows(Format::Zombies, "");
    CHECK(Shown(z) == 1 && z[Idx("Abyss")].shown && z[Idx("Abyss")].selected && Selected(z) == 1);

    std::vector<MapRow> f = MapRows(Format::FreeForAll, "/Game/Maps/Map_Hub_Tavern_Frank");
    CHECK(Shown(f) == 8);                                              // the pickable arenas, Forest too; the Abyss is for Zombies and Infection
    CHECK(!f[Idx("Crypts")].shown);
    CHECK(Selected(f) == 1 && f[Idx("Alley")].selected && f[Idx("Alley")].label == "> Alley <");
    CHECK(!f[Idx("Tavern")].shown && !f[Idx("Cutting")].shown && !f[Idx("Abyss")].shown);

    std::vector<MapRow> none = MapRows(Format::Duel, "/Game/Maps/Abyss/Abyss_Map_Open_EA");
    CHECK(Selected(none) == 1 && none[Idx("Alley")].selected);         // what Start would use
    for (const MapRow& r : none) CHECK(!r.selected || r.shown);
}

// The Fight map at the end of a Crypts gather: still allowed for the battle format, or the format's default (the Abyss where it fits).
static void TestBattleMap() {
    const std::string abyss = "/Game/Maps/Abyss/Abyss_Map_Open_EA", yard = "/Game/Maps/Arenas/Map_Arena_Yard";
    CHECK(BattleMapFor(Format::LastManStanding, abyss) == abyss);
    CHECK(BattleMapFor(Format::Duel, abyss) == abyss);   // Duel follows Last man standing: the Abyss stays
    CHECK(BattleMapFor(Format::Tournament, abyss) == MapAt(CryptsDefaultMap(Format::Tournament)).path && kMaps[CryptsDefaultMap(Format::Tournament)].arena);   // arenas only
    CHECK(BattleMapFor(Format::Duel, yard) == yard);
    CHECK(BattleMapFor(Format::Duel, "/Game/Maps/Somewhere/Map_Custom") == "/Game/Maps/Somewhere/Map_Custom");
    CHECK(BattleMapFor(Format::Duel, "/Game/Maps/Map_Hub_Tavern_Frank") == abyss);   // known, not pickable: the default
    const std::string smithery = "/Game/Maps/Workshop_Smithery_Map";                       // allowed for the format, still not pickable
    CHECK(BattleMapFor(Format::LastManStanding, smithery) == abyss);
    CHECK(BattleMapFor(Format::LastManStanding, kMaps[CryptsMapIndex()].path) == abyss);   // the Crypts map is never where the fight is
    // The gearless helpers are the Own-gear ones.
    for (size_t i = 0; i < MapCount(); ++i) CHECK(NextMap(i, Format::Duel) == NextMap(i, Format::Duel, Gear::Own));
    CHECK(EffectiveMap(Format::Zombies, yard) == EffectiveMap(Format::Zombies, Gear::Own, yard));
    // The Fight map row's value: the pick, or the default.
    CHECK(EffectiveFightMap(Format::LastManStanding, yard) == Idx("Yard") && EffectiveFightMap(Format::LastManStanding, "") == Idx("Abyss"));
    CHECK(EffectiveFightMap(Format::Tournament, abyss) != Idx("Abyss") && EffectiveFightMap(Format::PlayersVsBots, "nonsense") == Idx("Abyss"));
}

// A Crypts rematch's gather: the battle goes where the page shows the Fight map now, not where the last one was fought.
static void TestRematchMap() {
    const std::string abyss = "/Game/Maps/Abyss/Abyss_Map_Open_EA", yard = "/Game/Maps/Arenas/Map_Arena_Yard";
    const std::string pit = "/Game/Maps/Arenas/Map_Arena_Pit";
    CHECK(RematchBattleMap(Format::LastManStanding, yard, abyss) == yard);     // picked during the battle
    CHECK(RematchBattleMap(Format::LastManStanding, abyss, yard) == abyss);
    CHECK(RematchBattleMap(Format::Duel, pit, yard) == pit);
    CHECK(RematchBattleMap(Format::Duel, abyss, pit) == abyss);                // Duel takes the Abyss, like Last man standing
    CHECK(RematchBattleMap(Format::Tournament, abyss, pit) == MapAt(CryptsDefaultMap(Format::Tournament)).path);   // as START: arenas only
    CHECK(RematchBattleMap(Format::LastManStanding, "", pit) == pit);         // no pick: the last battle's map
    CHECK(RematchBattleMap(Format::Duel, "/Game/Mods/Arena/Map_Custom", pit) == pit);   // a modded pick
    CHECK(RematchBattleMap(Format::Duel, kMaps[CryptsMapIndex()].path, pit) == abyss);   // the Crypts map is no fight map: the default
}

int main() {
    TestEffective();
    TestBattleMap();
    TestRematchMap();
    TestRows();
    if (g_fail) { std::printf("test_hsm_map_list: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_map_list: all passed\n");
    return 0;
}

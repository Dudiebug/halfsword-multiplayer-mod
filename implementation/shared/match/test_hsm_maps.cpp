// Map picker catalog: lookup by path and short name, format filtering, cycling.
#include "match/hsm_maps.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static size_t IndexOf(const MapInfo* m) { return m ? size_t(m - &kMaps[0]) : MapCount(); }
static size_t IndexOfName(const char* name) { return IndexOf(FindMap(name)); }

static void TestCatalog() {
    CHECK(MapCount() == 11);
    std::set<std::string> paths, shorts;
    for (size_t i = 0; i < MapCount(); ++i) {
        const MapInfo& m = MapAt(i);
        std::string p = m.path;
        CHECK(p.rfind("/Game/Maps/", 0) == 0);                       // package path form OpenLevel takes
        CHECK(p.find("Menu") == std::string::npos);                   // menus are not picker maps
        CHECK(p.find("Test_Empty") == std::string::npos);
        CHECK(p.find('.') == std::string::npos);                      // no object suffix
        CHECK(m.display[0] != '\0');
        CHECK(m.arena || m.bigArena || m.abyss);                      // every map supports something
        CHECK(paths.insert(p).second);
        CHECK(shorts.insert(std::string(p.substr(p.rfind('/') + 1))).second);
    }
}

static void TestFindByPath() {
    const MapInfo* yard = FindMap("/Game/Maps/Arenas/Map_Arena_Yard");
    CHECK(yard && std::strcmp(yard->path, "/Game/Maps/Arenas/Map_Arena_Yard") == 0);
    CHECK(yard && std::strcmp(yard->display, "Yard") == 0 && yard->arena && !yard->bigArena && !yard->abyss);
    CHECK(FindMap("/game/maps/arenas/map_arena_YARD") == yard);   // case-insensitive
    CHECK(FindMap("/Game/Maps/Arenas/Map_Arena_Yard.Map_Arena_Yard") == yard);   // soft object path
    CHECK(FindMap("/Game/Maps/Abyss/Abyss_Map_Open_EA") == FindMap("Abyss_Map_Open_EA"));
    CHECK(FindMap("/Game/Maps/Map_Hub_Tavern_Frank") == FindMap("Map_Hub_Tavern_Frank"));
    CHECK(FindMap("/Game/Maps/Arena_Cutting_Map") == FindMap("arena_cutting_map"));
}

static void TestFindByShortName() {
    const MapInfo* yard = FindMap("Map_Arena_Yard");
    CHECK(yard && std::strcmp(yard->path, "/Game/Maps/Arenas/Map_Arena_Yard") == 0);
    CHECK(FindMap("map_arena_yard") == yard);
    CHECK(FindMap("MAP_ARENA_YARD") == yard);
    CHECK(FindMap("Map_Arena_Yard.Map_Arena_Yard") == yard);
    CHECK(FindMap("Workshop_Smithery_Map") && std::strcmp(FindMap("Workshop_Smithery_Map")->display, "Smithery") == 0);
    CHECK(FindMap("Map_Arena_Ambush_Test") == nullptr);                       // test map: excluded
}

static void TestFindUnknown() {
    CHECK(FindMap("") == nullptr);
    CHECK(FindMap("Map_Arena_Nowhere") == nullptr);
    CHECK(FindMap("/Game/Maps/Arenas/Map_Arena_Nowhere") == nullptr);
    CHECK(FindMap("Map_Arena_Yar") == nullptr);                       // prefix is not a match
    CHECK(FindMap("Map_Arena_Yard/") == nullptr);
    CHECK(FindMap("Maps/Arenas/Map_Arena_Yard") == nullptr);          // must be the full package path
    CHECK(FindMap("Map_Menu_Startup") == nullptr);                    // menus are not in the catalog
    CHECK(FindMap("Map_Test_Empty") == nullptr);
    CHECK(FindMap("Map_Arena_Yard.umap") == nullptr);
}

static void TestFormatFilter() {
    const MapInfo& yard = *FindMap("Map_Arena_Yard");
    const MapInfo& hub = *FindMap("Map_Hub_Tavern_Frank");
    const MapInfo& abyss = *FindMap("Abyss_Map_Open_EA");
    const MapInfo& cutting = *FindMap("Arena_Cutting_Map");

    // Arenas: everything, Duel included.
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams, Format::PlayersVsBots, Format::Duel})
        CHECK(MapAllowsFormat(yard, f));
    CHECK(!MapAllowsFormat(yard, Format::Zombies));
    CHECK(MapAllowsFormat(cutting, Format::Duel));

    // Big non-menu levels: every format except Duel, Zombies only on the abyss.
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams, Format::PlayersVsBots})
        CHECK(MapAllowsFormat(hub, f));
    CHECK(!MapAllowsFormat(hub, Format::Duel));
    CHECK(!MapAllowsFormat(hub, Format::Zombies));

    // Abyss: Zombies, plus the big-level formats; not Duel.
    CHECK(MapAllowsFormat(abyss, Format::Zombies));
    CHECK(MapAllowsFormat(abyss, Format::PlayersVsBots));
    CHECK(MapAllowsFormat(abyss, Format::FreeForAll));
    CHECK(!MapAllowsFormat(abyss, Format::Duel));

    // Every catalog entry supports at least one format, and only abyss maps take Zombies.
    size_t zombie = 0;
    for (size_t i = 0; i < MapCount(); ++i) {
        const MapInfo& m = MapAt(i);
        if (MapAllowsFormat(m, Format::Zombies)) ++zombie;
        CHECK(MapAllowsFormat(m, Format::Zombies) == m.abyss);
        CHECK(MapAllowsFormat(m, Format::FreeForAll) == (m.arena || m.bigArena));
    }
    CHECK(zombie == 1);
    CHECK(!MapAllowsFormat(MapAt(0), Format(200)));                   // out-of-range enum allows nothing
}

static void TestNextMapCycles() {
    // Walking every map in the catalog cycles back to the start, and the allowed set is visited in order.
    const size_t n = MapCount();
    size_t cur = 0;
    for (size_t step = 0; step < n; ++step) cur = NextMap(cur, Format::FreeForAll);
    CHECK(cur == 0);                                                  // wraps back to the start
    const size_t lastIdx = n - 1;
    CHECK(NextMap(lastIdx, Format::FreeForAll) == 0);                 // wrap-around at the end

    // Cycle with Duel from a non-arena map: the first arena after it.
    const size_t hub = IndexOfName("Map_Hub_Tavern_Frank");
    const size_t duelNext = NextMap(hub, Format::Duel);
    CHECK(duelNext < n && MapAllowsFormat(MapAt(duelNext), Format::Duel));
    CHECK(duelNext == 0);                                             // wraps past the big levels to Alley

    // Duel skips hub, workshop and abyss; the full Duel cycle is every arena once.
    std::set<size_t> seen;
    cur = 0;
    for (size_t step = 0; step < n; ++step) {
        cur = NextMap(cur, Format::Duel);
        CHECK(cur < n && MapAllowsFormat(MapAt(cur), Format::Duel));
        seen.insert(cur);
    }
    size_t arenas = 0;
    for (size_t i = 0; i < n; ++i) if (MapAt(i).arena) ++arenas;
    CHECK(seen.size() == arenas);

    // Zombies: the only allowed map cycles to itself.
    const size_t abyss = IndexOfName("Abyss_Map_Open_EA");
    CHECK(NextMap(abyss, Format::Zombies) == abyss);
    CHECK(NextMap(0, Format::Zombies) == abyss);
    CHECK(NextMap(n - 1, Format::Zombies) == abyss);

    // Skips disallowed maps going forward: from Cutting (index 8) the next team-capable map is the hub (9).
    CHECK(NextMap(IndexOfName("Arena_Cutting_Map"), Format::TwoTeams) == hub);
    CHECK(NextMap(IndexOfName("Abyss_Map_Open_EA"), Format::Duel) == 0);

    // Out-of-range current index still lands on a valid allowed map.
    CHECK(NextMap(n + 3, Format::Duel) < n);
    CHECK(MapAllowsFormat(MapAt(NextMap(n + 3, Format::Duel)), Format::Duel));
}

int main() {
    TestCatalog();
    TestFindByPath();
    TestFindByShortName();
    TestFindUnknown();
    TestFormatFilter();
    TestNextMapCycles();
    if (g_fail) { std::printf("test_hsm_maps: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_maps: all passed\n");
    return 0;
}

// The picker's catalog: only pickable maps are offered or cycled to; the Abyss is Zombies only.
#include "match/hsm_map_list.h"
#include "match/hsm_maps.h"
#include <cstdio>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

int main() {
    CHECK(!FindMap("Arena_Cutting_Map")->pick && !FindMap("Map_Hub_Tavern_Frank")->pick && !FindMap("Workshop_Smithery_Map")->pick);
    CHECK(FindMap("Abyss_Map_Open_EA")->pick && FindMap("Map_Arena_Yard")->pick);
    // The unpickable maps stay known (Progression travels to the Tavern by path).
    CHECK(FindMap("Map_Hub_Tavern_Frank") != nullptr);

    const Format all[] = {Format::Duel, Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams, Format::PlayersVsBots, Format::Zombies};
    for (Format f : all) {
        size_t cur = MapCount() - 1;
        for (size_t step = 0; step < MapCount() + 2; ++step) {
            cur = NextMap(cur, f);
            CHECK(cur < MapCount() && MapAt(cur).pick && MapAllowsFormat(MapAt(cur), f));
        }
        const size_t eff = EffectiveMap(f, "/Game/Maps/Map_Hub_Tavern_Frank");
        CHECK(eff < MapCount() && MapAt(eff).pick);
        CHECK((MapAt(eff).abyss) == (f == Format::Zombies));
        for (const MapRow& r : MapRows(f, "")) CHECK(!r.shown || r.selected || MapAt(r.index).pick);
    }
    CHECK(EffectiveMap(Format::FreeForAll, "Abyss_Map_Open_EA") != size_t(FindMap("Abyss_Map_Open_EA") - kMaps));
    CHECK(EffectiveMap(Format::Zombies, "Map_Arena_Yard") == size_t(FindMap("Abyss_Map_Open_EA") - kMaps));
    if (!g_fail) std::printf("test_hsm_map_pick: all passed\n");
    return g_fail ? 1 : 0;
}

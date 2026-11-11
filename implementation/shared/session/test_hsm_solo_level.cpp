// Unit tests for hsm_solo_level.h. Run via implementation/shared/run_tests.sh test_hsm_solo_level.
#include "session/hsm_solo_level.h"
#include "session/hsm_stock_maps.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

// The map the Seer and the start menu open, as a full path (the travel hook's name) and as the world's short name.
static void TestDungeonIsSolo() {
    CHECK(IsSoloLevel("/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main"));
    CHECK(IsSoloLevel("Map_Procedural_Block_Test_Main"));
    CHECK(IsSoloLevel("/Game/Maps/Test/ProceduralBlocksTest/Dungeon_Probe"));   // the folder alone is enough
    CHECK(IsSoloLevel("/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Hall_1_Arena_Logic"));
}

// Every arena a lobby can play, and the menus and the Tavern, stay multiplayer levels.
static void TestOtherLevelsAreNot() {
    CHECK(!IsSoloLevel(""));
    CHECK(!IsSoloLevel("/Game/Maps/Map_Hub_Tavern_Frank"));
    CHECK(!IsSoloLevel("/Game/Maps/Menus/Map_Menu_Startup"));
    CHECK(!IsSoloLevel("Arena_Cutting_Map"));
    CHECK(!IsSoloLevel("Abyss_Map_Open_EA"));
}

// The stock pak: only the procedural block test maps are solo, and every one of those is.
static void TestStockMaps() {
    int solo = 0;
    for (const char* p : kStockMapPaths) {
        const bool procedural = std::strstr(p, "ProceduralBlocksTest") != nullptr;
        CHECK(IsSoloLevel(p) == procedural);
        solo += IsSoloLevel(p);
    }
    CHECK(solo > 0);
}

int main() {
    TestDungeonIsSolo();
    TestOtherLevelsAreNot();
    TestStockMaps();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_solo_level: all tests passed\n");
    return 0;
}

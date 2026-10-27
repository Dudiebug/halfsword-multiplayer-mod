// Unit tests for the travel gate (hsm_travel_gate.h). Run via implementation/shared/run_tests.sh test_hsm_travel_gate.
#include "session/hsm_travel_gate.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestCatalog() {
    for (const MapInfo& m : kMaps) {
        CHECK(TravelAllowed(m.path, ""));
        CHECK(TravelAllowed(std::string(m.path) + "." + std::string(map_detail::ShortName(m.path)), ""));
        CHECK(TravelAllowed(map_detail::ShortName(m.path), ""));
    }
    CHECK(TravelAllowed("/Game/Maps/Map_Hub_Tavern_Frank", ""));
    CHECK(TravelAllowed("/game/maps/arenas/map_arena_yard", ""));   // case-insensitive, like FindMap
    CHECK(TravelAllowed(kTravelMenuLevel, ""));
    CHECK(TravelAllowed("/Game/Maps/Menus/Map_Menu_Startup.Map_Menu_Startup", ""));
    CHECK(TravelAllowed("/Game/Maps/Menus/Map_Menu_Other", ""));
}

static void TestLevelRefused() {
    CHECK(!TravelAllowed("", ""));
    CHECK(!TravelAllowed("listen", ""));
    CHECK(!TravelAllowed("1.2.3.4:7777", ""));
    CHECK(!TravelAllowed("1.2.3.4", ""));
    CHECK(!TravelAllowed("//host/share/Map", ""));
    CHECK(!TravelAllowed(R"(\\host\share\Map)", ""));
    CHECK(!TravelAllowed("/Game/Maps/Arenas/Map_Arena_Yard?listen", ""));
    CHECK(!TravelAllowed("/Game/Maps/Arenas/../Menus/Map_Menu_Startup", ""));
    CHECK(!TravelAllowed("/Game/Maps/Arenas/Map_Arena_Nope", ""));
    CHECK(!TravelAllowed("/Game/Maps/Menus/Map_Menu_", ""));
    CHECK(!TravelAllowed("/Game/Maps/Menus/Map_Menu_A/../../x", ""));
    CHECK(!TravelAllowed("/Game/Maps/Menus/Map_Menu_A.", ""));
    CHECK(!TravelAllowed("/Game/Maps/Menus/Map_Menu_A b", ""));
    CHECK(!TravelAllowed(std::string("/Game/Maps/Arenas/Map_Arena_Yard\0x", 35), ""));   // embedded NUL
    CHECK(!TravelAllowed("/Game/Maps/Arenas/Map_Arena_Yard\n", ""));
    CHECK(!TravelAllowed("/Game/Maps/Arenas/Map_Arena_Yard\x7f", ""));
    CHECK(!TravelAllowed("/Game/Maps/Arenas/Map_Arena_Yard\xc3\xa9", ""));
    CHECK(!TravelAllowed(std::string(kMaxLevelName + 1, 'a'), ""));
    CHECK(!TravelAllowed(std::string("/Game/Maps/Menus/Map_Menu_") + std::string(300, 'a'), ""));
}

static void TestOptions() {
    const char* yard = "/Game/Maps/Arenas/Map_Arena_Yard";
    CHECK(TravelAllowed(yard, ""));
    CHECK(!TravelAllowed(yard, "listen"));
    CHECK(!TravelAllowed(yard, "?listen"));
    CHECK(!TravelAllowed(yard, "?LISTEN"));
    CHECK(!TravelAllowed(yard, "?game=/Game/X.X_C"));
    CHECK(!TravelAllowed(yard, "?Game=/Game/X.X_C"));
    CHECK(!TravelAllowed(yard, "?a=1.2.3.4:7777"));
    CHECK(!TravelAllowed(yard, "?a=//x"));
    CHECK(!TravelAllowed(yard, "?a=\x01"));
    CHECK(!TravelAllowed(yard, std::string(kMaxLevelName + 1, '?')));
    CHECK(!TravelAllowed(yard, "?x"));    // an unlisted key
    CHECK(TravelAllowed(yard, "?&"));     // separators alone carry no key
}

static void TestSettings() {
    const uint8_t b0[] = {0}, b1[] = {1}, b2[] = {2}, e16[] = {16}, e65[] = {65}, e255[] = {255};
    CHECK(SettingValueOk(SettingKind::Bool, b0, 1) && SettingValueOk(SettingKind::Bool, b1, 1));
    CHECK(!SettingValueOk(SettingKind::Bool, b2, 1));
    CHECK(!SettingValueOk(SettingKind::Bool, b1, 2));
    CHECK(SettingValueOk(SettingKind::Byte, e16, 1) && !SettingValueOk(SettingKind::Byte, e65, 1) && !SettingValueOk(SettingKind::Byte, e255, 1));
    float f = 1.5f; uint8_t fb[4]; std::memcpy(fb, &f, 4);
    CHECK(SettingValueOk(SettingKind::Float, fb, 4));
    CHECK(!SettingValueOk(SettingKind::Float, fb, 8));
    f = std::numeric_limits<float>::quiet_NaN(); std::memcpy(fb, &f, 4);
    CHECK(!SettingValueOk(SettingKind::Float, fb, 4));
    f = std::numeric_limits<float>::infinity(); std::memcpy(fb, &f, 4);
    CHECK(!SettingValueOk(SettingKind::Float, fb, 4));
    f = -std::numeric_limits<float>::infinity(); std::memcpy(fb, &f, 4);
    CHECK(!SettingValueOk(SettingKind::Float, fb, 4));
    f = 0.0f; std::memcpy(fb, &f, 4);
    CHECK(SettingValueOk(SettingKind::Float, fb, 4));
    double d = 2.5; uint8_t db[8]; std::memcpy(db, &d, 8);
    CHECK(SettingValueOk(SettingKind::Double, db, 8) && !SettingValueOk(SettingKind::Double, db, 4));
    d = std::numeric_limits<double>::quiet_NaN(); std::memcpy(db, &d, 8);
    CHECK(!SettingValueOk(SettingKind::Double, db, 8));
    d = std::numeric_limits<double>::infinity(); std::memcpy(db, &d, 8);
    CHECK(!SettingValueOk(SettingKind::Double, db, 8));
    const uint8_t i4[4] = {1, 2, 3, 4};
    CHECK(SettingValueOk(SettingKind::Int, i4, 4) && !SettingValueOk(SettingKind::Int, i4, 3));
}

int main() {
    TestCatalog(); TestLevelRefused(); TestOptions(); TestSettings();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_travel_gate: ok\n");
    return 0;
}

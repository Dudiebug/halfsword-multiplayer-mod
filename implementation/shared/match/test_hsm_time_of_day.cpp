// The Time of day row (#2): the arenas with a day cycle, the game's Enum_DayTime byte for each pick, the lobby row.
#include "match/hsm_lobby_scene.h"
#include "match/hsm_match_impl.h"
#include <cstdio>
#include <optional>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static LobbyLocal Lobby(const char* map, bool host = true, Format f = Format::FreeForAll) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host;
    l.config = SelectFormat(l.config, f);
    l.mapPick = map;
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = host;
    l.players.push_back(me);
    return l;
}
static std::optional<SettingRow> Find(const LobbyScene& s, LobbyRow id) {   // a copy: the scene is often a temporary
    for (const SettingRow& r : s.settings) if (r.id == id) return r;   // never: the row is kept apart, like the Scenario
    if (id == LobbyRow::TimeOfDay && s.hasTimeOfDay) return s.timeOfDay;
    return std::nullopt;
}

int main() {
    // The maps: the six arenas whose level Blueprint reads Current FreeMode Time, and no other.
    for (const char* m : {"Alley", "EastTower", "Ambush_Test", "Pit", "Slums", "Yard"}) {
        const std::string path = std::string("/Game/Maps/Arenas/Map_Arena_") + m;
        CHECK(MapHasDayTime(path));
        CHECK(MapHasDayTime(path + ".Map_Arena_" + m));        // a soft object path
        CHECK(MapHasDayTime(std::string("Map_Arena_") + m));   // a short name
    }
    CHECK(MapHasDayTime("/game/maps/arenas/map_arena_pit"));
    for (const char* m : {"/Game/Maps/Arenas/Map_Arena_Cellar", "/Game/Maps/Arenas/Map_Arena_LordsHall", "/Game/Maps/Abyss/Abyss_Map_Open_EA",
                          "/Game/Maps/Map_Hub_Tavern_Frank", "/Game/Mods/Cool/Map_Arena_Pit", "", "x"})
        CHECK(!MapHasDayTime(m));

    // The game's byte: morning 0 ... night 3; the map's own and anything past the last set nothing.
    CHECK(GameDayTime(uint8_t(TimeOfDay::Map)) == -1);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Morning)) == 0);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Day)) == 1);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Evening)) == 2);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Night)) == 3);
    CHECK(GameDayTime(5) == -1 && GameDayTime(255) == -1);
    CHECK(std::string(TimeOfDayName(4)) == "Night" && std::string(TimeOfDayName(1)) == "Morning");
    CHECK(std::string(TimeOfDayName(0)) == "Map's own" && std::string(TimeOfDayName(9)) == "Map's own");

    // Sanitize: a stale or peer-written value is the map's own; every real pick stays.
    {
        MatchConfig c;
        c.timeOfDay = 7;
        CHECK(Sanitize(c).timeOfDay == 0);
        c.timeOfDay = kTimeOfDayCount;                                   // one past Night
        CHECK(Sanitize(c).timeOfDay == 0);
        for (uint8_t t = 0; t < kTimeOfDayCount; ++t) { c.timeOfDay = t; CHECK(Sanitize(c).timeOfDay == t); }
    }

    // The row: in WORLD, only on a map with a day cycle, the host's to step: Map's own, Morning ... Night, round again.
    {
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Pit");
        std::optional<SettingRow> r = Find(BuildLobbyScene(l), LobbyRow::TimeOfDay);
        CHECK(r && r->enabled && r->value == "Map's own" && r->label == "Time of day" && r->id == LobbyRow::TimeOfDay);
        const LobbyScene s = BuildLobbyScene(l);
        for (const SettingRow& x : s.settings) CHECK(x.id != LobbyRow::TimeOfDay);
        CHECK(s.settings.size() == BuildLobbyScene(Lobby("/Game/Maps/Arenas/Map_Arena_Cellar")).settings.size());   // the match rows are the same on any map
        CHECK(SectionOf(LobbyRow::TimeOfDay) == LobbySection::World);
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, 1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Morning));
        r = Find(BuildLobbyScene(l), LobbyRow::TimeOfDay);
        CHECK(r && r->value == "Morning" && r->hint.find("for everyone") != std::string::npos);
        for (int i = 0; i < 3; ++i) l.config = StepSetting(l.config, LobbyRow::TimeOfDay, 1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Night));
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, 1);
        CHECK(l.config.timeOfDay == 0);                            // round again to the map's own
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, -1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Night));
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, -1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Evening));
    }
    for (const char* m : {"/Game/Maps/Arenas/Map_Arena_Cellar", "/Game/Maps/Arenas/Map_Arena_LordsHall"})
        CHECK(!Find(BuildLobbyScene(Lobby(m)), LobbyRow::TimeOfDay));
    {
        std::optional<SettingRow> r = Find(BuildLobbyScene(Lobby("/Game/Maps/Arenas/Map_Arena_Yard", false)), LobbyRow::TimeOfDay);
        CHECK(r && !r->enabled);   // a client sees it, greyed
    }
    {
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Pit");
        l.progression = true;
        CHECK(!Find(BuildLobbyScene(l), LobbyRow::TimeOfDay));   // Progression plays the Tavern's own day
    }

    if (g_fail) { std::printf("test_hsm_time_of_day: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_time_of_day: ok\n");
    return 0;
}

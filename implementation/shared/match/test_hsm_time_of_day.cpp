// The Time of day row (#2): the arenas with a day cycle, the game's Enum_DayTime byte for each pick, the lobby row. Morning, Midday,
// Evening, Night, Midday for a new lobby; an older host's Map's own (0) reads as Midday.
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
    for (const SettingRow& r : s.settings) if (r.id == id) return r;
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

    // The game's byte: morning 0, midday 1, evening 2, night 3. There is no "Map's own" any more: 0 (an older host's Map's own) and
    // anything past Night read as Midday, so a pick is always set.
    CHECK(GameDayTime(uint8_t(TimeOfDay::Morning)) == 0);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Midday)) == 1);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Evening)) == 2);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Night)) == 3);
    CHECK(GameDayTime(uint8_t(TimeOfDay::Retired)) == 1 && GameDayTime(0) == 1);   // Map's own: Midday
    CHECK(GameDayTime(5) == 1 && GameDayTime(255) == 1);
    CHECK(std::string(TimeOfDayName(4)) == "Night" && std::string(TimeOfDayName(1)) == "Morning");
    CHECK(std::string(TimeOfDayName(2)) == "Midday" && std::string(TimeOfDayName(3)) == "Evening");
    CHECK(std::string(TimeOfDayName(0)) == "Midday" && std::string(TimeOfDayName(9)) == "Midday");   // no "Map's own" in any text
    for (int t = 0; t < 256; ++t) CHECK(std::string(TimeOfDayName(uint8_t(t))).find("Map") == std::string::npos);
    CHECK(TimeOfDayRead(0) == 2 && TimeOfDayRead(1) == 1 && TimeOfDayRead(2) == 2 && TimeOfDayRead(3) == 3 && TimeOfDayRead(4) == 4);
    CHECK(TimeOfDayRead(5) == 2 && TimeOfDayRead(200) == 2);
    CHECK(MatchConfig{}.timeOfDay == uint8_t(TimeOfDay::Midday));                    // a new lobby starts at Midday

    // Sanitize: a stale or peer-written value, and an older host's Map's own, is Midday; every real pick stays.
    {
        MatchConfig c;
        c.timeOfDay = 7;
        CHECK(Sanitize(c).timeOfDay == uint8_t(TimeOfDay::Midday));
        c.timeOfDay = kTimeOfDayCount;                                   // one past Night
        CHECK(Sanitize(c).timeOfDay == uint8_t(TimeOfDay::Midday));
        c.timeOfDay = 0;                                                 // an older host's Map's own
        CHECK(Sanitize(c).timeOfDay == uint8_t(TimeOfDay::Midday));
        for (uint8_t t = 1; t < kTimeOfDayCount; ++t) { c.timeOfDay = t; CHECK(Sanitize(c).timeOfDay == t); }
    }

    // The row: in WORLD, only on a map with a day cycle, the host's to step: Morning, Midday, Evening, Night, round again.
    {
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Pit");
        std::optional<SettingRow> r = Find(BuildLobbyScene(l), LobbyRow::TimeOfDay);
        CHECK(r && r->enabled && r->value == "Midday" && r->label == "Time of day" && r->id == LobbyRow::TimeOfDay);   // Midday by default
        const LobbyScene s = BuildLobbyScene(l);
        CHECK(s.settings.size() == BuildLobbyScene(Lobby("/Game/Maps/Arenas/Map_Arena_Cellar")).settings.size() + 1);   // the rows are the same on any map, bar this one
        CHECK(SectionOf(LobbyRow::TimeOfDay) == LobbySection::World);
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, 1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Evening));
        r = Find(BuildLobbyScene(l), LobbyRow::TimeOfDay);
        CHECK(r && r->value == "Evening" && r->hint.empty());
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, 1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Night));
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, 1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Morning));        // round again: there is no Map's own stop
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, -1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Night));
        l.config = StepSetting(l.config, LobbyRow::TimeOfDay, -1);
        CHECK(l.config.timeOfDay == uint8_t(TimeOfDay::Evening));
        // The four stops, in order, and none of them is the retired 0.
        MatchConfig w; w.timeOfDay = uint8_t(TimeOfDay::Morning);
        const char* names[4] = {"Morning", "Midday", "Evening", "Night"};
        for (int i = 0; i < 9; ++i) {
            CHECK(w.timeOfDay >= 1 && w.timeOfDay <= 4 && std::string(TimeOfDayName(w.timeOfDay)) == names[i % 4]);
            w = StepSetting(w, LobbyRow::TimeOfDay, 1);
        }
        // An older host's Map's own (0) steps from Midday, as it reads.
        MatchConfig old; old.timeOfDay = 0;
        CHECK(StepSetting(old, LobbyRow::TimeOfDay, 1).timeOfDay == uint8_t(TimeOfDay::Evening) && StepSetting(old, LobbyRow::TimeOfDay, -1).timeOfDay == uint8_t(TimeOfDay::Morning));
        l.config.timeOfDay = 0;
        r = Find(BuildLobbyScene(l), LobbyRow::TimeOfDay);
        CHECK(r && r->value == "Midday");
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

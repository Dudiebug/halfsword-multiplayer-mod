// Combat scenarios per map (match/hsm_scenarios.h): the per-map table, the fallback to Standard, the Scenario lobby row.
#include "match/hsm_lobby_scene.h"
#include "match/hsm_scenarios.h"
#include <cstdio>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static bool Offers(const char* map, Scenario s) { for (Scenario x : ScenariosFor(map)) if (x == s) return true; return false; }

static LobbyLocal Lobby(const char* map, Format f = Format::FreeForAll) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = true;
    l.config = SelectFormat(l.config, f);
    l.mapPick = map;
    LobbyPlayer me; me.name = "Me"; me.me = true; me.host = true;
    l.players.push_back(me);
    return l;
}
// The Scenario row is the scene's own (like the player limit), never one of the match settings.
static const SettingRow* Find(const LobbyScene& s, LobbyRow id) {
    if (id == LobbyRow::Scenario) return s.hasScenario ? &s.scenario : nullptr;
    for (const SettingRow& r : s.settings) if (r.id == id) return &r;
    return nullptr;
}

int main() {
    // The fence the mod builds itself: exactly the maps whose script builds one, at the script's own spot and radius.
    for (const char* m : {"Alley", "Cellar", "EastTower", "LordsHall", "Pit", "Slums", "Yard"}) {
        const std::string path = std::string("/Game/Maps/Arenas/Map_Arena_") + m + ".Map_Arena_" + m;
        FencePlacement f;
        CHECK(FencePlacementFor(path, f) == Offers(path.c_str(), Scenario::FencedRing));
    }
    {
        FencePlacement f;
        CHECK(FencePlacementFor("/Game/Maps/Arenas/Map_Arena_Cellar", f) && f.radius == 400 && f.x == 724 && f.y == 709);
        CHECK(FencePlacementFor("/game/maps/arenas/map_arena_yard", f) && f.radius == 525 && f.z == -5.0);
        CHECK(FencePlacementFor("Map_Arena_Pit", f) && f.radius == 525);
        CHECK(!FencePlacementFor("", f) && !FencePlacementFor("/Game/Maps/Abyss/Abyss_Map_Open_EA", f));
    }
    // The table: every arena offers Standard, Carnage and Brawling; only the Pit, Yard and Cellar the fence.
    for (const char* m : {"Alley", "Cellar", "EastTower", "LordsHall", "Pit", "Slums", "Yard"}) {
        const std::string path = std::string("/Game/Maps/Arenas/Map_Arena_") + m;
        const std::vector<Scenario> all = ScenariosFor(path);
        CHECK(!all.empty() && all[0] == Scenario::Standard);
        CHECK(Offers(path.c_str(), Scenario::Carnage) && Offers(path.c_str(), Scenario::Brawling));
        const bool fence = std::string(m) == "Pit" || std::string(m) == "Yard" || std::string(m) == "Cellar";
        CHECK(Offers(path.c_str(), Scenario::FencedRing) == fence);
        CHECK(all.size() == (fence ? 4u : 3u));
    }
    // Maps without special scenarios: Standard only. The Abyss, a modded map, nothing, rubbish.
    for (const char* m : {"/Game/Maps/Abyss/Abyss_Map_Open_EA", "/Game/Mods/Cool/Map_Cool", "", "x", "/Game/Maps/Arenas/"}) {
        const std::vector<Scenario> all = ScenariosFor(m);
        CHECK(all.size() == 1 && all[0] == Scenario::Standard);
    }
    // The map is found by its last path segment, case-insensitively, also as a soft object path.
    CHECK(Offers("/Game/Maps/Arenas/Map_Arena_Yard.Map_Arena_Yard", Scenario::FencedRing));
    CHECK(Offers("/game/maps/arenas/map_arena_pit", Scenario::FencedRing));
    CHECK(Offers("Map_Arena_Cellar", Scenario::FencedRing));
    CHECK(!Offers("/Game/Maps/Arenas/Map_Arena_Alley", Scenario::FencedRing));
    CHECK(!Offers("/Game/Maps/Arenas/Map_Arena_Yard_Extra", Scenario::FencedRing));       // a prefix is no match

    // Invalid for the map falls back to Standard.
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", uint8_t(Scenario::FencedRing)) == Scenario::FencedRing);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Alley", uint8_t(Scenario::FencedRing)) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Abyss/Abyss_Map_Open_EA", uint8_t(Scenario::Carnage)) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", 77) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", kScenarioCount) == Scenario::Standard);
    CHECK(EffectiveScenario("", 0) == Scenario::Standard);

    // What each one sets.
    CHECK(WorldOf(Scenario::Standard).loseCondition < 0 && !WorldOf(Scenario::Standard).carnage && !WorldOf(Scenario::Standard).brawling);
    CHECK(WorldOf(Scenario::FencedRing).loseCondition == 3 && !WorldOf(Scenario::FencedRing).carnage);
    CHECK(WorldOf(Scenario::Carnage).carnage && WorldOf(Scenario::Carnage).loseCondition < 0 && !WorldOf(Scenario::Carnage).brawling);
    CHECK(WorldOf(Scenario::Brawling).brawling && !WorldOf(Scenario::Brawling).carnage && WorldOf(Scenario::Brawling).loseCondition < 0);
    for (uint8_t i = 0; i < kScenarioCount; ++i) CHECK(*ScenarioName(Scenario(i)) && *ScenarioHint(Scenario(i)));

    // The stepper walks what the map offers and wraps; a pick the map does not offer steps from Standard.
    const char* yard = "/Game/Maps/Arenas/Map_Arena_Yard";
    const char* alley = "/Game/Maps/Arenas/Map_Arena_Alley";
    uint8_t v = 0;
    const uint8_t yardOrder[4] = {uint8_t(Scenario::FencedRing), uint8_t(Scenario::Carnage), uint8_t(Scenario::Brawling), uint8_t(Scenario::Standard)};
    for (uint8_t want : yardOrder) { v = StepScenario(yard, v, 1); CHECK(v == want); }
    CHECK(StepScenario(yard, 0, -1) == uint8_t(Scenario::Brawling));
    CHECK(StepScenario(alley, 0, 1) == uint8_t(Scenario::Carnage));
    CHECK(StepScenario(alley, uint8_t(Scenario::Brawling), 1) == uint8_t(Scenario::Standard));
    CHECK(StepScenario(alley, uint8_t(Scenario::FencedRing), 1) == uint8_t(Scenario::Carnage));   // Fenced is no stop here: Standard + 1
    CHECK(StepScenario("/Game/Maps/Abyss/Abyss_Map_Open_EA", 0, 1) == 0 && StepScenario("", 3, -1) == 0);

    // Sanitize and the lobby key.
    for (uint8_t i = 0; i < kScenarioCount; ++i) {
        MatchConfig c; c.scenario = i;
        CHECK(Sanitize(c).scenario == i);
        CHECK(ScenarioFromKey(std::to_string(int(i))) == i);                 // key round trip
    }
    MatchConfig bad; bad.scenario = 200; CHECK(Sanitize(bad).scenario == 0);
    CHECK(ScenarioFromKey("") == 0 && ScenarioFromKey("9") == 0 && ScenarioFromKey("x") == 0 && ScenarioFromKey("1x") == 0 &&
          ScenarioFromKey("-1") == 0 && ScenarioFromKey("0001") == 0 && ScenarioFromKey("004") == 0);

    // The lobby row follows the map: Standard-only maps have no row; the Yard has four stops; a pick left from another map reads Standard.
    {
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Alley");
        const LobbyScene s = BuildLobbyScene(l);
        const SettingRow* r = Find(s, LobbyRow::Scenario);
        CHECK(r && r->label == "Scenario" && r->value == "Standard" && r->enabled && s.scenarioMap.find("Alley") != std::string::npos);
        CHECK(SectionOf(LobbyRow::Scenario) == LobbySection::World);
        for (const SettingRow& x : s.settings) CHECK(x.id != LobbyRow::Scenario);                   // apart from the match settings, so their lists stay as they were
        CHECK(size_t(LobbyRow::Scenario) + 1 == size_t(LobbyRow::Sync) && SectionOf(LobbyRow::Scenario) == SectionOf(LobbyRow::Sync));   // its stepper sits just before Sync in WORLD
    }
    {
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Yard");
        l.config.scenario = uint8_t(Scenario::FencedRing);
        LobbyScene a = BuildLobbyScene(l);
        const SettingRow* r = Find(a, LobbyRow::Scenario);
        CHECK(r && r->value == "Fenced ring" && r->hint.find("fence") != std::string::npos);
        l.mapPick = "/Game/Maps/Arenas/Map_Arena_Alley";                      // the host picks another map: the fence is not on offer there
        LobbyScene b = BuildLobbyScene(l);
        r = Find(b, LobbyRow::Scenario);
        CHECK(r && r->value == "Standard");
        l.config.scenario = uint8_t(Scenario::Carnage);
        l.mapPick = "/Game/Maps/Arenas/Map_Arena_Pit";
        LobbyScene c = BuildLobbyScene(l);
        CHECK(Find(c, LobbyRow::Scenario) && Find(c, LobbyRow::Scenario)->value == "Carnage");
    }
    {
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Yard");               // a member sees the row but cannot step it
        l.host = false;
        LobbyScene a = BuildLobbyScene(l);
        const SettingRow* r = Find(a, LobbyRow::Scenario);
        CHECK(r && !r->enabled);
    }
    {
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Yard", Format::Zombies);   // Zombies runs in the Abyss: Standard only, no row
        LobbyScene zs = BuildLobbyScene(l);
        CHECK(!Find(zs, LobbyRow::Scenario));
        LobbyLocal m = Lobby("/Mods/Cool/Map_Cool");                              // a modded map: no row
        LobbyModMap mm; mm.path = "/Mods/Cool/Map_Cool"; mm.display = "Cool"; m.modMaps.push_back(mm);
        const LobbyScene ms = BuildLobbyScene(m);
        CHECK(!Find(ms, LobbyRow::Scenario) && ms.scenarioMap.empty());
        LobbyLocal p = Lobby("/Game/Maps/Arenas/Map_Arena_Yard"); p.progression = true;   // Progression co-op shows Damage alone
        LobbyScene ps = BuildLobbyScene(p);
        CHECK(!Find(ps, LobbyRow::Scenario));
    }
    // The stepper through the scene: StepSetting leaves the scenario alone, the map decides.
    { MatchConfig c; c.scenario = 2; CHECK(StepSetting(c, LobbyRow::Scenario, 1).scenario == 2); }

    if (g_fail == 0) std::printf("hsm_scenarios: all checks passed\n");
    return g_fail ? 1 : 0;
}

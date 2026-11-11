// Combat scenarios per map (match/hsm_scenarios.h): the per-map table, the fallback to Standard, the Scenario lobby row.
#include "match/hsm_lobby_scene.h"
#include "match/hsm_scenarios.h"
#include <cmath>
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

    // What each one sets on the game instance: only the Fenced ring has a setting; Carnage and Brawling are the mod's own effects.
    CHECK(WorldOf(Scenario::Standard).loseCondition < 0 && WorldOf(Scenario::Carnage).loseCondition < 0 && WorldOf(Scenario::Brawling).loseCondition < 0);
    CHECK(WorldOf(Scenario::FencedRing).loseCondition == 3 && kLoseConditionFence == 3);
    for (uint8_t i = 0; i < kScenarioCount; ++i) CHECK(*ScenarioName(Scenario(i)) && *ScenarioHint(Scenario(i)));

    // The bot count: one more than the players, 3 to 5.
    CHECK(kCarnageBotsMin == 3 && kCarnageBotsMax == 5);
    CHECK(CarnageBotCount(-4) == 3 && CarnageBotCount(0) == 3 && CarnageBotCount(1) == 3 && CarnageBotCount(2) == 3);
    CHECK(CarnageBotCount(3) == 4 && CarnageBotCount(4) == 5 && CarnageBotCount(5) == 5 && CarnageBotCount(10) == 5 && CarnageBotCount(1000000) == 5);
    for (int p = 0; p < 40; ++p) CHECK(CarnageBotCount(p) >= 3 && CarnageBotCount(p) <= 5 && CarnageBotCount(p + 1) >= CarnageBotCount(p));

    // Where a scenario works: Carnage in free-for-all only, Brawling in any format but the bot formats, the rest anywhere.
    {
        const ScenarioFormat ffa{true, false}, teams{false, false}, bots{false, true};
        for (const ScenarioFormat& f : {ffa, teams, bots}) CHECK(ScenarioApplies(Scenario::Standard, f) && ScenarioApplies(Scenario::FencedRing, f));
        CHECK(ScenarioApplies(Scenario::Carnage, ffa) && !ScenarioApplies(Scenario::Carnage, teams) && !ScenarioApplies(Scenario::Carnage, bots));
        CHECK(ScenarioApplies(Scenario::Brawling, ffa) && ScenarioApplies(Scenario::Brawling, teams) && !ScenarioApplies(Scenario::Brawling, bots));
        // The effects: Standard and the Fenced ring change nothing in the match itself.
        for (const ScenarioFormat& f : {ffa, teams, bots})
            for (Scenario s : {Scenario::Standard, Scenario::FencedRing}) CHECK(!EffectsOf(s, f, 3).bare && EffectsOf(s, f, 3).carnageBots == 0);
        CHECK(EffectsOf(Scenario::Brawling, ffa, 3).bare && EffectsOf(Scenario::Brawling, ffa, 3).carnageBots == 0);
        CHECK(EffectsOf(Scenario::Brawling, teams, 3).bare && !EffectsOf(Scenario::Brawling, bots, 3).bare);
        CHECK(!EffectsOf(Scenario::Carnage, ffa, 3).bare && EffectsOf(Scenario::Carnage, ffa, 3).carnageBots == 4);
        CHECK(EffectsOf(Scenario::Carnage, ffa, 1).carnageBots == 3 && EffectsOf(Scenario::Carnage, ffa, 8).carnageBots == 5);
        CHECK(EffectsOf(Scenario::Carnage, teams, 3).carnageBots == 0 && EffectsOf(Scenario::Carnage, bots, 3).carnageBots == 0);
    }
    // From the lobby's config and the map: the pick the map does not offer is Standard, a format that does not use it too.
    {
        const char* alley = "/Game/Maps/Arenas/Map_Arena_Alley";
        MatchConfig c; c.format = Format::FreeForAll;
        c.scenario = uint8_t(Scenario::Carnage);
        CHECK(ScenarioEffectsFor(c, alley, 2).carnageBots == 3 && ScenarioEffectsFor(c, alley, 4).carnageBots == 5 && !ScenarioEffectsFor(c, alley, 4).bare);
        CHECK(ScenarioEffectsFor(c, "/Game/Maps/Abyss/Abyss_Map_Open_EA", 4).carnageBots == 0);   // the Abyss offers Standard only
        c.format = Format::TwoTeams; CHECK(ScenarioEffectsFor(c, alley, 4).carnageBots == 0);
        c.format = Format::PlayersVsBots; CHECK(ScenarioEffectsFor(c, alley, 4).carnageBots == 0);
        c.scenario = uint8_t(Scenario::Brawling);
        c.format = Format::FreeForAll; CHECK(ScenarioEffectsFor(c, alley, 2).bare && ScenarioEffectsFor(c, alley, 2).carnageBots == 0);
        c.format = Format::LastManStanding; CHECK(ScenarioEffectsFor(c, alley, 2).bare);
        c.format = Format::Zombies; CHECK(!ScenarioEffectsFor(c, alley, 2).bare);
        c.format = Format::PlayersVsBots; CHECK(!ScenarioEffectsFor(c, alley, 2).bare);
        c.format = Format::FreeForAll; CHECK(!ScenarioEffectsFor(c, "/Mods/Cool/Map_Cool", 2).bare);   // a modded map: Standard
        c.scenario = 77; CHECK(!ScenarioEffectsFor(c, alley, 2).bare && ScenarioEffectsFor(c, alley, 2).carnageBots == 0);
        c.scenario = uint8_t(Scenario::FencedRing); CHECK(!ScenarioEffectsFor(c, "/Game/Maps/Arenas/Map_Arena_Yard", 2).bare);
        CHECK(ScenarioFormatOf(Format::FreeForAll).freeForAll && !ScenarioFormatOf(Format::FreeForAll).botFormat);
        CHECK(!ScenarioFormatOf(Format::TwoTeams).freeForAll && !ScenarioFormatOf(Format::TwoTeams).botFormat);
        CHECK(!ScenarioFormatOf(Format::Zombies).freeForAll && ScenarioFormatOf(Format::Zombies).botFormat);
        CHECK(!ScenarioFormatOf(Format::PlayersVsBots).freeForAll && ScenarioFormatOf(Format::PlayersVsBots).botFormat);
    }
    // The wording keeps to what is done: the blurb of a scenario names its effect, and where the format has none it says so.
    {
        const ScenarioFormat ffa{true, false}, teams{false, false}, bots{false, true};
        CHECK(std::string(ScenarioHint(Scenario::Carnage)).find("computer fighters") != std::string::npos);
        CHECK(std::string(ScenarioHint(Scenario::Carnage)).find("Free-for-all only") != std::string::npos);
        CHECK(std::string(ScenarioHint(Scenario::Carnage)).find("3 to 5") != std::string::npos);
        CHECK(std::string(ScenarioHint(Scenario::Brawling)).find("pants with no weapons") != std::string::npos);
        CHECK(std::string(ScenarioHint(Scenario::FencedRing)).find("Experimental") != std::string::npos);
        CHECK(std::string(ScenarioHint(Scenario::Carnage)).find("Experimental") == std::string::npos && std::string(ScenarioHint(Scenario::Brawling)).find("Experimental") == std::string::npos);
        CHECK(ScenarioHintFor(Scenario::Carnage, ffa) == ScenarioHint(Scenario::Carnage));
        CHECK(ScenarioHintFor(Scenario::Brawling, teams) == ScenarioHint(Scenario::Brawling));
        CHECK(ScenarioHintFor(Scenario::Standard, bots) == ScenarioHint(Scenario::Standard) && ScenarioHintFor(Scenario::FencedRing, bots) == ScenarioHint(Scenario::FencedRing));
        CHECK(ScenarioHintFor(Scenario::Carnage, teams) == "Carnage works in Free-for-all only. In this mode the match plays as Standard.");
        CHECK(ScenarioHintFor(Scenario::Carnage, bots) == ScenarioHintFor(Scenario::Carnage, teams));
        CHECK(ScenarioHintFor(Scenario::Brawling, bots) == "Brawling does not work in Survival or Zombies. Here the match plays as Standard.");
    }
    // The fence the game builds: a bag every 0.8 m of the circle, and the lose volume at 0.95 of the radius.
    CHECK(FenceBagsFor(525) == 41 && FenceBagsFor(400) == 31 && FenceBagsFor(0) == 0 && FenceBagsFor(-10) == 0);
    CHECK(FenceBagsFor(130) == 10 && FenceBagsFor(135) == 11 && FenceBagsFor(2000) == 157);   // rounded, not cut: 10.21 and 10.60
    CHECK(FenceBagsFor(std::nan("")) == 0);
    {
        FencePlacement f;
        CHECK(FencePlacementFor("/Game/Maps/Arenas/Map_Arena_Yard", f));
        CHECK(!OutsideFence(f, f.x, f.y, f.z) && !OutsideFence(f, f.x + 400, f.y + 200, f.z) && OutsideFence(f, f.x + 500, f.y, f.z) && OutsideFence(f, f.x + 500, f.y + 200, f.z));
        CHECK(!OutsideFence(f, f.x + 498, f.y, f.z) && OutsideFence(f, f.x + 499.5, f.y, f.z) && OutsideFence(f, f.x - 600, f.y, f.z));
        CHECK(OutsideFence(f, f.x, f.y - 499.5, f.z) && !OutsideFence(f, f.x, f.y - 498, f.z));
        CHECK(!OutsideFence(f, f.x, f.y, f.z + 498) && OutsideFence(f, f.x, f.y, f.z + 499.5) && OutsideFence(f, f.x, f.y, f.z - 499.5));   // the volume is a sphere
        const FencePlacement u{0, 0, 0, 100};                                                                                                 // the edge itself is inside
        CHECK(!OutsideFence(u, 95, 0, 0) && OutsideFence(u, 95.01, 0, 0) && !OutsideFence(u, 0, 0, -95) && OutsideFence(u, -95.01, 0, 0));
        CHECK(!OutsideFence(f, f.x + 300, f.y + 300, f.z) && OutsideFence(f, f.x + 360, f.y + 360, f.z));                                   // a diagonal: 0.95 * 525 = 498.75
    }

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
        LobbyLocal l = Lobby("/Game/Maps/Arenas/Map_Arena_Alley");               // the row's hint follows the format: Carnage does nothing in Teams
        l.config.scenario = uint8_t(Scenario::Carnage);
        const LobbyScene f = BuildLobbyScene(l);
        const SettingRow* r = Find(f, LobbyRow::Scenario);
        CHECK(r && r->value == "Carnage" && r->hint == ScenarioHint(Scenario::Carnage));
        l.config = SelectFormat(l.config, Format::TwoTeams);
        l.config.scenario = uint8_t(Scenario::Carnage);
        const LobbyScene t = BuildLobbyScene(l);
        r = Find(t, LobbyRow::Scenario);
        CHECK(r && r->value == "Carnage" && r->hint == ScenarioHintFor(Scenario::Carnage, ScenarioFormatOf(Format::TwoTeams)) && r->hint.find("plays as Standard") != std::string::npos);
        l.config.scenario = uint8_t(Scenario::Brawling);
        const LobbyScene w = BuildLobbyScene(l);
        r = Find(w, LobbyRow::Scenario);
        CHECK(r && r->hint == ScenarioHint(Scenario::Brawling));
        l.config = SelectFormat(l.config, Format::PlayersVsBots);
        l.config.scenario = uint8_t(Scenario::Brawling);
        const LobbyScene b = BuildLobbyScene(l);
        r = Find(b, LobbyRow::Scenario);
        CHECK(!b.hasScenario || (r && r->hint.find("does not work in Survival") != std::string::npos));
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

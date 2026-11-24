// Combat scenarios per map (match/hsm_scenarios.h): the per-map table, the fallback to Standard, the retired Fenced ring and
// Carnage scenarios, Brawling and the Blossfechten mode's kit. The lobby has no Scenario row since 0.9.0.
#include "match/hsm_lobby_scene.h"
#include "match/hsm_scenarios.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static bool Offers(const char* map, Scenario s) { for (Scenario x : ScenariosFor(map)) if (x == s) return true; return false; }

int main() {
    // The fence the mod still builds for an older host's Fenced ring: exactly the maps whose script builds one, at the script's own
    // spot and radius. No map offers the Fenced ring any more, so the placement is no longer tied to the table.
    for (const char* m : {"Alley", "Cellar", "EastTower", "LordsHall", "Pit", "Slums", "Yard"}) {
        const std::string path = std::string("/Game/Maps/Arenas/Map_Arena_") + m + ".Map_Arena_" + m;
        FencePlacement f;
        const bool fence = std::string(m) == "Pit" || std::string(m) == "Yard" || std::string(m) == "Cellar";
        CHECK(FencePlacementFor(path, f) == fence);
        CHECK(!Offers(path.c_str(), Scenario::FencedRing));
    }
    {
        FencePlacement f;
        CHECK(FencePlacementFor("/Game/Maps/Arenas/Map_Arena_Cellar", f) && f.radius == 400 && f.x == 724 && f.y == 709);
        CHECK(FencePlacementFor("/game/maps/arenas/map_arena_yard", f) && f.radius == 525 && f.z == -5.0);
        CHECK(FencePlacementFor("Map_Arena_Pit", f) && f.radius == 525);
        CHECK(!FencePlacementFor("", f) && !FencePlacementFor("/Game/Maps/Abyss/Abyss_Map_Open_EA", f));
    }
    // The table: every arena offers Standard, Brawling and Blossfechten, in that order, and nothing else (Carnage and the Fenced
    // ring are retired).
    for (const char* m : {"Alley", "Cellar", "EastTower", "LordsHall", "Pit", "Slums", "Yard"}) {
        const std::string path = std::string("/Game/Maps/Arenas/Map_Arena_") + m;
        const std::vector<Scenario> all = ScenariosFor(path);
        CHECK(all.size() == 3 && all[0] == Scenario::Standard && all[1] == Scenario::Brawling && all[2] == Scenario::Blossfechten);
        CHECK(!Offers(path.c_str(), Scenario::Carnage) && !Offers(path.c_str(), Scenario::FencedRing));
        CHECK(OffersBlossfechten(path));
    }
    // Maps without special scenarios: Standard only. The Abyss, a modded map, nothing, rubbish.
    for (const char* m : {"/Game/Maps/Abyss/Abyss_Map_Open_EA", "/Game/Mods/Cool/Map_Cool", "", "x", "/Game/Maps/Arenas/"}) {
        const std::vector<Scenario> all = ScenariosFor(m);
        CHECK(all.size() == 1 && all[0] == Scenario::Standard);
        CHECK(!OffersBlossfechten(m));
    }
    // The map is found by its last path segment, case-insensitively, also as a soft object path.
    CHECK(Offers("/Game/Maps/Arenas/Map_Arena_Yard.Map_Arena_Yard", Scenario::Blossfechten));
    CHECK(Offers("/game/maps/arenas/map_arena_pit", Scenario::Brawling));
    CHECK(Offers("Map_Arena_Cellar", Scenario::Blossfechten));
    CHECK(OffersBlossfechten("/GAME/MAPS/ARENAS/MAP_ARENA_ALLEY"));
    CHECK(!Offers("/Game/Maps/Arenas/Map_Arena_Yard_Extra", Scenario::Blossfechten));       // a prefix is no match
    CHECK(!OffersBlossfechten("/Game/Maps/Arenas/Map_Arena_Yard_Extra"));

    // Invalid for the map falls back to Standard; the retired scenarios are Standard everywhere.
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", uint8_t(Scenario::FencedRing)) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Alley", uint8_t(Scenario::Carnage)) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", uint8_t(Scenario::Brawling)) == Scenario::Brawling);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", uint8_t(Scenario::Blossfechten)) == Scenario::Blossfechten);
    CHECK(EffectiveScenario("/Game/Maps/Abyss/Abyss_Map_Open_EA", uint8_t(Scenario::Blossfechten)) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Abyss/Abyss_Map_Open_EA", uint8_t(Scenario::Carnage)) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", 77) == Scenario::Standard);
    CHECK(EffectiveScenario("/Game/Maps/Arenas/Map_Arena_Yard", kScenarioCount) == Scenario::Standard);
    CHECK(EffectiveScenario("", 0) == Scenario::Standard);

    // What each one sets on the game instance: nothing, now that the Fenced ring is retired (a client still builds the fence when
    // an older host's Travel carries lose condition 3).
    for (uint8_t i = 0; i < kScenarioCount; ++i) CHECK(WorldOf(Scenario(i)).loseCondition < 0 && *ScenarioName(Scenario(i)));
    CHECK(kLoseConditionFence == 3);

    // Where a scenario works: Brawling and Blossfechten in any format but the bot formats; the rest change nothing.
    {
        const ScenarioFormat ffa{true, false}, teams{false, false}, bots{false, true};
        for (const ScenarioFormat& f : {ffa, teams, bots}) CHECK(ScenarioApplies(Scenario::Standard, f) && ScenarioApplies(Scenario::FencedRing, f) && ScenarioApplies(Scenario::Carnage, f));
        CHECK(ScenarioApplies(Scenario::Brawling, ffa) && ScenarioApplies(Scenario::Brawling, teams) && !ScenarioApplies(Scenario::Brawling, bots));
        CHECK(ScenarioApplies(Scenario::Blossfechten, ffa) && ScenarioApplies(Scenario::Blossfechten, teams) && !ScenarioApplies(Scenario::Blossfechten, bots));
        // The effects: Standard, the retired Fenced ring and the retired Carnage change nothing in the match itself (no computer fighters).
        for (const ScenarioFormat& f : {ffa, teams, bots})
            for (Scenario s : {Scenario::Standard, Scenario::FencedRing, Scenario::Carnage}) CHECK(!EffectsOf(s, f).bare && !EffectsOf(s, f).blossfechten);
        CHECK(EffectsOf(Scenario::Brawling, ffa).bare && EffectsOf(Scenario::Brawling, teams).bare && !EffectsOf(Scenario::Brawling, bots).bare);
        CHECK(!EffectsOf(Scenario::Brawling, ffa).blossfechten);
        CHECK(EffectsOf(Scenario::Blossfechten, ffa).blossfechten && EffectsOf(Scenario::Blossfechten, teams).blossfechten);
        CHECK(!EffectsOf(Scenario::Blossfechten, bots).blossfechten && !EffectsOf(Scenario::Blossfechten, ffa).bare);
    }
    // From the lobby's config and the map: the pick the map does not offer is Standard, a format that does not use it too.
    {
        const char* alley = "/Game/Maps/Arenas/Map_Arena_Alley";
        MatchConfig c; c.format = Format::FreeForAll;
        c.scenario = uint8_t(Scenario::Carnage);                                               // an older host's Carnage: nothing happens
        CHECK(!ScenarioEffectsFor(c, alley).bare && !ScenarioEffectsFor(c, alley).blossfechten);
        c.scenario = uint8_t(Scenario::FencedRing);
        CHECK(!ScenarioEffectsFor(c, "/Game/Maps/Arenas/Map_Arena_Yard").bare && !ScenarioEffectsFor(c, "/Game/Maps/Arenas/Map_Arena_Yard").blossfechten);
        c.scenario = uint8_t(Scenario::Brawling);
        c.format = Format::FreeForAll; CHECK(ScenarioEffectsFor(c, alley).bare);
        c.format = Format::LastManStanding; CHECK(ScenarioEffectsFor(c, alley).bare);
        c.format = Format::Zombies; CHECK(!ScenarioEffectsFor(c, alley).bare);
        c.format = Format::PlayersVsBots; CHECK(!ScenarioEffectsFor(c, alley).bare);
        c.format = Format::FreeForAll; CHECK(!ScenarioEffectsFor(c, "/Mods/Cool/Map_Cool").bare);   // a modded map: Standard
        c.scenario = 77; CHECK(!ScenarioEffectsFor(c, alley).bare && !ScenarioEffectsFor(c, alley).blossfechten);
        c.scenario = uint8_t(Scenario::Blossfechten);                                          // the Blossfechten card
        c.format = Format::LastManStanding; CHECK(ScenarioEffectsFor(c, alley).blossfechten && !ScenarioEffectsFor(c, alley).bare);
        c.format = Format::Duel; CHECK(ScenarioEffectsFor(c, alley).blossfechten);
        CHECK(!ScenarioEffectsFor(c, "/Game/Maps/Abyss/Abyss_Map_Open_EA").blossfechten && !ScenarioEffectsFor(c, "/Mods/Cool/Map_Cool").blossfechten);
        c.format = Format::PlayersVsBots; CHECK(!ScenarioEffectsFor(c, alley).blossfechten);
        CHECK(ScenarioFormatOf(Format::FreeForAll).freeForAll && !ScenarioFormatOf(Format::FreeForAll).botFormat);
        CHECK(!ScenarioFormatOf(Format::TwoTeams).freeForAll && !ScenarioFormatOf(Format::TwoTeams).botFormat);
        CHECK(!ScenarioFormatOf(Format::Zombies).freeForAll && ScenarioFormatOf(Format::Zombies).botFormat);
        CHECK(!ScenarioFormatOf(Format::PlayersVsBots).freeForAll && ScenarioFormatOf(Format::PlayersVsBots).botFormat);
    }
    CHECK(std::string(ScenarioName(Scenario::Blossfechten)) == "Blossfechten" && std::string(ScenarioName(Scenario::Brawling)) == "Brawling");
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

    // Sanitize and the lobby key.
    for (uint8_t i = 0; i < kScenarioCount; ++i) {
        MatchConfig c; c.scenario = i;
        CHECK(Sanitize(c).scenario == i);
        CHECK(ScenarioFromKey(std::to_string(int(i))) == i);                 // key round trip
    }
    MatchConfig bad; bad.scenario = 200; CHECK(Sanitize(bad).scenario == 0);
    CHECK(ScenarioFromKey("") == 0 && ScenarioFromKey("9") == 0 && ScenarioFromKey("x") == 0 && ScenarioFromKey("1x") == 0 &&
          ScenarioFromKey("-1") == 0 && ScenarioFromKey("0001") == 0 && ScenarioFromKey("005") == 0 && ScenarioFromKey("5") == 0);
    CHECK(ScenarioFromKey("4") == uint8_t(Scenario::Blossfechten) && ScenarioFromKey("004") == uint8_t(Scenario::Blossfechten) && kScenarioCount == 5);

    // The Scenario row is gone from the lobby: no map shows one, the stepper has no scenario to walk.
    for (const char* map : {"/Game/Maps/Arenas/Map_Arena_Alley", "/Game/Maps/Arenas/Map_Arena_Yard", "/Mods/Cool/Map_Cool"}) {
        LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.mapPick = map;
        LobbyPlayer me; me.name = "Me"; me.me = true; me.host = true; l.players.push_back(me);
        const LobbyScene s = BuildLobbyScene(l);
        CHECK(s.settings.back().id == LobbyRow::Sync);
        MatchConfig c; c.scenario = 2;
        for (const SettingRow& r : s.settings) CHECK(StepSetting(c, r.id, 1).scenario == 2);   // no row steps the scenario
    }

    // Blossfechten deals swords: the game swaps in a sword only outside Free Mode, and a lobby match is Free Mode.
    {
        const WeaponMask s1 = WeaponBit(1), s2 = WeaponBit(2), mace = WeaponBit(3), pole = WeaponBit(7);
        CHECK(kSwordWeapons == WeaponMask(s1 | s2));
        CHECK(SwordsOnly(0) == kSwordWeapons);                                    // Any: both swords
        CHECK(SwordsOnly(s1) == s1 && SwordsOnly(s2) == s2);                     // a sword tick holds
        CHECK(SwordsOnly(WeaponMask(s1 | mace | pole)) == s1);                   // the other types go
        CHECK(SwordsOnly(WeaponMask(mace | pole)) == kSwordWeapons);             // no sword ticked: both swords
        CHECK(SwordsOnly(kWeaponMaskAll) == kSwordWeapons);
        MatchConfig c; c.gear = Gear::Own; c.tier = 3; c.weapons = WeaponMask(mace | s2);
        MatchConfig k = BlossfechtenKit(c);
        CHECK(k.gear == Gear::RandomTier && k.weapons == s2 && k.tier == 3);    // own kits: a random sword of the tier
        CHECK(RegearFor(k, RespawnCause::Rematch) == Regear::Each && RegearFor(k, RespawnCause::Individual) == Regear::Each);
        c.gear = Gear::HostKit; CHECK(BlossfechtenKit(c).gear == Gear::RandomTier);   // the host's kit may hold anything
        c.gear = Gear::Mirror; c.weapons = 0;
        CHECK(BlossfechtenKit(c).gear == Gear::Mirror && BlossfechtenKit(c).weapons == kSwordWeapons);   // one sword kit for everyone
        c.gear = Gear::RandomTier; CHECK(BlossfechtenKit(c).gear == Gear::RandomTier && BlossfechtenKit(c).weapons == kSwordWeapons);
        c.gear = Gear::Fists; c.weapons = WeaponBit(3); CHECK(BlossfechtenKit(c).gear == Gear::Fists && BlossfechtenKit(c).weapons == WeaponBit(3));   // Brawl and Tag stay bare
        c.gear = Gear::Crypts; CHECK(BlossfechtenKit(c).gear == Gear::Crypts && BlossfechtenKit(c).weapons == WeaponBit(3));   // the Crypts battle keeps what was gathered
        c.gear = Gear::Own; c.weapons = 0; c.format = Format::Riot;
        CHECK(Sanitize(BlossfechtenKit(Sanitize(c))).weapons == kSwordWeapons);   // what the host starts with survives Sanitize
    }
    // The Blossfechten card: the kit is the card's own, so the lobby shows no Gear, Tier, Weapons or Crossbows row for it.
    {
        LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.mapPick = "/Game/Maps/Arenas/Map_Arena_Alley";
        LobbyPlayer me; me.name = "Me"; me.me = true; me.host = true; l.players.push_back(me);
        l.config = SelectMode(MatchConfig{}, Format::LastManStanding, false, false, true);
        CHECK(IsBlossfechten(l.config) && l.config.gear == Gear::RandomTier && l.config.weapons == kSwordWeapons);
        for (const SettingRow& r : BuildLobbyScene(l).settings) CHECK(r.id != LobbyRow::Gear && r.id != LobbyRow::Tier && r.id != LobbyRow::Weapons && r.id != LobbyRow::Crossbows);
        // Leaving the card puts the ordinary kit back; another format's scenario 4 (an older host's) keeps its gear rows.
        const MatchConfig back = SelectMode(l.config, Format::LastManStanding, false);
        CHECK(!IsBlossfechten(back) && back.scenario == uint8_t(Scenario::Standard) && back.gear == Gear::Own && back.weapons == 0);
        LobbyLocal old = l; old.config = MatchConfig{}; old.config.format = Format::TwoTeams; old.config.scenario = uint8_t(Scenario::Blossfechten);
        bool gearRow = false; for (const SettingRow& r : BuildLobbyScene(old).settings) gearRow = gearRow || r.id == LobbyRow::Gear;
        CHECK(gearRow);
    }
    // The Tag card's line (the owner's wording, 2026-11-15).
    CHECK(std::string(FormatBlurb(Format::Tag)) == "Tag.");

    if (g_fail == 0) std::printf("hsm_scenarios: all checks passed\n");
    return g_fail ? 1 : 0;
}

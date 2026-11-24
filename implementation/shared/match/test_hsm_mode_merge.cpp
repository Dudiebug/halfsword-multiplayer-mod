// The merged lobby modes (match/hsm_mode_merge.h, 0.9.0), without a wire change: the Duel / Last man standing card by player
// count, Blossfechten as a card (Last man standing with scenario 4), Free camera always on, and the nearest mode for what an
// older host's lobby can still carry (Pit bridge duels, the Fenced ring and Carnage scenarios, Duel, Brawling).
#include "match/hsm_lobby_scene.h"
#include "match/hsm_map_list.h"
#include "match/hsm_mode_merge.h"
#include "match/hsm_scenarios.h"
#include "protocol/hsm_protocol.h"
#include "session/hsm_autoflow.h"
#include <cstdio>
#include <string>

using namespace hsm;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static const char* const kArenas[] = {"Alley", "Cellar", "EastTower", "LordsHall", "Pit", "Slums", "Yard"};
static std::string Arena(const char* m) { return std::string("/Game/Maps/Arenas/Map_Arena_") + m; }

int main() {
    // The by-count rule: exactly two players read Duel, any other count Last man standing. A lobby that cannot hold more than
    // two reads Duel while it waits for its opponent. One rule for the card, the lobby list, the mode filter and the start.
    CHECK(ReadsDuel(2) && ReadsDuel(2, 8) && ReadsDuel(2, 2));
    CHECK(!ReadsDuel(3) && !ReadsDuel(4, 2) && !ReadsDuel(8, 8) && !ReadsDuel(1) && !ReadsDuel(0) && !ReadsDuel(1, 8));
    CHECK(ReadsDuel(1, 2) && ReadsDuel(0, 2) && ReadsDuel(0, 1) && !ReadsDuel(-1, 8) && !ReadsDuel(3, 2));
    CHECK(!ReadsDuel(1, 3) && !ReadsDuel(0, 3) && !ReadsDuel(0, 8));   // a lobby of three or more seats reads Last man standing until two are in
    CHECK(std::string(DuelCardName(2)) == "Duel" && std::string(DuelCardName(3)) == "Last man standing");
    CHECK(std::string(DuelCardName(1)) == "Last man standing" && std::string(DuelCardName(1, 2)) == "Duel" && std::string(DuelCardName(0)) == "Last man standing");
    for (int p = 0; p < 40; ++p) CHECK((std::string(DuelCardName(p)) == "Duel") == (p == 2));

    // What the match starts as: Duel with exactly two, else Last man standing; every other format is as it is.
    CHECK(IsDuelCard(Format::Duel) && IsDuelCard(Format::LastManStanding) && !IsDuelCard(Format::FreeForAll) && !IsDuelCard(Format::TeamDuel) && !IsDuelCard(Format::Tag));
    for (Format f : {Format::Duel, Format::LastManStanding}) {
        CHECK(FormatForPlayers(f, 2) == Format::Duel);
        CHECK(FormatForPlayers(f, 1) == Format::LastManStanding && FormatForPlayers(f, 3) == Format::LastManStanding && FormatForPlayers(f, 0) == Format::LastManStanding);
        CHECK(FormatForPlayers(f, 16) == Format::LastManStanding);
    }
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::TeamDuel, Format::Tag, Format::Riot, Format::PlayersVsBots, Format::Zombies, Format::HideSeek})
        for (int p : {0, 1, 2, 3, 9}) CHECK(FormatForPlayers(f, p) == f);

    // The dedicated server: server_format=duel and lms read the same rule; the pit bridge word is gone.
    {
        Format f = Format::FreeForAll; int teams = 0;
        CHECK(ParseServerFormat("duel", &f, &teams) && f == Format::Duel);
        CHECK(ParseServerFormat("lms", &f, &teams) && f == Format::LastManStanding);
        CHECK(!ParseServerFormat("pitbridge", &f, &teams) && !ParseServerFormat("pitbridgeduels", &f, &teams) && !ParseServerFormat("", &f, &teams));
        CHECK(ServerFormatFor(Format::Duel, 2) == Format::Duel && ServerFormatFor(Format::Duel, 3) == Format::LastManStanding && ServerFormatFor(Format::Duel, 1) == Format::LastManStanding);
        CHECK(ServerFormatFor(Format::LastManStanding, 2) == Format::LastManStanding && ServerFormatFor(Format::LastManStanding, 5) == Format::LastManStanding);
        CHECK(ServerFormatFor(Format::Riot, 2) == Format::LastManStanding && ServerFormatFor(Format::Riot, 3) == Format::Riot && ServerFormatFor(Format::FreeForAll, 2) == Format::FreeForAll);
    }

    // The name a match shows: Duel by count, Blossfechten by its own name, the others as before.
    {
        MatchConfig c; c.format = Format::LastManStanding;
        CHECK(ModeNameFor(c, 2) == "Duel" && ModeNameFor(c, 3) == "Last man standing" && ModeNameFor(c, 1) == "Last man standing");
        CHECK(ModeNameFor(c, 1, 2) == "Duel" && ModeNameFor(c, 2, 8) == "Duel" && ModeNameFor(c, 4, 2) == "Last man standing");
        c.format = Format::Duel;
        CHECK(ModeNameFor(c, 2) == "Duel" && ModeNameFor(c, 5) == "Last man standing");   // a wire Duel with a third player in reads as the card does
        c.format = Format::LastManStanding; c.gear = Gear::Crypts;
        CHECK(ModeNameFor(c, 2) == "Crypt Royale (Duel)" && ModeNameFor(c, 3) == "Crypt Royale (Last man standing)");
        c.graveRobbers = true; CHECK(ModeNameFor(c, 2) == "Grave Robbers" && ModeNameFor(c, 7) == "Grave Robbers");
        c = MatchConfig{}; c.format = Format::LastManStanding; c.scenario = uint8_t(Scenario::Blossfechten);
        CHECK(ModeNameFor(c, 2) == "Blossfechten" && ModeNameFor(c, 6) == "Blossfechten");
        c.format = Format::Duel; CHECK(ModeNameFor(c, 2) == "Blossfechten");
        c = MatchConfig{}; c.format = Format::FreeForAll;
        CHECK(ModeNameFor(c, 2) == "Carnage" && ModeNameFor(c, 9) == "Carnage");
        c.gear = Gear::Fists; CHECK(ModeNameFor(c, 2) == "Brawl");
        c = MatchConfig{}; c.format = Format::TwoTeams; c.teams = 3;
        CHECK(ModeNameFor(c, 6) == ModeName(Format::TwoTeams, Gear::Own, 3));
        c = MatchConfig{}; c.format = Format::FreeForAll; c.scenario = uint8_t(Scenario::Blossfechten);   // another format's scenario 4 is no card
        CHECK(ModeNameFor(c, 4) == "Carnage");
    }

    // Blossfechten: Last man standing (or Duel) with scenario 4, not bare, not Crypts.
    {
        MatchConfig c; c.format = Format::LastManStanding;
        CHECK(!IsBlossfechten(c));
        c.scenario = uint8_t(Scenario::Blossfechten);
        CHECK(IsBlossfechten(c));
        c.format = Format::Duel; CHECK(IsBlossfechten(c));
        for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::TeamDuel, Format::Tag, Format::PlayersVsBots, Format::Zombies, Format::Tournament}) {
            c.format = f; CHECK(!IsBlossfechten(c));
        }
        c.format = Format::LastManStanding; c.gear = Gear::Crypts; CHECK(!IsBlossfechten(c));
        c.gear = Gear::Fists; CHECK(!IsBlossfechten(c));
        c = MatchConfig{}; c.format = Format::LastManStanding;
        for (uint8_t s : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3)}) { c.scenario = s; CHECK(!IsBlossfechten(c)); }
    }

    // The Blossfechten kit: random swords, no armour; the card's fixed gear. Sanitize holds it, and keeps no crossbows.
    {
        MatchConfig c; c.format = Format::LastManStanding; c.scenario = uint8_t(Scenario::Blossfechten);
        c.gear = Gear::Own; c.weapons = 0; c.crossbows = 2;
        const MatchConfig s = Sanitize(c);
        CHECK(s.gear == Gear::RandomTier && (s.weapons & kSwordWeapons) != 0 && (s.weapons & ~kSwordWeapons) == 0 && s.crossbows == 0);
        CHECK(s.scenario == uint8_t(Scenario::Blossfechten) && s.format == Format::LastManStanding);
        c.weapons = WeaponMask(WeaponBit(1)); CHECK(Sanitize(c).weapons == WeaponMask(WeaponBit(1)));   // the sword ticks stay
        c.weapons = WeaponMask(WeaponBit(3)); CHECK((Sanitize(c).weapons & kSwordWeapons) == kSwordWeapons);   // no sword ticked: both
        c.format = Format::Duel; c.weapons = 0; CHECK(Sanitize(c).gear == Gear::RandomTier && Sanitize(c).weapons == kSwordWeapons);
        MatchConfig plain; plain.format = Format::LastManStanding; plain.gear = Gear::Own; plain.weapons = 0;
        CHECK(Sanitize(plain).gear == Gear::Own && Sanitize(plain).weapons == 0);   // an ordinary lobby is left alone
        const MatchConfig k = SelectMode(MatchConfig{}, Format::LastManStanding, false, false, true);
        CHECK(IsBlossfechten(k) && k.gear == Gear::RandomTier && k.weapons == kSwordWeapons && k.crossbows == 0);
        CHECK(Sanitize(k).gear == k.gear && Sanitize(k).weapons == k.weapons);
        // The kit leaves Bare (Brawl, Tag) and the Crypts as they are.
        MatchConfig cr; cr.format = Format::LastManStanding; cr.gear = Gear::Crypts; cr.weapons = 0;
        CHECK(BlossfechtenKit(cr).gear == Gear::Crypts && BlossfechtenKit(cr).weapons == 0);
        MatchConfig fi; fi.format = Format::FreeForAll; fi.gear = Gear::Fists; fi.weapons = 0;
        CHECK(BlossfechtenKit(fi).gear == Gear::Fists && BlossfechtenKit(fi).weapons == 0);
        // Leaving the card puts the scenario, gear, tier and weapons back, whichever card is picked next.
        for (Format next : {Format::LastManStanding, Format::Duel, Format::TwoTeams, Format::FreeForAll}) {
            const MatchConfig o = SelectMode(k, next, false);
            CHECK(o.scenario == uint8_t(Scenario::Standard) && !IsBlossfechten(o) && o.gear == Gear::Own && o.weapons == 0 && o.tier == MatchConfig{}.tier);
        }
        const MatchConfig ob = SelectMode(k, Format::FreeForAll, true);
        CHECK(ob.scenario == uint8_t(Scenario::Standard) && IsBrawl(ob));
        const MatchConfig oc = SelectMode(k, Format::LastManStanding, false, true);
        CHECK(oc.scenario == uint8_t(Scenario::Standard) && IsCrypts(oc));
    }

    // Free camera is always on: whatever the lobby, a saved config or an older host says, Sanitize writes it on.
    {
        MatchConfig c; c.freeCam = false;
        CHECK(Sanitize(c).freeCam);
        c.format = Format::Duel; CHECK(Sanitize(c).freeCam);
        c.format = Format::LastManStanding; c.scenario = uint8_t(Scenario::Blossfechten); CHECK(Sanitize(c).freeCam);
        c = MatchConfig{}; c.freeCam = true; CHECK(Sanitize(c).freeCam);
        CHECK(MatchConfig{}.freeCam);
        MatchConfig e = LobbyConfigOf(c); e.freeCam = false; CHECK(LobbyConfigOf(e).freeCam);
    }

    // An older host's lobby, read by this build: the nearest mode that is still there.
    {
        MatchConfig c; c.format = Format::Duel; c.scoreLimit = 3;
        MatchConfig o = LobbyConfigOf(c);
        CHECK(o.format == Format::LastManStanding && o.scoreLimit == 3 && IsDuelCard(o.format));   // the round count stays
        c = MatchConfig{}; c.format = Format::LastManStanding; CHECK(LobbyConfigOf(c).format == Format::LastManStanding);
        c = MatchConfig{}; c.format = Format::PitBridgeDuel; c.scoreLimit = 99;
        o = LobbyConfigOf(c);
        CHECK(o.format == Format::FreeForAll && o.scoreLimit == DefaultScoreLimit(Format::FreeForAll));   // Pit bridge duels read as Carnage, at Carnage's limit
        CHECK(ModeNameFor(o, 4) == "Carnage" && !IsQueueFormat(o.format));
        for (uint8_t s : {uint8_t(Scenario::FencedRing), uint8_t(Scenario::Carnage)}) {
            c = MatchConfig{}; c.format = Format::FreeForAll; c.scenario = s;
            o = LobbyConfigOf(c);
            CHECK(o.scenario == uint8_t(Scenario::Standard) && o.format == Format::FreeForAll);
            c.format = Format::TwoTeams; CHECK(LobbyConfigOf(c).scenario == 0 && LobbyConfigOf(c).format == Format::TwoTeams);
        }
        c = MatchConfig{}; c.format = Format::FreeForAll; c.scenario = uint8_t(Scenario::Brawling);
        CHECK(LobbyConfigOf(c).scenario == uint8_t(Scenario::Brawling));                         // Brawling still plays as it did
        c = MatchConfig{}; c.format = Format::Duel; c.scenario = uint8_t(Scenario::Blossfechten);
        o = LobbyConfigOf(c); CHECK(o.format == Format::LastManStanding && IsBlossfechten(o));   // Duel with Blossfechten is the Blossfechten card
        c = MatchConfig{}; c.format = Format::TwoTeams; c.scenario = uint8_t(Scenario::Blossfechten);
        CHECK(LobbyConfigOf(c).scenario == uint8_t(Scenario::Blossfechten) && !IsBlossfechten(LobbyConfigOf(c)));   // not a card; the effect stays
        // Whatever it carries, the result is a config the lobby page and the host accept.
        for (int f = 0; f < kFormatCount; ++f)
            for (uint8_t s = 0; s < kScenarioCount; ++s) {
                MatchConfig x; x.format = Format(f); x.scenario = s;
                const MatchConfig y = Sanitize(LobbyConfigOf(x));
                CHECK(y.format != Format::Duel && y.format != Format::PitBridgeDuel && y.freeCam);
                CHECK(y.scenario != uint8_t(Scenario::FencedRing) && y.scenario != uint8_t(Scenario::Carnage));
                CHECK(!ModeNameFor(y, 2).empty() && !ModeNameFor(y, 5).empty());
                LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.config = y; l.mapPick = Arena("Alley");
                LobbyPlayer me; me.name = "Me"; me.me = true; me.host = true; l.players.push_back(me);
                const LobbyScene sc = BuildLobbyScene(l);
                CHECK(!sc.summary.empty() && !sc.modes.empty());
            }
    }

    // The browser: what a listed lobby is shown as. An older host advertises its own mode text; none of it may show garbage.
    CHECK(BrowserModeFor("Free-for-all", 0, 3, 8) == "Carnage" && BrowserModeFor("Carnage", 0, 3, 8) == "Carnage");
    CHECK(BrowserModeFor("Pit bridge duels", 0, 3, 8) == "Carnage");
    CHECK(BrowserModeFor("Pit bridge duels", uint8_t(Scenario::FencedRing), 3, 8) == "Carnage");
    CHECK(BrowserModeFor("Free-for-all", uint8_t(Scenario::FencedRing), 3, 8) == "Carnage");
    CHECK(BrowserModeFor("Free-for-all", uint8_t(Scenario::Carnage), 3, 8) == "Carnage" && BrowserModeFor("Carnage", uint8_t(Scenario::Carnage), 3, 8) == "Carnage");
    CHECK(BrowserModeFor("Free-for-all", uint8_t(Scenario::Brawling), 3, 8) == "Brawl" && BrowserModeFor("Carnage", uint8_t(Scenario::Brawling), 3, 8) == "Brawl");
    CHECK(BrowserModeFor("Free-for-all", uint8_t(Scenario::Blossfechten), 3, 8) == "Blossfechten");
    CHECK(BrowserModeFor("Carnage", uint8_t(Scenario::Blossfechten), 3, 8) == "Blossfechten" && BrowserModeFor("Pit bridge duels", uint8_t(Scenario::Blossfechten), 3, 8) == "Blossfechten");
    CHECK(BrowserModeFor("Duel", 0, 2, 8) == "Duel" && BrowserModeFor("Duel", 0, 3, 8) == "Last man standing");
    CHECK(BrowserModeFor("Last man standing", 0, 2, 8) == "Duel" && BrowserModeFor("Last man standing", 0, 5, 8) == "Last man standing");
    CHECK(BrowserModeFor("Duel", 0, 1, 2) == "Duel" && BrowserModeFor("Last man standing", 0, 1, 8) == "Last man standing");
    CHECK(BrowserModeFor("Duel", uint8_t(Scenario::Blossfechten), 2, 8) == "Blossfechten" && BrowserModeFor("Last man standing", uint8_t(Scenario::Blossfechten), 7, 8) == "Blossfechten");
    CHECK(BrowserModeFor("Duel", uint8_t(Scenario::Carnage), 2, 8) == "Duel" && BrowserModeFor("Last man standing", uint8_t(Scenario::FencedRing), 4, 8) == "Last man standing");
    CHECK(BrowserModeFor("Teams (3)", 0, 6, 8) == "Teams (3)" && BrowserModeFor("Teams (3)", uint8_t(Scenario::Blossfechten), 6, 8) == "Teams (3)"
          && BrowserModeFor("Teams (3)", uint8_t(Scenario::Brawling), 6, 8) == "Teams (3)");   // others as advertised, whatever the scenario
    CHECK(BrowserModeFor("Brawl", 0, 3, 8) == "Brawl" && BrowserModeFor("Crypt Royale (Duel)", 0, 3, 8) == "Crypt Royale (Duel)" && BrowserModeFor("Crypts (Duel)", 0, 3, 8) == "Crypt Royale (Duel)"   // an older host's Crypts card
          && BrowserModeFor("Grave Robbers", 0, 3, 8) == "Grave Robbers" && BrowserModeFor("", 0, 0, 0).empty());
    for (const char* t : {"Free-for-all", "Carnage", "Pit bridge duels", "Duel", "Last man standing", "Brawl", "Blossfechten", "Teams", "", "Odd text"})
        for (uint8_t s = 0; s < 12; ++s)
            for (int p : {0, 1, 2, 3, 20}) {
                const std::string r = BrowserModeFor(t, s, p, 8);
                CHECK(r != "Free-for-all" && r != "Pit bridge duels" && r != "Duel or last man standing");
                CHECK(r == std::string(t) || r == "Carnage" || r == "Brawl" || r == "Blossfechten" || r == "Duel" || r == "Last man standing");
            }

    // The mode filter's stops: the merged stop takes both of the card's names, the others as before ("Teams" holds "Teams (3)").
    CHECK(BrowserModeMatches("Duel", kDuelCardChip) && BrowserModeMatches("Last man standing", kDuelCardChip));
    CHECK(!BrowserModeMatches("Carnage", kDuelCardChip) && !BrowserModeMatches("Blossfechten", kDuelCardChip) && !BrowserModeMatches("Team duel", kDuelCardChip));
    CHECK(BrowserModeMatches("Carnage", "Carnage") && BrowserModeMatches("Blossfechten", "Blossfechten") && BrowserModeMatches("Brawl", "Brawl"));
    CHECK(BrowserModeMatches("Teams (3)", "Teams") && BrowserModeMatches("Teams", "Teams") && !BrowserModeMatches("Teams (3)", "Team duel"));
    CHECK(!BrowserModeMatches("Duel", "Last man standing") && !BrowserModeMatches("Last man standing", "Duel"));   // the split stops are gone from the filter
    CHECK(BrowserModeMatches("Crypt Royale (Duel)", "Crypt Royale") && !BrowserModeMatches("Crypt Royalex", "Crypt Royale") && !BrowserModeMatches("", "Carnage"));
    CHECK(std::string(kDuelCardChip) == "Duel or last man standing");

    // The Blossfechten maps: exactly the seven arenas, by the scenario table (not the Forest, the Abyss, the Crypts layouts or a mod's map).
    {
        for (const char* m : kArenas) CHECK(OffersBlossfechten(Arena(m)));
        for (const char* m : {"/Game/Maps/Abyss/Abyss_Map_Open_EA", "/Game/Mods/Cool/Map_Cool", "", "x", "/Game/Maps/Arenas/Map_Arena_Ambush_Test",
                              "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main#Purgatory", "/Game/Maps/Map_Hub_Tavern_Frank"})
            CHECK(!OffersBlossfechten(m));
        size_t allowed = 0;
        for (size_t i = 0; i < MapCount(); ++i) {
            const MapInfo& m = kMaps[i];
            const bool ok = MapAllowsMatch(m, Format::LastManStanding, Gear::RandomTier, true);
            bool arena = false;
            for (const char* a : kArenas) if (Arena(a) == m.path) arena = true;
            CHECK(ok == arena);
            if (ok) ++allowed;
        }
        CHECK(allowed == 7);
        // The card needs the format's map as well: a format that fights nowhere in the arenas takes none of the seven.
        for (size_t i = 0; i < MapCount(); ++i) CHECK(!MapAllowsMatch(kMaps[i], Format::Zombies, Gear::Own, true));
        // The same card without Blossfechten takes the Forest, the Abyss, the layouts: Duel and Last man standing share the rule.
        for (size_t i = 0; i < MapCount(); ++i)
            CHECK(MapAllowsMatch(kMaps[i], Format::Duel, Gear::Own) == MapAllowsMatch(kMaps[i], Format::LastManStanding, Gear::Own));
        const MapInfo* forest = FindMap("Map_Arena_Ambush_Test");
        const MapInfo* abyss = FindMap("Abyss_Map_Open_EA");
        CHECK(forest && abyss);
        if (forest && abyss) {
            CHECK(MapAllowsMatch(*forest, Format::Duel, Gear::Own) && !MapAllowsMatch(*forest, Format::Duel, Gear::Own, true));
            CHECK(!MapAllowsMatch(*abyss, Format::Duel, Gear::Own) && !MapAllowsMatch(*abyss, Format::LastManStanding, Gear::RandomTier, true));
        }
        // The picker follows: a pick the mode refuses falls to the first arena, a pick it takes stays.
        CHECK(kMaps[EffectiveMap(Format::LastManStanding, Gear::RandomTier, Arena("Yard"), true)].path == Arena("Yard"));
        CHECK(kMaps[EffectiveMap(Format::LastManStanding, Gear::RandomTier, "/Game/Maps/Arenas/Map_Arena_Ambush_Test", true)].arena);
        CHECK(OffersBlossfechten(kMaps[EffectiveMap(Format::LastManStanding, Gear::RandomTier, "/Game/Maps/Arenas/Map_Arena_Ambush_Test", true)].path));
        CHECK(OffersBlossfechten(kMaps[EffectiveMap(Format::LastManStanding, Gear::RandomTier, "/Game/Maps/Abyss/Abyss_Map_Open_EA", true)].path));
        for (size_t i = 0; i < MapCount(); ++i) CHECK(OffersBlossfechten(kMaps[NextMap(i, Format::LastManStanding, Gear::RandomTier, true)].path));
    }

    // The lobby page: the card list holds the merged cards, in the mock's places, and none of the removed ones.
    {
        LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.mapPick = Arena("Alley");
        LobbyPlayer a; a.name = "A"; a.me = true; a.host = true; l.players.push_back(a);
        LobbyPlayer b; b.name = "B"; l.players.push_back(b);
        l.config = SelectMode(MatchConfig{}, Format::LastManStanding, false);
        LobbyScene s2 = BuildLobbyScene(l);
        int duelCards = 0, blossCards = 0, pit = 0;
        for (const ModeCard& m : s2.modes) {
            if (m.format == Format::Duel) ++duelCards;
            if (m.format == Format::PitBridgeDuel) ++pit;
            if (m.blossfechten) ++blossCards;
            CHECK(m.name != "PIT BRIDGE DUELS" && (m.name != "DUEL" || m.format == Format::LastManStanding));
        }
        CHECK(duelCards == 0 && blossCards == 1 && pit == 0);
        const ModeCard* card = nullptr;
        for (const ModeCard& m : s2.modes) if (m.selected) card = &m;
        CHECK(card && card->name == "DUEL" && !card->blossfechten && s2.summary.find("DUEL") != std::string::npos);   // two players: the card reads Duel
        LobbyPlayer c3; c3.name = "C"; l.players.push_back(c3);
        s2 = BuildLobbyScene(l);
        card = nullptr; for (const ModeCard& m : s2.modes) if (m.selected) card = &m;
        CHECK(card && card->name == "LAST MAN STANDING" && s2.summary.find("LAST MAN STANDING") != std::string::npos);   // three: Last man standing
        // A Duel config of an older host selects the same card.
        l.config.format = Format::Duel;
        s2 = BuildLobbyScene(l);
        int sel = 0; for (const ModeCard& m : s2.modes) if (m.selected) ++sel;
        CHECK(sel == 1);
        // The Blossfechten card: selected alone, fixed gear (no Gear rows), its seven maps.
        l.config = SelectMode(MatchConfig{}, Format::LastManStanding, false, false, true);
        s2 = BuildLobbyScene(l);
        sel = 0; card = nullptr; for (const ModeCard& m : s2.modes) if (m.selected) { ++sel; card = &m; }
        CHECK(sel == 1 && card && card->blossfechten && card->name == "BLOSSFECHTEN" && s2.summary.find("BLOSSFECHTEN") != std::string::npos);
        for (const SettingRow& r : s2.settings) CHECK(r.id != LobbyRow::Gear && r.id != LobbyRow::Tier && r.id != LobbyRow::Weapons && r.id != LobbyRow::Crossbows);
        int enabledMaps = 0; for (const MapCard& m : s2.maps) if (m.allowed && !m.modded && !kMaps[m.index].crypts) ++enabledMaps;   // the Crypts map card (any mode may pick it) aside
        CHECK(enabledMaps == 7);
        l.modMaps = {{"/Game/Maps/Custom/Arena_Bog", "Arena Bog", "BogMap_P.pak"}};
        s2 = BuildLobbyScene(l);
        int modded = 0; for (const MapCard& m : s2.maps) if (m.modded) { ++modded; CHECK(!m.allowed && !m.enabled && !m.reason.empty()); }
        CHECK(modded == 1);   // a modded map is not one of the seven arenas
    }

    if (g_fail == 0) std::printf("hsm_mode_merge: all checks passed\n");
    return g_fail ? 1 : 0;
}

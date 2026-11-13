// Unit tests for the Crypts card in the lobby (#3199, hsm_lobby_scene.h). Run via implementation/shared/run_tests.sh test_hsm_crypts_scene.
#include "match/hsm_lobby_scene.h"
#include "match/hsm_match_impl.h"
#include <algorithm>
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static LobbyPlayer Player(const char* name, bool host, bool me) { LobbyPlayer p; p.name = name; p.host = host; p.me = me; return p; }
static LobbyLocal InLobby(bool host, MatchConfig c, int players = 2) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.2.0"; l.myName = host ? "Ann" : "Bob";
    l.config = c;
    l.players = {Player("Ann", true, host), Player("Bob", false, !host)};
    l.playerCount = players;
    return l;
}
static MatchConfig Crypts(Format battle = Format::LastManStanding) {
    MatchConfig c = SelectMode(MatchConfig{}, Format::FreeForAll, false, true);
    while (c.format != battle) { const Format was = c.format; c = StepSetting(c, LobbyRow::Battle, 1); if (c.format == was) break; }
    return c;
}
static std::vector<LobbyRow> Ids(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }
static const SettingRow* RowOf(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return &r; return nullptr; }
static const ModeCard* CryptsCard(const LobbyScene& s) { for (const ModeCard& m : s.modes) if (m.crypts) return &m; return nullptr; }
static bool Has(const std::vector<LobbyRow>& v, LobbyRow r) { return std::find(v.begin(), v.end(), r) != v.end(); }

// The card: after Brawl, before Progression, a versus mode; picking it lights it alone.
static void TestCard() {
    LobbyScene own = BuildLobbyScene(InLobby(true, MatchConfig{}));
    CHECK(own.modes.size() == kModeCardCount);
    const ModeCard& k = own.modes[kModeOrderCount + 1];
    CHECK(k.crypts && !k.brawl && !k.progression && k.name == "CRYPTS" && !k.selected && k.enabled && k.group == ModeGroup::Versus);
    CHECK(k.description == kCryptsBlurb && k.format == kCryptsDefaultBattle);
    CHECK(own.modes[kModeOrderCount].brawl && own.modes.back().progression);
    LobbyScene s = BuildLobbyScene(InLobby(true, Crypts()));
    const ModeCard* c = CryptsCard(s);
    CHECK(c && c->selected && c->enabled);
    int lit = 0, cards = 0; for (const ModeCard& m : s.modes) { lit += m.selected; cards += m.crypts; }
    CHECK(lit == 1 && cards == 1);                                                       // the Last man standing card is not lit too
    LobbyScene member = BuildLobbyScene(InLobby(false, Crypts()));
    CHECK(CryptsCard(member) && CryptsCard(member)->selected && !CryptsCard(member)->enabled);
    LobbyLocal prog = InLobby(true, Crypts()); prog.progression = true;
    CHECK(!CryptsCard(BuildLobbyScene(prog))->selected);                                 // the Progression card wins
    LobbyScene bots = BuildLobbyScene(InLobby(true, Crypts(Format::PlayersVsBots)));
    CHECK(CryptsCard(bots)->format == Format::PlayersVsBots && CryptsCard(bots)->group == ModeGroup::Versus);   // still the Crypts card's group
}

// Picking and leaving the card.
static void TestSelect() {
    MatchConfig c; c.format = Format::TwoTeams; c.gear = Gear::Mirror; c.tier = 6; c.scoreLimit = 30; c.timeLimitS = 600; c.gatherS = 1200;
    MatchConfig k = SelectMode(c, Format::TwoTeams, false, true);
    CHECK(IsCrypts(k) && k.format == kCryptsDefaultBattle && k.scoreLimit == DefaultScoreLimit(kCryptsDefaultBattle) && k.tier == MatchConfig{}.tier);
    CHECK(k.timeLimitS == 600 && k.gatherS == 1200);                                    // the other settings stay
    CHECK(Sanitize(k).gear == Gear::Crypts);
    k.scoreLimit = 7; CHECK(SelectMode(k, Format::FreeForAll, false, true).scoreLimit == 7);   // the card again changes nothing
    MatchConfig same = SelectMode(k, Format::LastManStanding, false);                     // the battle's own card: the same format, own loadouts
    CHECK(!IsCrypts(same) && same.format == Format::LastManStanding && same.gear == Gear::Own && same.scoreLimit == 7);
    MatchConfig duel = SelectMode(k, Format::Duel, false);
    CHECK(duel.format == Format::Duel && duel.gear == Gear::Own && duel.scoreLimit == DefaultScoreLimit(Format::Duel));
    MatchConfig brawl = SelectMode(k, Format::FreeForAll, true);
    CHECK(IsBrawl(brawl) && brawl.gear == Gear::Fists);
    MatchConfig fromBrawl = SelectMode(brawl, Format::FreeForAll, false, true);
    CHECK(IsCrypts(fromBrawl) && fromBrawl.tier == MatchConfig{}.tier && fromBrawl.format == kCryptsDefaultBattle);
    MatchConfig tag = SelectFormat(MatchConfig{}, Format::Tag);
    MatchConfig fromTag = SelectMode(tag, Format::FreeForAll, false, true);
    CHECK(IsCrypts(fromTag) && fromTag.damagePct == kDamageStock);                     // Tag's light hits stay in Tag
    // The Gear row never lands on Crypts.
    MatchConfig g; for (int i = 0; i < 20; ++i) { g = StepSetting(g, LobbyRow::Gear, i % 3 ? 1 : -1); CHECK(!IsCrypts(g)); }
}

// The Battle row steps through the battle formats and keeps the gear; the Gather time through 5..30 minutes.
static void TestRows() {
    MatchConfig c = Crypts();
    std::vector<Format> seen;
    for (size_t i = 0; i < kCryptsBattleCount; ++i) { seen.push_back(c.format); CHECK(IsCrypts(c) && CryptsBattleFormat(c.format)); c = StepSetting(c, LobbyRow::Battle, 1); }
    CHECK(c.format == kCryptsDefaultBattle && seen.size() == kCryptsBattleCount);       // wraps
    for (size_t i = 0; i < kCryptsBattleCount; ++i) CHECK(seen[i] == kCryptsBattles[i]);
    CHECK(StepSetting(c, LobbyRow::Battle, -1).format == Format::PlayersVsBots);          // back wraps to the last
    CHECK(StepSetting(StepSetting(c, LobbyRow::Battle, 1), LobbyRow::Battle, -1).format == c.format);
    for (Format b : kCryptsBattles) CHECK(GearAllowed(b, Gear::Crypts) && Sanitize(Crypts(b)).gear == Gear::Crypts);
    MatchConfig three = c; three.format = Format::ThreeTeams; CHECK(StepSetting(three, LobbyRow::Battle, 1).format == Format::FreeForAll);   // a stale value steps from the first
    MatchConfig t = StepSetting(c, LobbyRow::Battle, 1);
    CHECK(t.scoreLimit == DefaultScoreLimit(t.format));                                  // a new battle starts at its own limit
    // Gather time.
    MatchConfig g = Crypts(); CHECK(g.gatherS == 600);
    g = StepSetting(g, LobbyRow::Gather, 1); CHECK(g.gatherS == 900);
    g = StepSetting(g, LobbyRow::Gather, -1); g = StepSetting(g, LobbyRow::Gather, -1); CHECK(g.gatherS == 300);
    g = StepSetting(g, LobbyRow::Gather, -1); CHECK(g.gatherS == 1800);                  // wraps
    g = StepSetting(g, LobbyRow::Gather, 1); CHECK(g.gatherS == 300);
    g.gatherS = 3600; CHECK(StepSetting(g, LobbyRow::Gather, 1).gatherS == 300 && StepSetting(g, LobbyRow::Gather, -1).gatherS == 1800);
    g.gatherS = 720; CHECK(StepSetting(g, LobbyRow::Gather, 1).gatherS == 900 && StepSetting(g, LobbyRow::Gather, -1).gatherS == 600);   // a typed 12 min
    LobbyScene s = BuildLobbyScene(InLobby(true, g));
    const SettingRow* gather = RowOf(s, LobbyRow::Gather);
    const SettingRow* battle = RowOf(s, LobbyRow::Battle);
    CHECK(gather && gather->label == "Gather time" && gather->value == "12 min" && gather->enabled && !gather->hint.empty());
    CHECK(battle && battle->label == "Battle" && battle->value == FormatName(g.format) && battle->enabled);
    CHECK(SectionOf(LobbyRow::Battle) == LobbySection::Match && SectionOf(LobbyRow::Gather) == LobbySection::Match);
    CHECK(size_t(LobbyRow::Players) + 1 == size_t(LobbyRow::Battle) && size_t(LobbyRow::Battle) + 1 == size_t(LobbyRow::Gather) && size_t(LobbyRow::Gather) + 1 == size_t(LobbyRow::Limit));
    LobbyScene m = BuildLobbyScene(InLobby(false, g));
    CHECK(RowOf(m, LobbyRow::Gather) && !RowOf(m, LobbyRow::Gather)->enabled);          // members read it
}

// The rows: Battle and Gather time first, then the battle's own rows without Gear (the gathered kits are the gear), and the
// new character's Weapons after its tier and Bodies.
static void TestSettings() {
    for (Format b : kCryptsBattles) {
        MatchConfig c = Crypts(b);
        const std::vector<LobbyRow> v = SettingsFor(c);
        CHECK(v.size() >= 3 && v[0] == LobbyRow::Battle && v[1] == LobbyRow::Gather && v.back() == LobbyRow::Sync);
        CHECK(!Has(v, LobbyRow::Gear) && Has(v, LobbyRow::Weapons) && Has(v, LobbyRow::Tier) && Has(v, LobbyRow::Bodies));
        MatchConfig own = c; own.gear = Gear::Own;
        std::vector<LobbyRow> want = {LobbyRow::Battle, LobbyRow::Gather};
        for (LobbyRow r : SettingsFor(own)) {
            if (r == LobbyRow::Gear) continue;
            want.push_back(r);
            if (r == LobbyRow::Bodies) want.push_back(LobbyRow::Weapons);
        }
        CHECK(v == want);
        LobbyScene s = BuildLobbyScene(InLobby(true, c, b == Format::Duel ? 2 : 4));
        CHECK(Ids(s) == v);
    }
    for (Format f : {Format::LastManStanding, Format::Tag, Format::Duel}) {             // no other card shows them
        MatchConfig c; c.format = f;
        const std::vector<LobbyRow> v = SettingsFor(Sanitize(c));
        CHECK(!Has(v, LobbyRow::Battle) && !Has(v, LobbyRow::Gather));
    }
    CHECK(Has(SettingsFor(Crypts(Format::TwoTeams)), LobbyRow::Teams) && Has(SettingsFor(Crypts(Format::PlayersVsBots)), LobbyRow::WaveBreak));
    // The Tier row is the new character's rank, with its budget; Weapons and Bodies speak of that character.
    MatchConfig c = Crypts(); c.tier = 3;
    LobbyScene s = BuildLobbyScene(InLobby(true, c));
    const SettingRow* tier = RowOf(s, LobbyRow::Tier);
    CHECK(tier && tier->label == "Character tier" && tier->value == std::string(TierName(3)) + " \xC2\xB7 budget " + std::to_string(TierBudget(3)));
    CHECK(tier && tier->hint.find("new random character of this rank") != std::string::npos);
    const SettingRow* weapons = RowOf(s, LobbyRow::Weapons);
    CHECK(weapons && weapons->hint.find("new character") != std::string::npos && weapons->chips.size() == size_t(kWeaponTypeCount) + 1);
    const SettingRow* bodies = RowOf(s, LobbyRow::Bodies);
    CHECK(bodies && bodies->hint.find("Character tier") != std::string::npos);
    MatchConfig o; o.format = Format::LastManStanding;                                     // the other cards keep their words
    const LobbyScene os = BuildLobbyScene(InLobby(true, Sanitize(o)));
    const SettingRow* gearTier = RowOf(os, LobbyRow::Tier);
    CHECK(gearTier && gearTier->label == "Gear tier" && gearTier->hint.find("new random character") == std::string::npos);
}

// The maps: the Abyss is offered with the battles that take large levels; the summary names the battle and the gather.
static void TestMapsAndSummary() {
    LobbyLocal l = InLobby(true, Crypts());
    l.mapPick = kMaps[CryptsDefaultMap(l.config.format)].path;
    LobbyScene s = BuildLobbyScene(l);
    const MapCard* abyss = nullptr; for (const MapCard& m : s.maps) if (std::string(m.name) == "Abyss") abyss = &m;
    CHECK(abyss && abyss->allowed && abyss->enabled && abyss->selected);
    CHECK(s.scenarioMap == "/Game/Maps/Abyss/Abyss_Map_Open_EA");
    CHECK(s.summary == "CRYPTS (LAST MAN STANDING) \xC2\xB7 Abyss \xC2\xB7 Crypts loot, 10 min");
    LobbyLocal noCrypts = l; noCrypts.config.gear = Gear::Own;
    LobbyScene o = BuildLobbyScene(noCrypts);
    for (const MapCard& m : o.maps) if (std::string(m.name) == "Abyss") CHECK(!m.allowed && !m.selected);   // Last man standing alone: no Abyss
    LobbyLocal duel = InLobby(true, Crypts(Format::Duel)); duel.mapPick = "/Game/Maps/Abyss/Abyss_Map_Open_EA";
    LobbyScene d = BuildLobbyScene(duel);
    for (const MapCard& m : d.maps) if (std::string(m.name) == "Abyss") CHECK(!m.allowed && !m.selected && m.reason == "Duel needs an arena");
    CHECK(d.start);                                                                       // two players: a Duel
    LobbyScene d3 = BuildLobbyScene(InLobby(true, Crypts(Format::Duel), 3));
    CHECK(!d3.start && d3.waiting == "Duel is for 2 players");                            // the battle's own player rule
    CHECK(ModeName(Format::TwoTeams, Gear::Crypts, 3) == "Crypts (Teams)" && ModeMatches(ModeName(Format::TwoTeams, Gear::Crypts), "Crypts"));
    CHECK(std::find(BrowserModes().begin(), BrowserModes().end(), "Crypts") != BrowserModes().end());
    CHECK(std::string(GearName(Gear::Crypts)) == "Crypts loot");
}

int main() {
    TestCard();
    TestSelect();
    TestRows();
    TestSettings();
    TestMapsAndSummary();
    if (g_fail) { std::printf("test_hsm_crypts_scene: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypts_scene: ok\n");
    return 0;
}

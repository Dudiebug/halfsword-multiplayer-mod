// Crossbow TDM (match/hsm_modes.h): Teams where everyone has a crossbow. The rules are Teams' (kills to the limit, 2..4 teams,
// picks and auto-balance), the Crossbows row is forced to All and hidden, and without the crossbow classes the host says so once
// and the mode plays as plain Teams. Also the lobby's card, rows and availability.
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_maps.h"
#include "match/hsm_modes.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

using V = std::vector<uint8_t>;
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// Criterion: the format is Teams' rules under another name: 2..4 teams, team picks, scores per team, kills to the limit.
static void TestRules() {
    const Format f = Format::CrossbowTDM;
    CHECK(HasTeams(f) && PicksTeams(f) && !HasRoles(f) && !IsRoundFormat(f) && !IsBotFormat(f) && !ScoresPerFighter(f) && IsCrossbowTdm(f));
    CHECK(TeamCount(f, 2) == 2 && TeamCount(f, 3) == 3 && TeamCount(f, 4) == 4 && TeamCount(f, 9) == 4 && TeamCount(f, 0) == 2);
    CHECK(DefaultScoreLimit(f) == DefaultScoreLimit(Format::TwoTeams) && DropClearFor(f) == DropClearFor(Format::TwoTeams));
    // ResolveTeams gives the same teams as Teams for the same picks.
    const std::vector<TeamRequest> req = {{0, 2, kNoTeam}, {1, kNoTeam, kNoTeam}, {2, kNoTeam, 0}, {3, kNoTeam, kNoTeam}, {4, 1, kNoTeam}};
    CHECK(ResolveTeams(req, f, 3) == ResolveTeams(req, Format::TwoTeams, 3));
    CHECK(ResolveTeams(req, f, 3).at(0) == 2 && ResolveTeams(req, f, 3).at(2) == 0);
    // Scoring: a kill is the killer's team's point, a teamkill costs one, the limit ends it, like Teams.
    MatchConfig c; c.format = f; c.scoreLimit = 2; c.teams = 2;
    Match m; m.Start(c, {{1, 0, true}, {2, 0, true}, {3, 1, true}, {4, 1, true}}, 0);
    m.Tick(Match::kCountdownMs);
    m.OnDeath(3, 1, 6000);
    CHECK(m.Score(0) == 1 && m.Kills(1) == 1 && m.phase() == MatchPhase::Live);
    m.OnDeath(2, 1, 6100);                                       // a teamkill
    CHECK(m.Score(0) == 0);
    m.OnDeath(4, 1, 6200);
    CHECK(m.Score(0) == 1);
    Match::Events ev = m.Tick(6200 + 5000);
    CHECK(ev.respawn.size() == 3);                               // the fallen come back after the respawn delay, unlike the round formats
    m.MarkRespawned(3);
    m.OnDeath(3, 1, 12000);
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 0);
    CHECK(MapAllowsFormat(kMaps[0], f) == MapAllowsFormat(kMaps[0], Format::TwoTeams));
    for (const MapInfo& mi : kMaps) CHECK(MapAllowsFormat(mi, f) == MapAllowsFormat(mi, Format::FreeForAll));
    CHECK(ModeName(f, Gear::Own, 3) == "Crossbow TDM (3)" && ModeName(f, Gear::Own) == "Crossbow TDM" && Has(FormatBlurb(f), "crossbow"));
    CHECK(ModeMatches("Crossbow TDM (4)", "Crossbow TDM") && !ModeMatches("Crossbow TDM", "Teams"));
}

// Criterion: the Crossbows row is forced to All for this mode (whatever a stale lobby value says) and left alone elsewhere.
static void TestSanitize() {
    MatchConfig c; c.format = Format::CrossbowTDM;
    for (uint8_t stale : {0, 1, 2, 3, 4, 9}) { c.crossbows = stale; CHECK(Sanitize(c).crossbows == uint8_t(Crossbows::All)); }
    c.gear = Gear::Fists; CHECK(Sanitize(c).gear == Gear::Own && Sanitize(c).crossbows == uint8_t(Crossbows::All));   // no Brawl in Crossbow TDM
    c.format = Format::TwoTeams; c.gear = Gear::Own; c.crossbows = 2; CHECK(Sanitize(c).crossbows == 2);
    c.teams = 4; c.format = Format::CrossbowTDM; CHECK(Sanitize(c).teams == 4);
}

// Criterion: every fighter gets a crossbow, whatever the seed and the number of fighters; no role excludes anyone.
static void TestEveryoneHasOne() {
    MatchConfig c; c.format = Format::CrossbowTDM;
    const uint8_t mode = Sanitize(c).crossbows;
    CHECK(mode == uint8_t(Crossbows::All));
    for (int n = 2; n <= 9; ++n) {
        V fighters; for (int i = 1; i <= n; ++i) fighters.push_back(uint8_t(i));
        for (uint32_t seed : {1u, 77u, 123456u}) {
            CHECK(PickCrossbowHolders(mode, fighters, seed) == fighters);
            CHECK(PickCrossbowHoldersByRole(mode, fighters, {}, seed) == fighters);
        }
    }
}

// Criterion: the host decides from whether the game has the crossbow classes: present, the crossbows are handed out; missing, none
// and the mode is Teams, with its own log line; other modes keep the Crossbows row and its line.
static void TestFallback() {
    MatchConfig c; c.format = Format::CrossbowTDM; c = Sanitize(c);
    CHECK(EffectiveCrossbows(c, true) == uint8_t(Crossbows::All) && EffectiveCrossbows(c, false) == uint8_t(Crossbows::Off));
    CHECK(std::string(CrossbowMissingLine(Format::CrossbowTDM)) == "crossbow: Crossbow TDM without crossbows (class missing)");
    CHECK(std::string(CrossbowMissingLine(Format::TwoTeams)) == "crossbow: the Crossbows row has no effect in this match (the crossbow or its bolt class is missing)");
    CHECK(CrossbowForEveryone(Format::CrossbowTDM, true) && !CrossbowForEveryone(Format::CrossbowTDM, false) && !CrossbowForEveryone(Format::TwoTeams, true));
    MatchConfig t; t.format = Format::TwoTeams; t.crossbows = 2;
    CHECK(EffectiveCrossbows(t, true) == 2 && EffectiveCrossbows(t, false) == 2);           // the row's own path checks the classes itself
    // Played as Teams without crossbows: nobody is drawn.
    V all = {1, 2, 3, 4};
    CHECK(PickCrossbowHolders(EffectiveCrossbows(c, false), all, 5).empty());
}

static LobbyLocal InLobby(Format f, int players = 4) {
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.config.format = f; l.config.teams = 3; l.config = Sanitize(l.config); l.playerCount = players;
    return l;
}

// Criterion: the rows are Teams' rows minus the Crossbows row; the card sits among the Versus modes and needs 2+ players.
static void TestLobby() {
    LobbyScene teams = BuildLobbyScene(InLobby(Format::TwoTeams)), tdm = BuildLobbyScene(InLobby(Format::CrossbowTDM));
    std::vector<LobbyRow> a, b;
    for (const SettingRow& r : teams.settings) if (r.id != LobbyRow::Crossbows) a.push_back(r.id);
    for (const SettingRow& r : tdm.settings) b.push_back(r.id);
    CHECK(a == b && !b.empty());
    bool hasCrossbows = false, hasTeams = false, hasKit = false, hasMyTeam = false;
    for (LobbyRow r : b) { hasCrossbows = hasCrossbows || r == LobbyRow::Crossbows; hasTeams = hasTeams || r == LobbyRow::Teams; hasKit = hasKit || r == LobbyRow::TeamKit; hasMyTeam = hasMyTeam || r == LobbyRow::MyTeam; }
    CHECK(!hasCrossbows && hasTeams && hasKit && hasMyTeam);
    for (const SettingRow& r : tdm.settings) if (r.id == LobbyRow::Teams) CHECK(r.value == "3 teams");
    const ModeCard* card = nullptr;
    for (const ModeCard& m : tdm.modes) if (m.format == Format::CrossbowTDM) card = &m;
    CHECK(card && card->name == "CROSSBOW TDM" && card->selected && card->enabled && card->group == ModeGroup::Versus && !card->description.empty());
    CHECK(tdm.balance && Has(tdm.summary, "CROSSBOW TDM (3)"));
    CHECK(!ModeAvailable(Format::CrossbowTDM, 1).ok && ModeAvailable(Format::CrossbowTDM, 2).ok);
    int seen = 0; for (const ModeCard& m : tdm.modes) seen += m.format == Format::CrossbowTDM && !m.brawl && !m.progression;
    CHECK(seen == 1);
    bool browser = false; for (const std::string& n : BrowserModes()) browser = browser || n == "Crossbow TDM";
    CHECK(browser);
    // Team picks work as in Teams.
    LobbyLocal withPlayers = InLobby(Format::CrossbowTDM);
    LobbyPlayer ann; ann.name = "Ann"; ann.me = ann.host = true; ann.team = 2;
    withPlayers.players = {ann};
    LobbyScene sc = BuildLobbyScene(withPlayers);
    CHECK(sc.roster.size() == 1 && sc.roster[0].team == "Green");
}

int main() {
    TestRules(); TestSanitize(); TestEveryoneHasOne(); TestFallback(); TestLobby();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("crossbow tdm tests passed\n");
    return 0;
}

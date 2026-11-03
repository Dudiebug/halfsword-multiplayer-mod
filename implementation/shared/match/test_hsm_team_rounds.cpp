// Team rounds (Team Duel, Riot, Juggernaut): Duel's round flow taken to teams. The last team standing takes the round, scores
// are per team, Duel and Last Man Standing stay as they were. Also what the formats add to the lobby (cards, rows, availability,
// maps), the HUD text and scoreboard tags, and the banner that names a fighter's role.
#include "match/hsm_hud_scene.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_maps.h"
#include <algorithm>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

constexpr uint32_t kLive = Match::kCountdownMs;
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static MatchConfig Cfg(Format f, int teams = 2, uint16_t limit = 3) {
    MatchConfig c; c.format = f; c.teams = uint8_t(teams); c.scoreLimit = limit; c.respawnDelayS = 3; return c;
}
// A live match: fighter i (entity 1..) is on team teams[i].
static Match Live(const MatchConfig& c, const std::vector<int8_t>& teams) {
    Match m; std::vector<FighterInfo> f;
    for (size_t i = 0; i < teams.size(); ++i) f.push_back({uint8_t(i + 1), teams[i], true});
    m.Start(c, f, 0); m.Tick(kLive);
    return m;
}

// Criterion: the last TEAM standing takes the round, not the last fighter; the round needs every other team wiped.
static void TestLastTeamStanding() {
    Match m = Live(Cfg(Format::TeamDuel), {0, 0, 1, 1});
    CHECK(m.phase() == MatchPhase::Live && m.round() == 1 && m.RoundInPlay());
    m.OnDeath(1, 3, kLive + 10); m.OnDeath(3, 2, kLive + 20);
    CHECK(m.RoundInPlay() && m.Score(0) == 0 && m.Score(1) == 0);       // one of each team down: both teams still stand
    m.OnDeath(4, 2, kLive + 30);                                         // team 1 wiped
    CHECK(!m.RoundInPlay() && m.Score(0) == 1 && m.Score(1) == 0);       // team 0 takes it, by team
    CHECK(m.roundWinner() == 2 && m.TeamOf(m.roundWinner()) == 0);       // the survivor stands for the team
    CHECK(m.Kills(2) == 2 && m.Kills(3) == 1 && m.Deaths(1) == 1);       // kills are still counted per fighter
    CHECK(m.ScoreKeyOf(2) == 0 && m.ScoreKeyOf(3) == 1 && m.scores().size() == 1);
    CHECK(m.RoundGapLeftMs(kLive + 30) == 3000 && m.round() == 1);
    // The gap, then everybody back for round 2.
    Match::Events ev = m.Tick(kLive + 30 + 2999);
    CHECK(!ev.roundReset && ev.respawn.empty());
    ev = m.Tick(kLive + 30 + 3000);
    CHECK(ev.roundReset && ev.respawn.size() == 4 && m.round() == 2 && m.RoundInPlay());
    for (uint8_t e = 1; e <= 4; ++e) CHECK(m.Alive(e));
    // A two-member team keeps the round going while one of them stands.
    m.OnDeath(3, 1, kLive + 4000); m.OnDeath(4, 2, kLive + 4010);
    CHECK(m.Score(0) == 2 && !m.RoundInPlay());
}

// Criterion: 2, 3 and 4 teams.
static void TestTeamCounts() {
    for (int teams = 2; teams <= 4; ++teams) {
        std::vector<int8_t> side;
        for (int t = 0; t < teams; ++t) { side.push_back(int8_t(t)); side.push_back(int8_t(t)); }   // two fighters per team
        Match m = Live(Cfg(Format::TeamDuel, teams), side);
        CHECK(TeamCount(Format::TeamDuel, teams) == teams);
        // Wipe every team but the last one: the round stays open until then.
        for (int t = 0; t + 1 < teams; ++t) {
            CHECK(m.RoundInPlay());
            m.OnDeath(uint8_t(2 * t + 1), uint8_t(2 * teams - 1), kLive + 10u * uint32_t(t));
            CHECK(m.RoundInPlay());                                      // half of a team down: not wiped
            m.OnDeath(uint8_t(2 * t + 2), uint8_t(2 * teams - 1), kLive + 10u * uint32_t(t) + 5);
        }
        CHECK(!m.RoundInPlay() && m.Score(teams - 1) == 1);              // the last team takes it
        for (int t = 0; t + 1 < teams; ++t) CHECK(m.Score(t) == 0);
        CHECK(m.TeamOf(m.roundWinner()) == teams - 1);
    }
}

// Criterion: the rounds to win decide the match, by team; the winner is a team key.
static void TestMatchWin() {
    Match m = Live(Cfg(Format::TeamDuel, 2, 2), {0, 1, 1});
    m.OnDeath(1, 2, kLive + 10);                                         // round 1: team 1 wins
    CHECK(m.Score(1) == 1 && m.phase() == MatchPhase::Live);
    m.Tick(kLive + 10 + 3000);
    m.OnDeath(2, 1, kLive + 3100); m.OnDeath(3, 1, kLive + 3110);        // round 2: team 0 wins
    CHECK(m.Score(0) == 1 && m.Score(1) == 1 && m.phase() == MatchPhase::Live);
    m.Tick(kLive + 3110 + 3000);
    m.OnDeath(1, 2, kLive + 7000);                                       // round 3: team 1 reaches 2
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 1);
    const Match::Events ev = m.Tick(kLive + 7000);
    CHECK(ev.ended);
    // A match for 3 teams with the same flow.
    Match t3 = Live(Cfg(Format::TeamDuel, 3, 1), {0, 1, 2});
    t3.OnDeath(1, 2, kLive + 1); CHECK(t3.RoundInPlay());                // two teams left
    t3.OnDeath(2, 3, kLive + 2); CHECK(!t3.RoundInPlay() && t3.winner() == 2 && t3.phase() == MatchPhase::Results);
}

// Criterion: a drawn round (nobody can finish it) scores for no team; the next round starts after the gap.
static void TestDraw() {
    Match m = Live(Cfg(Format::TeamDuel), {0, 0, 1});
    m.OnDeath(1, 3, kLive + 5);                                          // the last fighters of two teams are left: 2 and 3
    CHECK(m.RoundInPlay());
    CHECK(m.Draw(kLive + 100));                                          // both down together: the host's referee calls it
    CHECK(!m.RoundInPlay() && m.roundWinner() == 0 && m.Score(0) == 0 && m.Score(1) == 0 && m.scores().empty());
    CHECK(!m.Draw(kLive + 110));                                         // already over
    const Match::Events ev = m.Tick(kLive + 100 + 3000);
    CHECK(ev.roundReset && m.round() == 2 && m.RoundInPlay() && m.Alive(1) && m.Alive(3));
    // A wiped team with a draw call afterwards cannot award the round twice.
    m.OnDeath(3, 1, kLive + 3200);
    CHECK(m.Score(0) == 1 && !m.Draw(kLive + 3300) && m.Score(0) == 1);
}

// Criterion: giving up (held G) goes as a fall with no kill credited; it can end the round for the other team.
static void TestGiveUp() {
    Match m = Live(Cfg(Format::TeamDuel), {0, 1, 1});
    CHECK(m.Forfeit(1, kLive + 10));                                     // the only fighter of team 0 gives up
    CHECK(!m.RoundInPlay() && m.Score(1) == 1 && m.Score(0) == 0 && m.Kills(2) == 0 && m.Kills(3) == 0 && m.Deaths(1) == 1);
    CHECK(!m.Forfeit(2, kLive + 20));                                    // the round is over: nothing to give up
    m.Tick(kLive + 10 + 3000);
    CHECK(m.Forfeit(2, kLive + 3100) && m.RoundInPlay());                // half of team 1: the team still stands
    CHECK(!m.Forfeit(2, kLive + 3110));                                  // already down
    CHECK(m.Forfeit(3, kLive + 3200) && !m.RoundInPlay() && m.Score(0) == 1);   // now team 0 has it
    Match d = Live(Cfg(Format::TeamDuel), {0, 1});
    CHECK(!d.Forfeit(9, kLive + 10) && d.RoundInPlay());                 // a stranger gives up nothing
}

// Criterion: a leaver counts as down; the team that is left takes the round, and a joiner is alive on its team.
static void TestLeaver() {
    Match m = Live(Cfg(Format::TeamDuel), {0, 0, 1});
    CHECK(m.Leave(1, kLive + 10) && m.RoundInPlay());                    // team 0 still has fighter 2
    CHECK(m.Leave(2, kLive + 20));
    CHECK(!m.RoundInPlay() && m.Score(1) == 1 && m.Score(0) == 0);       // team 0 is gone: team 1 has the round
    CHECK(m.Gone(1) && m.Gone(2) && m.Deaths(1) == 1 && m.Kills(3) == 0);   // no kill credited to anyone
    // A late joiner (only a Duel benches a third player) is alive on its team and plays the next round.
    m.AddFighter({4, 0, true});
    CHECK(m.Alive(4) && m.TeamOf(4) == 0);
    const Match::Events ev = m.Tick(kLive + 20 + 3000);
    CHECK(ev.roundReset && ev.respawn.size() == 2 && m.RoundInPlay() && !m.Alive(1) && !m.Alive(2));   // the leavers stay out
    m.OnDeath(3, 4, kLive + 4000);
    CHECK(m.Score(0) == 1 && m.roundWinner() == 4 && m.TeamOf(m.roundWinner()) == 0);
}

// Criterion: Riot and Juggernaut's roles move fighters between the two teams at a rematch; scores start again.
static void TestRematchTeams() {
    Match m = Live(Cfg(Format::Riot, 2, 1), {0, 1, 1});
    m.OnDeath(2, 1, kLive + 1); m.OnDeath(3, 1, kLive + 2);
    CHECK(m.phase() == MatchPhase::Results && m.winner() == 0);
    const Match::Events ev = m.Tick(kLive + 2 + Match::kResultsMs);
    CHECK(ev.rematch && m.phase() == MatchPhase::Countdown && m.Score(0) == 0 && m.round() == 1);
    m.SetTeam(1, 1); m.SetTeam(2, 0); m.SetTeam(9, 0);                   // the Knight moves on; a stranger is ignored
    CHECK(m.TeamOf(1) == 1 && m.TeamOf(2) == 0 && m.TeamOf(3) == 1 && m.TeamOf(9) == kNoTeam);
    m.Tick(kLive + 2 + Match::kResultsMs + Match::kCountdownMs);
    const uint32_t t = kLive + 2 + Match::kResultsMs + Match::kCountdownMs;
    m.OnDeath(1, 2, t + 1); m.OnDeath(3, 2, t + 2);
    CHECK(m.winner() == 0 && m.Kills(2) == 2);                           // fighter 2 is now the lone side
}

// Criterion: Duel and Last Man Standing keep their per-fighter rounds.
static void TestDuelUnchanged() {
    Match d; d.Start(Cfg(Format::LastManStanding, 2, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0); d.Tick(kLive);
    d.OnDeath(1, 2, kLive + 1); CHECK(d.RoundInPlay());
    d.OnDeath(3, 2, kLive + 2); CHECK(!d.RoundInPlay() && d.Score(2) == 1 && d.Score(0) == 0 && d.roundWinner() == 2);
    CHECK(d.ScoreKeyOf(2) == 2 && ScoresPerFighter(Format::LastManStanding));
    Match two; two.Start(Cfg(Format::Duel, 2, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}}, 0); two.Tick(kLive);
    two.OnDeath(1, 2, kLive + 1); CHECK(!two.RoundInPlay() && two.Score(2) == 1 && two.roundWinner() == 2);
    two.Tick(kLive + 1 + 3000); two.AddFighter({3, kNoTeam, true});
    CHECK(!two.Alive(3));                                                // a third player joining a fought Duel is still benched
}

// Criterion: teams for the team rounds come from ResolveTeams (Team Duel: the lobby's picks); Riot's from the roles.
static void TestResolveTeams() {
    std::vector<TeamRequest> req = {{0, kNoTeam, kNoTeam}, {1, kNoTeam, kNoTeam}, {2, 2, kNoTeam}, {3, kNoTeam, 1}, {4, kNoTeam, kNoTeam}};
    auto td = ResolveTeams(req, Format::TeamDuel, 3);
    CHECK(td[2] == 2 && td[3] == 1);                                     // a pick and a host override hold
    for (const auto& [slot, t] : td) CHECK(t >= 0 && t < 3);
    auto riot = ResolveTeams(req, Format::Riot, 4);                      // two teams whatever the lobby's count
    for (const auto& [slot, t] : riot) CHECK(t >= 0 && t < 2);
    for (const auto& [slot, t] : ResolveTeams(req, Format::LastManStanding, 2)) CHECK(t == kNoTeam);   // the per-fighter rounds have none
}

// Criterion: mode availability (Team Duel 2+, Riot and Juggernaut 3+), hints, and the maps like free-for-all.
static void TestAvailability() {
    for (int p = 0; p <= 9; ++p) {
        CHECK(ModeAvailable(Format::TeamDuel, p).ok == (p >= 2));
        CHECK(ModeAvailable(Format::Riot, p).ok == (p >= 3));
        CHECK(ModeAvailable(Format::Juggernaut, p).ok == (p >= 3));
    }
    CHECK(std::string(ModeAvailable(Format::Riot, 2).hint) == "Needs 3 or more players" && std::string(ModeAvailable(Format::Juggernaut, 1).hint) == "Needs 3 or more players");
    CHECK(std::string(ModeAvailable(Format::TeamDuel, 1).hint) == "Needs 2 or more players" && std::string(ModeAvailable(Format::Riot, 3).hint).empty());
    for (size_t i = 0; i < MapCount(); ++i)
        for (Format f : {Format::TeamDuel, Format::Riot, Format::Juggernaut})
            CHECK(MapAllowsFormat(kMaps[i], f) == MapAllowsFormat(kMaps[i], Format::FreeForAll));
    CHECK(MapAllowsFormat(kMaps[0], Format::Riot) == (kMaps[0].arena || kMaps[0].bigArena));
}

static LobbyLocal InLobby(Format f, int players) {
    LobbyLocal l; l.phase = LobbyPhase::InLobby; l.host = true; l.version = "0.8.0"; l.myName = "Ann";
    l.config.format = f; l.config.scoreLimit = DefaultScoreLimit(f);
    for (int i = 0; i < players; ++i) { LobbyPlayer p; p.name = "P" + std::to_string(i); p.host = i == 0; p.me = i == 0; l.players.push_back(p); }
    return l;
}
static std::vector<LobbyRow> RowIds(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }

// Criterion: the lobby cards sit in VERSUS, the rows match each format, the names and blurbs exist.
static void TestLobby() {
    using R = LobbyRow;
    const LobbyScene riot = BuildLobbyScene(InLobby(Format::Riot, 4));
    CHECK(RowIds(riot) == (std::vector<R>{R::Limit, R::Time, R::Respawn, R::FreeCam, R::FriendlyFire, R::Damage, R::TeamKit, R::Crossbows, R::Sync}));   // no Gear, Tier, Weapons, Teams, My team; the camera, Team colours and Crossbows rows apply
    CHECK(RowIds(BuildLobbyScene(InLobby(Format::Juggernaut, 4))) == RowIds(riot));
    const LobbyScene td = BuildLobbyScene(InLobby(Format::TeamDuel, 4));
    CHECK(RowIds(td) == (std::vector<R>{R::Limit, R::Time, R::Respawn, R::FreeCam, R::Gear, R::Tier, R::Teams, R::FriendlyFire, R::Damage, R::MyTeam, R::TeamKit, R::Crossbows, R::Sync}));
    CHECK(SettingsFor(MatchConfig{Format::Riot}) == SettingsFor(MatchConfig{Format::Juggernaut}));
    // Teams row: Team Duel counts 2..4 and steps; Riot's rows hold none to step.
    MatchConfig c; c.format = Format::TeamDuel;
    CHECK(StepSetting(c, R::Teams, 1).teams == 3 && StepSetting(StepSetting(c, R::Teams, 1), R::Teams, 1).teams == 4 && StepSetting(c, R::Teams, -1).teams == 4);
    CHECK(DefaultScoreLimit(Format::Riot) == 3 && SelectFormat(MatchConfig{}, Format::Riot).scoreLimit == 3);
    CHECK((LimitOptions(Format::Riot) == std::vector<int>{1, 2, 3, 5, 7, 10}));
    // Cards.
    CHECK(riot.modes.size() == kModeCardCount);
    int found = 0;
    for (size_t i = 0; i < riot.modes.size(); ++i) {
        const ModeCard& m = riot.modes[i];
        if (m.brawl || m.progression) continue;
        if (m.format == Format::TeamDuel) { ++found; CHECK(m.name == "TEAM DUEL" && m.group == ModeGroup::Versus && !m.description.empty()); }
        if (m.format == Format::Riot) { ++found; CHECK(m.name == "RIOT" && m.group == ModeGroup::Versus && m.selected && m.enabled); }
        if (m.format == Format::Juggernaut) { ++found; CHECK(m.name == "JUGGERNAUT" && m.group == ModeGroup::Versus); }
    }
    CHECK(found == 3);
    CHECK(std::string(FormatBlurb(Format::Riot)) != FormatBlurb(Format::Juggernaut) && std::string(FormatBlurb(Format::TeamDuel)) != FormatBlurb(Format::FreeForAll));
    // Too few players: the card is disabled with its hint, START is off and the wait line says why.
    const LobbyScene two = BuildLobbyScene(InLobby(Format::Riot, 2));
    for (const ModeCard& m : two.modes) {
        if (m.brawl || m.progression) continue;
        if (m.format == Format::Riot || m.format == Format::Juggernaut) CHECK(!m.enabled && m.hint == "Needs 3 or more players");
        if (m.format == Format::TeamDuel) CHECK(m.enabled);
    }
    CHECK(!two.start && two.waiting == "Needs 3 or more players");
    CHECK(BuildLobbyScene(InLobby(Format::Riot, 3)).waiting != "Needs 3 or more players");
    // Roster: Team Duel shows teams and the auto-balance; Riot's teams come from the roles, so none are shown.
    LobbyLocal tl = InLobby(Format::TeamDuel, 3); tl.players[1].team = 1;
    LobbyScene ts = BuildLobbyScene(tl);
    CHECK(ts.balance && ts.roster[1].team == "Blue" && ts.roster[0].team == "Auto");
    LobbyLocal rl = InLobby(Format::Riot, 3); rl.players[1].team = 1;
    LobbyScene rs = BuildLobbyScene(rl);
    CHECK(!rs.balance && rs.roster[1].team.empty() && !rs.roster[1].movable);
    // The summary names the mode, not a gear the roles override; Team Duel's keeps it.
    CHECK(Has(rs.summary, "RIOT") && !Has(rs.summary, "Own loadouts") && !Has(rs.summary, "tier"));
    LobbyLocal tg = InLobby(Format::TeamDuel, 3); tg.config.teams = 3; tg.config.gear = Gear::RandomTier;
    CHECK(Has(BuildLobbyScene(tg).summary, "TEAM DUEL (3)") && Has(BuildLobbyScene(tg).summary, "Random by tier"));
    // Names, and the browser.
    CHECK(FormatName(Format::TeamDuel) == "Team duel" && FormatName(Format::Riot) == "Riot" && FormatName(Format::Juggernaut) == "Juggernaut");
    CHECK(ModeName(Format::TeamDuel, Gear::Own, 3) == "Team duel (3)" && ModeName(Format::Riot, Gear::RandomTier, 2) == "Riot");
    CHECK(ModeName(uint8_t(Format::Juggernaut), uint8_t(Gear::Own), 2) == "Juggernaut");
    const auto& modes = BrowserModes();
    for (const char* n : {"Team duel", "Riot", "Juggernaut"}) {
        CHECK(std::find(modes.begin(), modes.end(), n) != modes.end());
        BrowserFilter f; f.mode = uint8_t(std::find(modes.begin(), modes.end(), n) - modes.begin());
        LobbyFound x; x.compatible = true; x.players = 1; x.maxPlayers = 8;
        x.mode = ModeName(n == std::string("Team duel") ? Format::TeamDuel : n == std::string("Riot") ? Format::Riot : Format::Juggernaut, Gear::Own, 3);
        CHECK(PassesFilter(x, f));
        x.mode = "Teams (3)"; CHECK(!PassesFilter(x, f));
    }
    BrowserFilter teamsFilter; teamsFilter.mode = uint8_t(std::find(modes.begin(), modes.end(), "Teams") - modes.begin());
    LobbyFound tdLobby; tdLobby.compatible = true; tdLobby.mode = "Team duel (3)"; tdLobby.players = 1; tdLobby.maxPlayers = 8;
    CHECK(!PassesFilter(tdLobby, teamsFilter));                          // the Teams chip does not list Team duel
}

static MatchInfo RiotInfo() {
    MatchInfo m; m.format = uint8_t(Format::Riot); m.scoreLimit = 3; m.phase = uint8_t(MatchPhase::Live); m.respawnDelayS = 4; m.round = 2;
    m.scores = {{0, 1}, {1, 0}};
    m.fighters = {{1, 0, 2, 0, "Host", 0, true}, {2, 1, 0, 1, "Ann", 60, true}, {3, 1, 0, 0, "Bo", 70, true}};
    m.fighters[0].role = kRoleKnight; m.fighters[1].role = RioterRoleId(2); m.fighters[2].role = RioterRoleId(4);
    return m;
}

// Criterion: the HUD shows the team scores, the round winner as a team, the winner by team, and the role tag and line.
static void TestHud() {
    MatchInfo m = RiotInfo();
    std::string s = StatusLine(m, 2);
    CHECK(Has(s, "Rounds") && Has(s, "RED 1") && Has(s, "BLUE 0") && Has(s, "first to 3"));
    m.roundGapLeftMs = 3000; m.roundWinner = 1;                          // the gap: the Knight's team took the round
    CHECK(StatusLine(m, 2) == "Round 2 to RED   next round in 3");
    CHECK(StatusLine(m, 1) == "Round 2 to your team   next round in 3");
    m.roundWinner = 0; CHECK(Has(StatusLine(m, 2), "drawn"));
    m.roundGapLeftMs = 0; m.phase = uint8_t(MatchPhase::Results); m.winner = 1; m.timeLeftMs = 9000;
    CHECK(Has(StatusLine(m, 2), "BLUE WINS"));
    CHECK(ResultShot(m, 2).text == "BLUE WINS" && ResultShot(m, 1).text == "BLUE WINS");
    // Duel keeps naming the fighter.
    MatchInfo d; d.format = uint8_t(Format::Duel); d.phase = uint8_t(MatchPhase::Live); d.round = 1; d.roundGapLeftMs = 2000; d.roundWinner = 2;
    d.fighters = {{1, -1, 0, 1, "Host"}, {2, -1, 1, 0, "Ann"}};
    CHECK(StatusLine(d, 1) == "Round 1 to Ann   next round in 2" && StatusLine(d, 2) == "Round 1 to you   next round in 2");
    // Scoreboard: the Knight is tagged, the Rioters are not; two bands; team scores.
    LocalHud l; l.me = 1; l.host = 1; l.tab = true;
    const HudScene scene = BuildHudScene(RiotInfo(), l, 1000);
    CHECK(scene.board.visible);
    int bands = 0, tagged = 0;
    for (const BoardRow& r : scene.board.rows) {
        if (r.kind == BoardRow::Kind::Band) { ++bands; continue; }
        if (r.name == "Host") { CHECK(r.tag == "KNIGHT"); ++tagged; }
        else CHECK(r.tag.empty());
    }
    CHECK(bands == 2 && tagged == 1);
    MatchInfo jg = RiotInfo(); jg.format = uint8_t(Format::Juggernaut); jg.fighters[0].role = kRoleJuggernaut; jg.fighters[1].role = kRoleSkirmisher;
    for (const BoardRow& r : BuildHudScene(jg, l, 1000).board.rows) if (r.name == "Host") CHECK(r.tag == "JUGGERNAUT");
    for (const BoardRow& r : BuildHudScene(jg, l, 1000).board.rows) if (r.name == "Ann") CHECK(r.tag.empty());
}

// Criterion: the role banner at the spawn - "You are the KNIGHT" - on the FIGHT banner, once per round; none without a role.
static void TestBanner() {
    MatchInfo m = RiotInfo(); m.phase = uint8_t(MatchPhase::Countdown); m.round = 1; m.scores.clear();
    BannerTracker t;
    t.Observe(m, 1, 0);                                                  // the first MatchInfo only records
    m.phase = uint8_t(MatchPhase::Live);
    t.Observe(m, 1, 100);
    CHECK(t.shot().text == "FIGHT" && Has(t.shot().sub, "YOU ARE THE KNIGHT") && Has(t.shot().sub, "ROUND 1"));
    BannerTracker r; m.phase = uint8_t(MatchPhase::Countdown); r.Observe(m, 2, 0); m.phase = uint8_t(MatchPhase::Live); r.Observe(m, 2, 100);
    CHECK(Has(r.shot().sub, "YOU ARE A RIOTER") && !Has(r.shot().sub, "KNIGHT"));
    // A new round shows it again; the rematch's new role too.
    m.round = 2; t.Observe(m, 1, 5000);
    CHECK(t.shot().text == "FIGHT" && Has(t.shot().sub, "ROUND 2") && Has(t.shot().sub, "YOU ARE THE KNIGHT"));
    // The round banner names the team: ours or theirs.
    MatchInfo g = m; g.roundGapLeftMs = 3000; g.roundWinner = 1; g.scores = {{0, 1}};
    t.Observe(g, 1, 6000);
    CHECK(t.shot().text == "ROUND TO YOUR TEAM" && t.shot().atMs == 6000);
    BannerTracker o; MatchInfo h = g; h.roundGapLeftMs = 0; o.Observe(h, 2, 0); o.Observe(g, 2, 10);
    CHECK(o.shot().text == "ROUND TO RED");
    // Another format's FIGHT banner has no role line (a plain fighter).
    MatchInfo duel; duel.format = uint8_t(Format::Duel); duel.round = 1; duel.phase = uint8_t(MatchPhase::Countdown); duel.fighters = {{1, -1, 0, 0, "A"}, {2, -1, 0, 0, "B"}};
    BannerTracker dt; dt.Observe(duel, 1, 0); duel.phase = uint8_t(MatchPhase::Live); dt.Observe(duel, 1, 10);
    CHECK(dt.shot().text == "FIGHT" && dt.shot().sub == "ROUND 1");
}

int main() {
    TestLastTeamStanding(); TestTeamCounts(); TestMatchWin(); TestDraw(); TestGiveUp(); TestLeaver(); TestRematchTeams(); TestDuelUnchanged();
    TestResolveTeams(); TestAvailability(); TestLobby(); TestHud(); TestBanner();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("team round tests passed\n");
    return 0;
}

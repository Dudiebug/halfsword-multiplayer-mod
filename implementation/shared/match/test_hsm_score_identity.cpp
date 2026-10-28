// Scores by player identity (SteamID, not slot): a rejoin gets its score back (#29), and so does the new host after a migration.
#include "session/hsm_migration.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

constexpr uint64_t kAnn = 76561198000000001ull, kBob = 76561198000000002ull, kCy = 76561198000000003ull;

static MatchConfig Cfg(Format f) { MatchConfig c; c.format = f; c.scoreLimit = 10; c.respawnDelayS = 2; return c; }
static std::vector<FighterInfo> Three() { return {{1, kNoTeam, true, kAnn}, {2, kNoTeam, true, kBob}, {3, kNoTeam, true, kCy}}; }

// ann (entity 1) kills bob (2) twice and cy (3) once, and dies to bob once.
static void Fight(Match& m) {
    m.Tick(5000);
    m.OnDeath(2, 1, 6000); m.OnDeath(3, 1, 6100); m.Tick(9000); m.MarkRespawned(2); m.MarkRespawned(3); m.OnDeath(2, 1, 9100); m.OnDeath(1, 2, 9200);
}

static void TestRejoinGetsScoreBack() {
    Match m; m.Start(Cfg(Format::FreeForAll), Three(), 0);
    Fight(m);
    CHECK(m.Score(1) == 3 && m.Kills(1) == 3 && m.Deaths(1) == 1 && m.Score(2) == 1 && m.Kills(2) == 1);
    m.RemoveFighter(1);                                     // ann leaves for good
    CHECK(!m.Has(1) && m.Score(1) == 0);
    m.AddFighter({1, kNoTeam, true, kAnn});                 // and comes back
    CHECK(m.Score(1) == 3 && m.Kills(1) == 3 && m.Deaths(1) == 1);
    CHECK(m.Score(2) == 1 && m.Kills(3) == 0);              // the others are untouched
}

static void TestRejoinOnAnotherSlot() {
    Match m; m.Start(Cfg(Format::FreeForAll), Three(), 0);
    Fight(m);
    m.RemoveFighter(1);
    m.AddFighter({7, kNoTeam, true, kAnn});                 // the slot was taken meanwhile
    CHECK(m.Score(7) == 3 && m.Kills(7) == 3 && m.Deaths(7) == 1);
    CHECK(m.Score(1) == 0);                                 // nothing left on the old entity
    m.RemoveFighter(7); m.AddFighter({7, kNoTeam, true, kAnn});   // the bank is used up on return, and refilled on leave
    CHECK(m.Score(7) == 3);
}

static void TestDifferentPlayerStartsAtZero() {            // the negative control
    Match m; m.Start(Cfg(Format::FreeForAll), Three(), 0);
    Fight(m);
    m.RemoveFighter(1);
    m.AddFighter({1, kNoTeam, true, 76561198000000099ull}); // another player takes ann's slot
    CHECK(m.Score(1) == 0 && m.Kills(1) == 0 && m.Deaths(1) == 0);
    m.AddFighter({8, kNoTeam, true, 0});                    // no identity (direct IP, bot): nothing banked for it
    CHECK(m.Score(8) == 0 && m.Kills(8) == 0);
    m.RemoveFighter(8); m.AddFighter({8, kNoTeam, true, 0});
    CHECK(m.Score(8) == 0);
    m.AddFighter({9, kNoTeam, true, kAnn});                 // ann herself is still owed hers
    CHECK(m.Score(9) == 3);
}

static void TestRematchForgetsLeavers() {
    Match m; m.Start(Cfg(Format::FreeForAll), Three(), 0);
    Fight(m);
    m.RemoveFighter(1);
    m.Finish(2, 10000); m.Tick(10000 + Match::kResultsMs);  // results over: the rematch
    m.AddFighter({1, kNoTeam, true, kAnn});
    CHECK(m.Score(1) == 0 && m.Kills(1) == 0);
}

static void TestTeamsKeepTeamScore() {
    Match m; m.Start(Cfg(Format::TwoTeams), {{1, 0, true, kAnn}, {2, 1, true, kBob}, {3, 0, true, kCy}}, 0);
    m.Tick(5000);
    m.OnDeath(2, 1, 6000); m.MarkRespawned(2); m.OnDeath(2, 3, 9000);
    CHECK(m.Score(0) == 2 && m.Kills(1) == 1);
    m.RemoveFighter(1);
    CHECK(m.Score(0) == 2);                                 // the team keeps its score when a member leaves
    m.AddFighter({1, 0, true, kAnn});
    CHECK(m.Score(0) == 2 && m.Kills(1) == 1);              // and the member's own kills return; no double count
}

static MatchInfo InfoOf(const Match& m, std::map<uint8_t, uint64_t> ids) {
    MatchInfo i; i.format = uint8_t(m.config().format); i.scoreLimit = m.config().scoreLimit; i.phase = uint8_t(m.phase());
    for (const auto& [k, s] : m.scores()) i.scores.push_back({int16_t(k), int16_t(s)});
    for (const auto& [e, id] : ids) {
        FighterEntry f; f.entity = e; f.team = m.TeamOf(e); f.kills = int16_t(m.Kills(e)); f.deaths = int16_t(m.Deaths(e)); f.steamId = id;
        i.fighters.push_back(f);
    }
    // what a client holds went over the wire
    auto bytes = Encode(i); MatchInfo out; if (!Decode(bytes.data(), bytes.size(), out)) { std::puts("decode failed"); ++g_fail; }
    return out;
}

static void TestMigrationRestoresFreeForAll() {
    Match old; old.Start(Cfg(Format::FreeForAll), Three(), 0);
    Fight(old);
    const ScoreCarry board = ScoreCarryFrom(InfoOf(old, {{1, kAnn}, {2, kBob}, {3, kCy}}));
    CHECK(board.format == Format::FreeForAll && board.players.size() == 3 && board.teams.empty());

    // The new host is bob; the map is reopened and the players join in another order, on other slots (ann has not yet).
    Match m; m.Start(Cfg(Format::FreeForAll), {{1, kNoTeam, true, kBob}, {2, kNoTeam, true, kCy}}, 100000, board);
    CHECK(m.phase() == MatchPhase::Countdown);
    CHECK(m.Score(1) == 1 && m.Kills(1) == 1 && m.Deaths(1) == 2);           // bob: one point, killed twice by ann, one kill of ann
    CHECK(m.Score(2) == 0 && m.Kills(2) == 0 && m.Deaths(2) == 1);            // cy: died once
    m.AddFighter({3, kNoTeam, true, kAnn});                 // ann comes in late
    CHECK(m.Score(3) == 3 && m.Kills(3) == 3 && m.Deaths(3) == 1);
    m.AddFighter({4, kNoTeam, true, 76561198000000077ull}); // a stranger: nothing
    CHECK(m.Score(4) == 0 && m.Kills(4) == 0);
    CHECK(m.scores().count(4) == 0);
}

static void TestMigrationRestoresTeamsAndSurvival() {
    Match old; old.Start(Cfg(Format::TwoTeams), {{1, 0, true, kAnn}, {2, 1, true, kBob}}, 0);
    old.Tick(5000); old.OnDeath(2, 1, 6000); old.MarkRespawned(2); old.OnDeath(2, 1, 9000);
    const ScoreCarry board = ScoreCarryFrom(InfoOf(old, {{1, kAnn}, {2, kBob}}));
    CHECK(board.teams.size() == 1 && board.teams.at(0) == 2 && board.players.size() == 2);
    Match m; m.Start(Cfg(Format::TwoTeams), {{1, 1, true, kBob}}, 0, board);
    CHECK(m.Score(0) == 2 && m.Score(1) == 0 && m.Kills(1) == 0 && m.Deaths(1) == 2);
    m.AddFighter({2, 0, true, kAnn});
    CHECK(m.Kills(2) == 2 && m.Score(0) == 2);

    ScoreCarry sb; sb.format = Format::PlayersVsBots; sb.teams[kHumanTeam] = 4; sb.players.push_back({kAnn, 0, 6, 1});
    Match s2; s2.Start(Cfg(Format::PlayersVsBots), {{1, 0, true, kAnn}}, 0, sb);
    CHECK(s2.wave() == 4 && s2.Score(kHumanTeam) == 4 && s2.Kills(1) == 6);
}

static void TestMigrationBoardGuards() {
    Match old; old.Start(Cfg(Format::FreeForAll), Three(), 0);
    Fight(old);
    const MatchInfo live = InfoOf(old, {{1, kAnn}, {2, kBob}, {3, kCy}});
    // another format than the board's: not applied
    Match two; two.Start(Cfg(Format::TwoTeams), {{1, 0, true, kAnn}}, 0, ScoreCarryFrom(live));
    CHECK(two.Kills(1) == 0 && two.scores().empty());
    // no match running, or over: nothing to hand over
    MatchInfo idle = live; idle.phase = uint8_t(MatchPhase::Idle); CHECK(ScoreCarryFrom(idle).empty());
    MatchInfo done = live; done.phase = uint8_t(MatchPhase::Results); CHECK(ScoreCarryFrom(done).empty());
    CHECK(ScoreCarryFrom(MatchInfo{}).empty());
    // fighters without a SteamID (bots) are not carried
    MatchInfo bots = live; for (FighterEntry& f : bots.fighters) f.steamId = 0;
    CHECK(ScoreCarryFrom(bots).players.empty());
    // an empty board changes nothing
    Match plain; plain.Start(Cfg(Format::FreeForAll), Three(), 0, ScoreCarry{});
    CHECK(plain.scores().empty() && plain.Kills(1) == 0);
}

int main() {
    TestRejoinGetsScoreBack(); TestRejoinOnAnotherSlot(); TestDifferentPlayerStartsAtZero(); TestRematchForgetsLeavers();
    TestTeamsKeepTeamScore(); TestMigrationRestoresFreeForAll(); TestMigrationRestoresTeamsAndSurvival(); TestMigrationBoardGuards();
    if (g_fail) { std::printf("test_hsm_score_identity: %d failure(s)\n", g_fail); return 1; }
    std::puts("test_hsm_score_identity: all passed");
    return 0;
}

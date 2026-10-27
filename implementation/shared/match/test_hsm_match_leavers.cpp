// 0.7.0 round agent: leavers (K1/K2/K3), Duel countdown deaths (F2), trade kills (K7).
#include "match/hsm_match.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static MatchConfig Cfg(Format f, uint16_t limit, uint16_t respawnS = 5) {
    MatchConfig c; c.format = f; c.scoreLimit = limit; c.timeLimitS = 0; c.respawnDelayS = respawnS; return c;
}
static std::vector<FighterInfo> Two() { return {{1, kNoTeam, true}, {2, kNoTeam, true}}; }

static void TestDuelCountdownDeath() {
    Match m; m.Start(Cfg(Format::Duel, 3), Two(), 0);
    m.OnDeath(2, 0, 100);                                   // falls before the fight
    CHECK(!m.Alive(2));
    auto ev = m.Tick(500); CHECK(ev.respawn.empty());
    ev = m.Tick(1200); CHECK(ev.respawn.size() == 1 && ev.respawn[0] == 2);
    CHECK(m.Alive(2));
    ev = m.Tick(5000); CHECK(ev.live && m.phase() == MatchPhase::Live);
}

static void TestDuelLeave() {
    Match m; m.Start(Cfg(Format::Duel, 2, 3), Two(), 0);
    m.Tick(5000);
    CHECK(m.Leave(2, 6000));                                // 2 quits mid-fight
    CHECK(m.Gone(2) && !m.Alive(2) && m.Score(1) == 1 && m.RoundInPlay() == false);
    auto ev = m.Tick(9000);                                 // the gap: the leaver does not come back
    CHECK(ev.roundReset && ev.respawn.size() == 1 && ev.respawn[0] == 1);
    CHECK(!m.Alive(2));
    m.Tick(9100);                                           // the survivor takes the next round too, at once
    CHECK(m.Score(1) == 2 && m.phase() == MatchPhase::Results && m.winner() == 1);
}

static void TestLeaveThenReturn() {
    Match m; m.Start(Cfg(Format::FreeForAll, 5, 2), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    m.Tick(5000);
    CHECK(m.Leave(3, 6000) && m.Deaths(3) == 1);
    CHECK(!m.Leave(3, 6100));                               // already down
    auto ev = m.Tick(9000); CHECK(ev.respawn.empty());      // not respawned while gone
    m.Return(3);
    ev = m.Tick(9100); CHECK(ev.respawn.size() == 1 && ev.respawn[0] == 3);
}

static void TestLeaveOutsideCombat() {
    Match m; m.Start(Cfg(Format::FreeForAll, 5), Two(), 0);
    CHECK(!m.Leave(1, 100) && m.Alive(1));                  // countdown: nothing to record
}

static void TestSurvivalLastHumanLeaves() {
    Match m; m.Start(Cfg(Format::PlayersVsBots, 100), {{1, 0, true}}, 0);
    m.Tick(5000);
    CHECK(m.Leave(1, 6000));
    CHECK(m.phase() == MatchPhase::Results && m.winner() == kBotTeam);
}

static void TestRemoveClearsScore() {
    Match m; m.Start(Cfg(Format::FreeForAll, 5), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    m.Tick(5000);
    m.OnDeath(1, 2, 6000);
    CHECK(m.Score(2) == 1);
    m.RemoveFighter(2);
    CHECK(m.Score(2) == 0);                                 // the slot's next player starts at 0
    m.AddFighter({2, kNoTeam, true});
    CHECK(m.Score(2) == 0 && m.Kills(2) == 0);

    Match t; t.Start(Cfg(Format::TwoTeams, 5), {{1, 0, true}, {2, 1, true}}, 0);
    t.Tick(5000); t.OnDeath(2, 1, 6000);
    t.RemoveFighter(1);
    CHECK(t.Score(0) == 1);                                 // a team keeps its score
}

static void TestKillLogForget() {
    KillLog k; k.OnHit(2, 1, 1000); k.OnHit(3, 1, 1000); k.OnHit(1, 3, 1000);
    k.ForgetAttacker(1);
    CHECK(k.KillerOf(2, 2000) == 0 && k.KillerOf(3, 2000) == 0 && k.KillerOf(1, 2000) == 3);
}

static void TestTradeKill() {
    Match m; m.Start(Cfg(Format::FreeForAll, 1), Two(), 0);
    m.Tick(5000);
    m.OnDeath(2, 1, 6000);                                  // 1 reaches the limit ...
    m.OnDeath(1, 2, 6000);                                  // ... and 2 kills back in the same tick
    CHECK(m.phase() == MatchPhase::SuddenDeath && m.winner() == Match::kNoWinner);
    CHECK(m.Score(1) == 1 && m.Score(2) == 1);
    Match n; n.Start(Cfg(Format::FreeForAll, 1), Two(), 0);
    n.Tick(5000);
    n.OnDeath(2, 1, 6000);
    n.OnDeath(1, 0, 6500);                                  // later: no change
    CHECK(n.phase() == MatchPhase::Results && n.winner() == 1);
}

// K1/K2: a leaver back within its reservation is not spawned into the round it fell in. It is held until the match
// respawns it: the next Duel round, the next wave, or in FFA/teams the respawn delay (counted from its fall).
static void TestRejoinWaitsForRespawn() {
    Match d; d.Start(Cfg(Format::Duel, 5, 3), Two(), 0);
    d.Tick(5000);
    CHECK(!d.AwaitsRespawn(2));
    d.Leave(2, 6000);
    CHECK(!d.AwaitsRespawn(2));                             // still gone: not a respawn candidate yet
    d.Return(2);
    CHECK(d.AwaitsRespawn(2) && !d.AwaitsRespawn(1));       // back, but down for this round
    auto ev = d.Tick(7000); CHECK(ev.respawn.empty() && d.AwaitsRespawn(2));
    ev = d.Tick(9000);                                      // the round gap ends: the next round
    CHECK(ev.roundReset && ev.respawn.size() == 2);
    CHECK(!d.AwaitsRespawn(2));

    Match f; f.Start(Cfg(Format::FreeForAll, 9, 5), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    f.Tick(5000);
    f.Leave(3, 6000); f.Return(3);
    CHECK(f.AwaitsRespawn(3));
    ev = f.Tick(10999); CHECK(ev.respawn.empty() && f.AwaitsRespawn(3));
    ev = f.Tick(11000); CHECK(ev.respawn.size() == 1 && ev.respawn[0] == 3);   // the normal delay, from the fall
    f.MarkRespawned(3);                                     // RespawnEntity did it
    CHECK(!f.AwaitsRespawn(3));

    Match t; t.Start(Cfg(Format::TwoTeams, 9, 5), {{1, 0, true}, {2, 1, true}}, 0);
    t.Tick(5000);
    t.Leave(2, 6000); t.Return(2);
    CHECK(t.AwaitsRespawn(2));
    t.MarkRespawned(2);
    CHECK(!t.AwaitsRespawn(2));

    Match c; c.Start(Cfg(Format::FreeForAll, 9), Two(), 0);
    c.Leave(2, 100);                                        // countdown: nothing recorded, nothing to wait for
    CHECK(!c.AwaitsRespawn(2) && !c.AwaitsRespawn(7));
    c.Tick(5000); c.Finish(1, 5100);
    CHECK(!c.AwaitsRespawn(2));                             // not a fought phase
}

int main() {
    TestRejoinWaitsForRespawn();
    TestDuelCountdownDeath(); TestDuelLeave(); TestLeaveThenReturn(); TestLeaveOutsideCombat();
    TestSurvivalLastHumanLeaves(); TestRemoveClearsScore(); TestKillLogForget(); TestTradeKill();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

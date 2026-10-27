// P5: the free-for-all / teams give-up. A downed fighter that holds G dies on purpose; the death within kGaveUpWindowMs is
// scored as a give-up: the last attacker (KillLog, 60 s) gets the kill, with no attacker nobody scores, never -1.
#include "match/hsm_match.h"
#include "match/hsm_hud_scene.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static MatchConfig Cfg(Format f) { MatchConfig c; c.format = f; c.scoreLimit = 5; return c; }

// What HandleDeath resolves for a give-up: the mark decides the flag, KillerOf the killer (self never named).
static uint8_t GiveUpKiller(const KillLog& k, uint8_t victim, uint32_t now) { return k.KillerOf(victim, now); }

static void TestWindow() {
    CHECK(GaveUpDeath(1000, 1000) && GaveUpDeath(1000, 1000 + kGaveUpWindowMs));
    CHECK(!GaveUpDeath(1000, 1001 + kGaveUpWindowMs));
    CHECK(!GaveUpDeath(5000, 4000));                                  // a mark ahead of now (clock stepped back) is stale
    CHECK(GaveUpDeath(0xFFFFFF00u, 0x100u));                          // across the tick-count wrap
    CHECK(!GaveUpDeath(0xFFFFFF00u, 0x10000u));
}

static void TestFfaGiveUpScoring() {
    Match m; m.Start(Cfg(Format::FreeForAll), {{1, kNoTeam, true}, {2, kNoTeam, true}, {3, kNoTeam, true}}, 0);
    m.Tick(Match::kCountdownMs);
    const uint32_t t = Match::kCountdownMs;
    KillLog log;
    log.OnHit(2, 3, t + 100);                                          // 3 wounded 2
    const uint8_t killer = GiveUpKiller(log, 2, t + 40000);            // 2 gives up 40 s later: still 3's kill
    CHECK(killer == 3);
    m.OnDeath(2, killer, t + 40000);
    CHECK(m.Score(3) == 1 && m.Kills(3) == 1 && m.Deaths(2) == 1 && m.Score(2) == 0 && !m.Alive(2));
    m.MarkRespawned(2);
    // No recent attacker (the blow was more than 60 s ago, or none): nobody scores, the victim only dies.
    log.OnHit(1, 3, t + 100);
    CHECK(GiveUpKiller(log, 1, t + 100 + KillLog::kWindowMs + 1) == 0);
    m.OnDeath(1, 0, t + 70000);
    CHECK(m.Score(1) == 0 && m.Score(2) == 0 && m.Score(3) == 1 && m.Deaths(1) == 1 && !m.Alive(1));
    CHECK(m.phase() == MatchPhase::Live);
}

static void TestTeamsGiveUp() {
    Match m; m.Start(Cfg(Format::TwoTeams), {{1, 0, true}, {2, 1, true}, {3, 0, true}, {4, 1, true}}, 0);
    m.Tick(Match::kCountdownMs);
    const uint32_t t = Match::kCountdownMs;
    m.OnDeath(2, 3, t + 10);                                           // gave up after a teammate-of-1 blow: team 0 scores
    CHECK(m.Score(0) == 1 && m.Score(1) == 0);
    m.OnDeath(4, 0, t + 20);                                           // gave up with nobody to credit
    CHECK(m.Score(0) == 1 && m.Score(1) == 0 && m.Deaths(4) == 1);
}

static void TestKillLogNeverNamesSelf() {
    KillLog log;
    log.OnHit(2, 2, 100);                                              // a self-hit is never an attacker
    CHECK(log.KillerOf(2, 200) == 0);
    log.OnHit(2, 5, 300);
    CHECK(log.KillerOf(2, 300 + KillLog::kWindowMs) == 5 && log.KillerOf(2, 301 + KillLog::kWindowMs) == 0);
}

static void TestHudWaitPill() {
    MatchInfo mi; mi.format = uint8_t(Format::Duel); mi.phase = uint8_t(MatchPhase::Live); mi.roundGapLeftMs = 4000;
    LocalHud l; l.giveUpWait = true;
    HudScene s = BuildHudScene(mi, l, 1000);
    CHECK(s.pill.visible && s.pill.text.find("NEXT ROUND IN") != std::string::npos);
    l.giveUpWait = false;
    CHECK(BuildHudScene(mi, l, 1000).pill.text.find("NOTHING TO GIVE UP") == std::string::npos);
}

int main() {
    TestWindow(); TestFfaGiveUpScoring(); TestTeamsGiveUp(); TestKillLogNeverNamesSelf(); TestHudWaitPill();
    std::printf(g_fail ? "%d FAILED\n" : "all give-up ffa tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

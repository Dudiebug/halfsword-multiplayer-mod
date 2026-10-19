// Unit tests for hsm_level_gate.h. Run via implementation/shared/run_tests.sh test_hsm_level_gate.
#include "session/hsm_level_gate.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static LevelProbe P(bool manager, bool spawned, bool pawn) { LevelProbe p; p.manager = manager; p.playerSpawned = spawned; p.pawn = pawn; return p; }

// The level-start timeline seen in the first two-player test: manager present, flag cleared by Clean Up Map, then set
// once the stock spawner has possessed the player.
static void TestOpensAfterStockSpawn() {
    LevelSpawnGate g; g.OnLevel(1000);
    CHECK(!g.Open());
    CHECK(!g.Update(1050, P(true, false, false)));
    CHECK(!g.Update(1600, P(true, false, true)));    // a pawn, but the level has not spawned its player yet
    CHECK(!g.Open());
    CHECK(g.Update(2200, P(true, true, true)));      // opens on this update
    CHECK(g.Open());
    CHECK(g.why() == LevelSpawnGate::Why::LevelSpawned);
    CHECK(!g.Update(2300, P(true, true, true)));     // only the opening update returns true
    CHECK(!g.Update(2400, P(true, false, false)));   // and it stays open for the rest of the level
    CHECK(g.Open());
}

// "Player Spawned" already set when first seen (a value left from before the first Clean Up Map) is not trusted: the
// cleanup that follows would still wipe what the mod spawns.
static void TestStaleFlagIgnored() {
    LevelSpawnGate g; g.OnLevel(0);
    CHECK(!g.Update(10, P(true, true, true)));
    CHECK(!g.Update(500, P(true, true, true)));
    CHECK(!g.Update(600, P(true, false, true)));     // Clean Up Map
    CHECK(g.Update(1500, P(true, true, true)));
}

static void TestNeedsPawn() {
    LevelSpawnGate g; g.OnLevel(0);
    CHECK(!g.Update(100, P(true, false, false)));
    CHECK(!g.Update(1200, P(true, true, false)));
    CHECK(!g.Open());
    CHECK(g.Update(1300, P(true, true, true)));
}

static void TestNoManagerFallback() {
    LevelSpawnGate g; g.OnLevel(1000);
    CHECK(!g.Update(1000 + LevelSpawnGate::kNoManagerMs - 1, P(false, false, true)));
    CHECK(g.Update(1000 + LevelSpawnGate::kNoManagerMs, P(false, false, true)));
    CHECK(g.why() == LevelSpawnGate::Why::NoManager);
}

// A manager seen at any point means the no-manager fallback no longer applies (it may just be between probes).
static void TestManagerSeenThenMissing() {
    LevelSpawnGate g; g.OnLevel(0);
    CHECK(!g.Update(100, P(true, false, false)));
    CHECK(!g.Update(LevelSpawnGate::kNoManagerMs + 100, P(false, false, true)));
    CHECK(!g.Open());
}

static void TestTimeout() {
    LevelSpawnGate g; g.OnLevel(0);
    CHECK(!g.Update(100, P(true, false, true)));
    CHECK(!g.Update(LevelSpawnGate::kMaxWaitMs - 1, P(true, false, true)));
    CHECK(g.Update(LevelSpawnGate::kMaxWaitMs, P(true, false, true)));
    CHECK(g.why() == LevelSpawnGate::Why::TimedOut);
}

static void TestNewLevelCloses() {
    LevelSpawnGate g; g.OnLevel(0);
    g.Update(100, P(true, false, true));
    CHECK(g.Update(200, P(true, true, true)));
    g.OnLevel(10000);
    CHECK(!g.Open());
    CHECK(g.why() == LevelSpawnGate::Why::Closed);
    CHECK(!g.Update(10100, P(true, true, true)));    // the new level's flag must be seen cleared again
    CHECK(!g.Update(10000 + LevelSpawnGate::kNoManagerMs + 1, P(false, false, true)));   // manager was seen this level
    CHECK(g.Update(10000 + LevelSpawnGate::kMaxWaitMs, P(true, true, true)));
}

// Before the first OnLevel the clock starts at the first update.
static void TestClockStartsAtFirstUpdate() {
    LevelSpawnGate g;
    CHECK(!g.Open());
    CHECK(!g.Update(50000, P(false, false, false)));
    CHECK(!g.Update(50000 + LevelSpawnGate::kNoManagerMs - 1, P(false, false, false)));
    CHECK(g.Update(50000 + LevelSpawnGate::kNoManagerMs, P(false, false, false)));
}

// The timeout log names what the level never did (test 4: the client waited the full 15 s; the log did not say why).
static void TestWaitingFor() {
    using W = LevelSpawnGate::Waiting;
    {   // no level manager yet
        LevelSpawnGate g; g.OnLevel(0);
        CHECK(g.waitingFor() == W::Manager);
        CHECK(!g.Update(100, P(false, false, true)));
        CHECK(g.waitingFor() == W::Manager);
    }
    {   // "Player Spawned" never seen cleared: no Clean Up Map seen
        LevelSpawnGate g; g.OnLevel(0);
        CHECK(!g.Update(100, P(true, true, true)));
        CHECK(g.Update(LevelSpawnGate::kMaxWaitMs, P(true, true, true)));
        CHECK(g.why() == LevelSpawnGate::Why::TimedOut);
        CHECK(g.waitingFor() == W::PlayerSpawnedCleared);
    }
    {   // cleared and never set again: the stock spawner never ran
        LevelSpawnGate g; g.OnLevel(0);
        CHECK(!g.Update(100, P(true, false, true)));
        CHECK(g.Update(LevelSpawnGate::kMaxWaitMs, P(true, false, true)));
        CHECK(g.waitingFor() == W::PlayerSpawned);
    }
    {   // cleared, never set again, and no fighter either: the spawner that would set both never ran
        LevelSpawnGate g; g.OnLevel(0);
        CHECK(!g.Update(100, P(true, false, false)));
        CHECK(g.Update(LevelSpawnGate::kMaxWaitMs, P(true, false, false)));
        CHECK(g.waitingFor() == W::PlayerSpawned);
    }
    {   // set again, but player 0 has no fighter
        LevelSpawnGate g; g.OnLevel(0);
        CHECK(!g.Update(100, P(true, false, false)));
        CHECK(!g.Update(200, P(true, true, false)));
        CHECK(g.Update(LevelSpawnGate::kMaxWaitMs, P(true, true, false)));
        CHECK(g.waitingFor() == W::Pawn);
    }
    {   // the manager was seen, then missing from the last probe
        LevelSpawnGate g; g.OnLevel(0);
        CHECK(!g.Update(100, P(true, false, true)));
        CHECK(g.Update(LevelSpawnGate::kMaxWaitMs, P(false, false, true)));
        CHECK(g.waitingFor() == W::Manager);
    }
    {   // opened by the level, or by the no-manager rule: nothing was missing
        LevelSpawnGate g; g.OnLevel(0);
        CHECK(!g.Update(100, P(true, false, true)));
        CHECK(g.Update(200, P(true, true, true)));
        CHECK(g.waitingFor() == W::Nothing);
        LevelSpawnGate h; h.OnLevel(0);
        CHECK(h.Update(LevelSpawnGate::kNoManagerMs, P(false, false, true)));
        CHECK(h.waitingFor() == W::Nothing);
    }
}

// The last probe is kept for the log and frozen once the gate opens.
static void TestLastProbe() {
    LevelSpawnGate g; g.OnLevel(0);
    CHECK(!g.Update(100, P(true, false, true)));
    CHECK(g.lastProbe().manager && !g.lastProbe().playerSpawned && g.lastProbe().pawn);
    CHECK(g.Update(LevelSpawnGate::kMaxWaitMs, P(true, false, false)));
    CHECK(g.lastProbe().manager && !g.lastProbe().playerSpawned && !g.lastProbe().pawn);
    CHECK(!g.Update(LevelSpawnGate::kMaxWaitMs + 100, P(false, true, true)));   // after opening: not kept
    CHECK(g.lastProbe().manager && !g.lastProbe().playerSpawned && !g.lastProbe().pawn);
    g.OnLevel(20000);
    CHECK(!g.lastProbe().manager && !g.lastProbe().playerSpawned && !g.lastProbe().pawn);
    CHECK(g.waitingFor() == LevelSpawnGate::Waiting::Manager);
}

// Test 4: the client's own 15 s fallback and the gate's 15 s timeout both spawned its fighter, 60 ms apart.
static void TestLocalFighterGrace() {
    double grace = -1;
    CHECK(!LocalFighterGrace(true, false, grace));   // owner-sync client, gate closed: the gate's exit spawns it
    grace = -1;
    CHECK(LocalFighterGrace(true, true, grace));
    CHECK(grace == kAnnouncedGraceMs);               // gate open: a fighter lost later comes back soon
    grace = -1;
    CHECK(LocalFighterGrace(false, false, grace));
    CHECK(grace == kLocalFighterGraceMs);            // host, solo, host-sync client: after the stock spawner
    grace = -1;
    CHECK(LocalFighterGrace(false, true, grace));
    CHECK(grace == kLocalFighterGraceMs);
    CHECK(kAnnouncedGraceMs < kLocalFighterGraceMs);
}

static void TestRecreateLimiter() {
    RecreateLimiter r;
    CHECK(r.Allow(3, 1000));
    CHECK(!r.Allow(3, 1000 + RecreateLimiter::kRetryMs - 1));
    CHECK(r.Allow(4, 1500));                          // per entity
    CHECK(r.Allow(3, 1000 + RecreateLimiter::kRetryMs));
    CHECK(!r.Allow(3, 1000 + RecreateLimiter::kRetryMs + 1));
    r.Clear();
    CHECK(r.Allow(3, 1000 + RecreateLimiter::kRetryMs + 2));
    CHECK(r.Allow(4, 1600));                          // cleared too
    CHECK(!r.Allow(4, 1601));
}

int main() {
    TestOpensAfterStockSpawn();
    TestStaleFlagIgnored();
    TestNeedsPawn();
    TestNoManagerFallback();
    TestManagerSeenThenMissing();
    TestTimeout();
    TestNewLevelCloses();
    TestClockStartsAtFirstUpdate();
    TestWaitingFor();
    TestLastProbe();
    TestLocalFighterGrace();
    TestRecreateLimiter();
    if (g_fail) { std::printf("test_hsm_level_gate: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_level_gate: ok\n");
    return 0;
}

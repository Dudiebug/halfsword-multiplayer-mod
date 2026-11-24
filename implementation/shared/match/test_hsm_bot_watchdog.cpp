// The bot watchdog (#2413): a bot below the floor, a bot that has not moved for a long time with no player near, and a
// wave where nothing has happened for a long time.
#include "match/hsm_bot_watchdog.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestDefaultsAreConservative() {
    const BotWatchLimits l;
    CHECK(l.belowCm == 2000 && l.stillCm == 40 && l.stillMs == 90000 && l.engagedCm == 600 && l.waveStallMs == 300000);
}

static void TestBelowFloor() {
    BotWatch w;
    w.Arm(5, 100, 0);
    CHECK(w.Observe(5, 0, 0, 100, -1, 1000) == BotVerdict::Ok);
    CHECK(w.Observe(5, 0, 0, 100 - 2000, -1, 2000) == BotVerdict::Ok);        // exactly the limit is still the floor
    CHECK(w.Observe(5, 0, 0, 100 - 2000.5, -1, 3000) == BotVerdict::BelowFloor);
    CHECK(w.Observe(5, 0, 0, 100 - 5000, 10, 4000) == BotVerdict::BelowFloor);   // a player next to it does not excuse a fall
    BotWatch f;                                         // first seen already under the world
    f.Arm(7, 0, 0);
    CHECK(f.Observe(7, 0, 0, -3000, -1, 1000) == BotVerdict::BelowFloor);
}

static void TestSpawnHeightIsWhereItWasArmed() {
    BotWatch w;
    w.Arm(5, 500, 0);                                  // spawned high up; the first look finds it lower, on the floor
    CHECK(w.Observe(5, 0, 0, 0, -1, 1000) == BotVerdict::Ok);
    CHECK(w.Observe(5, 0, 0, 500 - 2100, -1, 2000) == BotVerdict::BelowFloor);   // measured from 500, not from 0
    BotWatch u;                                         // never armed: the first look is the reference
    CHECK(u.Observe(6, 0, 0, 0, -1, 1000) == BotVerdict::Ok);
    CHECK(u.Observe(6, 0, 0, -1999, -1, 2000) == BotVerdict::Ok);
    CHECK(u.Observe(6, 0, 0, -2001, -1, 3000) == BotVerdict::BelowFloor);
    BotWatch h;                                         // first seen high up: the reference is that height, not zero
    CHECK(h.Observe(8, 0, 0, 5000, -1, 1000) == BotVerdict::Ok);
    CHECK(h.Observe(8, 0, 0, 3000, -1, 2000) == BotVerdict::Ok);
    CHECK(h.Observe(8, 0, 0, 2999, -1, 3000) == BotVerdict::BelowFloor);
}

static void TestStill() {
    BotWatch w;
    CHECK(w.Observe(5, 10, 20, 0, -1, 1000) == BotVerdict::Ok);
    CHECK(w.Observe(5, 10, 20, 0, -1, 1000 + 89999) == BotVerdict::Ok);
    CHECK(w.Observe(5, 10, 20, 0, -1, 1000 + 90000) == BotVerdict::Still);
    CHECK(w.Observe(5, 10, 20, 0, -1, 1000 + 200000) == BotVerdict::Still);    // and stays so until it is dealt with
}

static void TestMovingResetsTheClock() {
    BotWatch w;
    w.Observe(5, 0, 0, 0, -1, 0);
    CHECK(w.Observe(5, 0, 0, 0, -1, 80000) == BotVerdict::Ok);
    CHECK(w.Observe(5, 41, 0, 0, -1, 85000) == BotVerdict::Ok);                // moved past the limit: a new mark
    CHECK(w.Observe(5, 41, 0, 0, -1, 85000 + 89999) == BotVerdict::Ok);
    CHECK(w.Observe(5, 41, 0, 0, -1, 85000 + 90000) == BotVerdict::Still);
}

static void TestSmallDriftIsStillStuck() {
    BotWatch w;
    w.Observe(5, 0, 0, 0, -1, 0);
    CHECK(w.Observe(5, 40, 0, 0, -1, 50000) == BotVerdict::Ok);                // exactly 40 cm: it did not move
    CHECK(w.Observe(5, 0, 30, 0, -1, 90000) == BotVerdict::Still);             // jiggling in place does not count either
    BotWatch v;
    v.Observe(5, 0, 0, 0, -1, 0);
    CHECK(v.Observe(5, 0, 0, 41, -1, 80000) == BotVerdict::Ok);                // height counts as movement
    CHECK(v.Observe(5, 0, 0, 41, -1, 80000 + 89999) == BotVerdict::Ok);
    CHECK(v.Observe(5, 0, 0, 41, -1, 80000 + 90000) == BotVerdict::Still);
}

static void TestPlayerNearbyMeansFighting() {
    BotWatch w;
    w.Observe(5, 0, 0, 0, 100, 0);
    CHECK(w.Observe(5, 0, 0, 0, 600, 200000) == BotVerdict::Ok);               // a player at 600 cm: engaged
    CHECK(w.Observe(5, 0, 0, 0, -1, 200000 + 89999) == BotVerdict::Ok);        // the clock restarted while it was engaged
    CHECK(w.Observe(5, 0, 0, 0, 601, 200000 + 90000) == BotVerdict::Still);    // 601 cm is not engaged
    BotWatch v;
    v.Observe(5, 0, 0, 0, -1, 0);
    CHECK(v.Observe(5, 0, 0, 0, 0, 100000) == BotVerdict::Ok);                 // zero distance is a player on top of it
}

static void TestForget() {
    BotWatch w;
    w.Arm(5, 0, 0);
    w.Observe(5, 0, 0, 0, -1, 0);
    CHECK(w.Size() == 1);
    w.Forget(5);
    CHECK(w.Size() == 0);
    CHECK(w.Observe(5, 0, 0, 0, -1, 500000) == BotVerdict::Ok);                // a reused entity starts afresh
    w.Clear();
    CHECK(w.Size() == 0);
}

static void TestEntitiesAreIndependent() {
    BotWatch w;
    w.Observe(5, 0, 0, 0, -1, 0);
    w.Observe(6, 0, 0, 0, -1, 80000);
    CHECK(w.Observe(5, 0, 0, 0, -1, 90000) == BotVerdict::Still);
    CHECK(w.Observe(6, 0, 0, 0, -1, 90000) == BotVerdict::Ok);
}

static void TestWrap() {
    BotWatch w;
    w.Observe(5, 0, 0, 0, -1, 0xFFFFF000u);
    CHECK(w.Observe(5, 0, 0, 0, -1, 0xFFFFF000u + 89999) == BotVerdict::Ok);
    CHECK(w.Observe(5, 0, 0, 0, -1, 0xFFFFF000u + 90000) == BotVerdict::Still);   // past the 32-bit wrap
    CHECK(WaveStalled(0xFFFFFF00u, 0xFFFFFF00u + 300000, 1));
    CHECK(!WaveStalled(0xFFFFFF00u, 0xFFFFFF00u + 299999, 1));
}

static void TestWaveStalled() {
    CHECK(!WaveStalled(1000, 1000 + 299999, 3));
    CHECK(WaveStalled(1000, 1000 + 300000, 3));
    CHECK(!WaveStalled(1000, 1000 + 900000, 0));                               // no bot alive: nothing to kill
    CHECK(WaveStalled(1000, 1000 + 300000, 1));
}

// The Pit (#3370): a bot that gave up 3 m or more under its spawn height for 3 s is out; one that yields on the floor, or
// lies under it without giving up (the 20 m rule's case), is not.
static void TestOutBelow() {
    BotWatch w;
    w.Arm(5, 100, 0);
    for (uint32_t t = 1000; t <= 60000; t += 1000) CHECK(!w.OutBelow(5, 100 - 120, true, t));   // yielded, lying on the floor
    CHECK(!w.OutBelow(5, 100 - 299, true, 61000) && !w.OutBelow(5, 100 - 299, true, 70000));   // just short of 3 m
    CHECK(!w.OutBelow(5, 100 - 300, true, 71000));                                     // exactly 3 m under: the count starts
    CHECK(!w.OutBelow(5, 100 - 600, true, 73999));
    CHECK(w.OutBelow(5, 100 - 600, true, 74000));                                      // held for 3 s
    BotWatch n;
    n.Arm(6, 100, 0);
    for (uint32_t t = 1000; t <= 30000; t += 1000) CHECK(!n.OutBelow(6, -1500, false, t));   // under it, not given up
    // The count starts again when the flag goes off, or the bot climbs back up.
    BotWatch r;
    r.Arm(7, 0, 0);
    CHECK(!r.OutBelow(7, -400, true, 1000));
    CHECK(!r.OutBelow(7, -400, false, 3000));
    CHECK(!r.OutBelow(7, -400, true, 3500) && !r.OutBelow(7, -400, true, 6499) && r.OutBelow(7, -400, true, 6500));
    BotWatch c;
    c.Arm(8, 0, 0);
    CHECK(!c.OutBelow(8, -400, true, 1000));
    CHECK(!c.OutBelow(8, -100, true, 3000));
    CHECK(!c.OutBelow(8, -400, true, 3500) && c.OutBelow(8, -400, true, 6500));
    // Measured from where it was armed; a bot never armed takes its first height.
    BotWatch h;
    h.Arm(9, 500, 0);
    CHECK(!h.OutBelow(9, 250, true, 1000) && !h.OutBelow(9, 250, true, 9000));
    CHECK(!h.OutBelow(9, 200, true, 10000) && h.OutBelow(9, 200, true, 13000));
    BotWatch u;
    CHECK(!u.OutBelow(4, 1000, true, 0) && !u.OutBelow(4, 1000, true, 10000) && !u.OutBelow(4, 701, true, 20000));
    CHECK(!u.OutBelow(4, 700, true, 30000) && u.OutBelow(4, 700, true, 33000));
    double z = 1;
    CHECK(!u.SpawnZ(3, z) && z == 1 && u.SpawnZ(4, z) && z == 1000 && h.SpawnZ(9, z) && z == 500);
    u.NoteLoseMatch(5);                                                                // a mark, but no height yet
    z = 1;
    CHECK(!u.SpawnZ(5, z) && z == 1);
    // The lose volume's call counts as given up (a zombie never gets Give Up), until the entity is armed again.
    BotWatch z1;
    z1.Arm(10, 100, 0);
    CHECK(!z1.OutBelow(10, -400, false, 1000));
    z1.NoteLoseMatch(10);
    CHECK(!z1.OutBelow(10, -400, false, 2000) && !z1.OutBelow(10, -400, false, 4999) && z1.OutBelow(10, -400, false, 5000));
    CHECK(!z1.OutBelow(10, 50, false, 6000));                                          // on the floor: still not out
    z1.Arm(10, 100, 7000);
    CHECK(!z1.OutBelow(10, -400, false, 7000) && !z1.OutBelow(10, -400, false, 20000));   // a new bot in the entity
    // Forget: a reused entity starts afresh.
    w.Forget(5);
    w.Arm(5, 100, 80000);
    CHECK(!w.OutBelow(5, -500, true, 80000) && !w.OutBelow(5, -500, true, 82999) && w.OutBelow(5, -500, true, 83000));
    CHECK(BotWatchLimits{}.outBelowCm == 300 && BotWatchLimits{}.outMs == 3000);
}

int main() {
    TestDefaultsAreConservative(); TestBelowFloor(); TestSpawnHeightIsWhereItWasArmed(); TestStill(); TestMovingResetsTheClock(); TestSmallDriftIsStillStuck();
    TestPlayerNearbyMeansFighting(); TestForget(); TestEntitiesAreIndependent(); TestWrap(); TestWaveStalled(); TestOutBelow();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

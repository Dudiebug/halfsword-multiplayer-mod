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

int main() {
    TestDefaultsAreConservative(); TestBelowFloor(); TestSpawnHeightIsWhereItWasArmed(); TestStill(); TestMovingResetsTheClock(); TestSmallDriftIsStillStuck();
    TestPlayerNearbyMeansFighting(); TestForget(); TestEntitiesAreIndependent(); TestWrap(); TestWaveStalled();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

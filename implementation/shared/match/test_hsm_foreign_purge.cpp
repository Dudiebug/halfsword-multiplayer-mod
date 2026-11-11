// When a client purges the fighters of its own copy of the level (#2754).
#include "match/hsm_foreign_purge.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestDefaults() {
    const ForeignPurgeTiming t;
    CHECK(t.burstMs == 6000 && t.steadyMs == 1000);
}

// The report: a lobby between matches (no match running) kept a foe only this player saw.
static void TestLobbyBetweenMatchesIsPurged() {
    CHECK(ForeignPurgeDue(true, false, 60000, 1000));
    CHECK(!ForeignPurgeDue(true, false, 60000, 999));
    CHECK(ForeignPurgeDue(true, true, 60000, 1000));
    CHECK(!ForeignPurgeDue(true, true, 60000, 999));
}

static void TestEveryFrameInTheFirstSeconds() {
    CHECK(ForeignPurgeDue(true, false, 0, 0));          // the frame the start-up ended, right after a purge
    CHECK(ForeignPurgeDue(true, false, 16, 16));
    CHECK(ForeignPurgeDue(true, true, 5999, 1));
    CHECK(ForeignPurgeDue(true, false, 5999, 0));
    CHECK(!ForeignPurgeDue(true, false, 6000, 0));      // the window is over: the steady pace
    CHECK(!ForeignPurgeDue(true, true, 6000, 999));
    CHECK(ForeignPurgeDue(true, true, 6000, 1000));
}

static void TestOldRuleWhenEarlyIsOff() {
    CHECK(!ForeignPurgeDue(false, false, 0, 5000));     // no match running: never
    CHECK(!ForeignPurgeDue(false, false, 60000, 60000));
    CHECK(!ForeignPurgeDue(false, true, 0, 999));       // a match: once a second, even in the first seconds
    CHECK(!ForeignPurgeDue(false, true, 100, 0));
    CHECK(ForeignPurgeDue(false, true, 0, 1000));
    CHECK(ForeignPurgeDue(false, true, 60000, 1000));
}

static void TestCustomTiming() {
    ForeignPurgeTiming t; t.burstMs = 100; t.steadyMs = 50;
    CHECK(ForeignPurgeDue(true, false, 99, 0, t));
    CHECK(!ForeignPurgeDue(true, false, 100, 49, t));
    CHECK(ForeignPurgeDue(true, false, 100, 50, t));
    CHECK(ForeignPurgeDue(false, true, 0, 50, t));
    CHECK(!ForeignPurgeDue(false, true, 0, 49, t));
}

int main() {
    TestDefaults(); TestLobbyBetweenMatchesIsPurged(); TestEveryFrameInTheFirstSeconds(); TestOldRuleWhenEarlyIsOff(); TestCustomTiming();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

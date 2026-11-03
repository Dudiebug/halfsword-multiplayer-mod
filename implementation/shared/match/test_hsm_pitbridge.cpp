// Pit Bridge Duels: the bridge's ends from a board's bounds, which board is the bridge, which side each fighter takes.
#include "match/hsm_pitbridge.h"
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestEndsAlongX() {
    const BridgeEnds e = BridgeEndsFrom({1000, 2000, 300}, {300, 50, 10});       // a 6 m board along X
    CHECK(e.ok);
    CHECK(e.a.x == 1000 - 225 && e.b.x == 1000 + 225 && e.a.y == 2000 && e.b.y == 2000);   // 75 cm (a quarter of the half length) in from each tip
    CHECK(e.a.z == 310 && e.b.z == 310);                                              // standing on the top
}

static void TestEndsAlongY() {
    const BridgeEnds e = BridgeEndsFrom({-500, 0, 0}, {40, 300, 5});
    CHECK(e.ok && e.a.y == -225 && e.b.y == 225 && e.a.x == -500 && e.b.x == -500);
}

static void TestEndsAreApart() {
    const BridgeEnds e = BridgeEndsFrom({0, 0, 0}, {300, 50, 10});
    CHECK(e.ok && Dist2(e.a, e.b) > 300.0 * 300.0);                                   // well apart
}

static void TestShortBoardKeepsAQuarter() {
    const BridgeEnds e = BridgeEndsFrom({0, 0, 0}, {240, 20, 0});
    CHECK(e.ok && e.b.x == 240 - 60);                                                 // 60 cm, a quarter of 240
}

static void TestNotABridge() {
    CHECK(!BridgeEndsFrom({0, 0, 0}, {100, 20, 5}).ok);                               // too short
    CHECK(!BridgeEndsFrom({0, 0, 0}, {300, 200, 5}).ok);                              // not clearly longer than wide
    CHECK(!BridgeEndsFrom({0, 0, 0}, {250, 250, 5}).ok);                              // square
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    CHECK(!BridgeEndsFrom({nan, 0, 0}, {300, 20, 5}).ok);
    CHECK(!BridgeEndsFrom({0, 0, 0}, {inf, 20, 5}).ok);
    CHECK(!BridgeEndsFrom({0, 0, 0}, {300, nan, 5}).ok);
}

static void TestPickBridge() {
    std::vector<BridgeEnds> boards = {BridgeEndsFrom({0, 0, 0}, {250, 20, 5}), BridgeEndsFrom({0, 0, 0}, {10, 10, 5}),
                                      BridgeEndsFrom({0, 0, 0}, {300, 20, 5})};
    CHECK(!boards[1].ok);
    CHECK(PickBridge(boards, {250, 10, 300}) == 2);
    CHECK(PickBridge(boards, {300, 400, 300}) == 0);                                  // a box that is no board never wins
    CHECK(PickBridge({}, {}) == -1);
    CHECK(PickBridge({boards[1]}, {10}) == -1);
}

static void TestSides() {
    CHECK(PitSideOf(0) == 0 && PitSideOf(1) == 1 && PitSideOf(7) == 1);
}

int main() {
    TestEndsAlongX();
    TestEndsAlongY();
    TestEndsAreApart();
    TestShortBoardKeepsAQuarter();
    TestNotABridge();
    TestPickBridge();
    TestSides();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

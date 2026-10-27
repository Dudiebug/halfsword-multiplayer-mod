// Unit tests for the flail tolerance of hsm_held.h. Run via implementation/shared/run_tests.sh test_hsm_held_flail.
#include "sync/hsm_held.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static Q4 AxisAngle(V3 axis, double deg) {
    const double h = deg * 3.14159265358979323846 / 360.0;
    const double s = std::sin(h);
    return Q4{axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}

static void TestToleranceValues() {
    const HeldTolerance d = HeldToleranceFor(false), f = HeldToleranceFor(true);
    CHECK(d.deg == kHeldReseatDeg && d.uu == kHeldReseatUu && d.minGapMs == kHeldMinGapMs);
    CHECK(f.deg > d.deg && f.uu > d.uu && f.minGapMs > d.minGapMs);
}

static void TestFlailIgnoresChainError() {   // 30 deg and 15 uu: a re-seat for a plain weapon, the chain's doing for a flail
    const HeldPose owner{V3{0, 0, 0}, Q4{}};
    const HeldPose mine{V3{15, 0, 0}, AxisAngle(V3{0, 0, 1}, 30)};
    HeldCheck plain, flail;
    CHECK(!plain.Update(0, owner, mine));
    CHECK(plain.Update(400, owner, mine));
    CHECK(!flail.Update(0, owner, mine, HeldToleranceFor(true)));
    CHECK(!flail.Update(400, owner, mine, HeldToleranceFor(true)));
    CHECK(!flail.Update(5000, owner, mine, HeldToleranceFor(true)));
}

static void TestFlailGap() {   // a flail seat far enough off is re-seated, then not again for 3 s
    const HeldPose owner{V3{0, 0, 0}, Q4{}};
    const HeldPose mine{V3{40, 0, 0}, Q4{}};
    const HeldPose owner2{V3{0, 30, 0}, Q4{}};   // the owner moved the weapon in the hand: another seat
    const HeldTolerance tol = HeldToleranceFor(true);
    HeldCheck c;
    CHECK(!c.Update(10000, owner, mine, tol));
    CHECK(c.Update(10400, owner, mine, tol));
    CHECK(!c.Update(11000, owner2, mine, tol));
    CHECK(!c.Update(11400, owner2, mine, tol));   // persisted, but 1.4 s after the last seat
    CHECK(!c.Update(12900, owner2, mine, tol));   // 2.5 s
    CHECK(c.Update(13500, owner2, mine, tol));    // 3.1 s
}

int main() {
    TestToleranceValues();
    TestFlailIgnoresChainError();
    TestFlailGap();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_held_flail: ok\n");
    return 0;
}

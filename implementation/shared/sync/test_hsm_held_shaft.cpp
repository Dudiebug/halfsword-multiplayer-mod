// Unit tests for the mid-shaft slide tolerance of hsm_held.h. Run via implementation/shared/run_tests.sh test_hsm_held_shaft.
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

// A spear along hand-frame X, its root 80 uu behind the hand: gripped mid-shaft.
static const HeldPose kSpear{V3{-80, 0, 0}, Q4{}};
static const V3 kBase{-80, 0, 0}, kTip{120, 0, 0};

static void TestSpearGetsSlide() {
    const HeldTolerance t = HeldToleranceForShaft(HeldToleranceFor(false), kSpear, Q4{}, kBase, kTip);
    CHECK(t.slideUu == kHeldSlideUu);
    CHECK(std::fabs(t.axis.x - 1.0) < 1e-9 && std::fabs(t.axis.y) < 1e-9 && std::fabs(t.axis.z) < 1e-9);
}

static void TestAxisInHandFrame() {   // the hand turned 90 deg about Z: the world X shaft is the hand's -Y
    const Q4 hand = AxisAngle(V3{0, 0, 1}, 90);
    const HeldPose owner{V3{0, 80, 0}, Q4{}};
    const HeldTolerance t = HeldToleranceForShaft(HeldToleranceFor(false), owner, hand, V3{-80, 0, 0}, V3{120, 0, 0});
    CHECK(t.slideUu == kHeldSlideUu);
    CHECK(std::fabs(t.axis.y + 1.0) < 1e-9);
}

static void TestSwordKeepsPlain() {   // held at the hilt, by the root: no slide
    const HeldPose sword{V3{-5, 0, 0}, Q4{}};
    const HeldTolerance t = HeldToleranceForShaft(HeldToleranceFor(false), sword, Q4{}, V3{-5, 0, 0}, V3{95, 0, 0});
    CHECK(t.slideUu == 0.0);
    const HeldPose slid{V3{-30, 0, 0}, Q4{}};
    CHECK(HeldDiffers(sword, slid, t));   // 25 uu along the blade: still made again
}

static void TestShortOrMissingTip() {   // no tip read (base == tip): no direction, plain tolerance
    const HeldTolerance t = HeldToleranceForShaft(HeldToleranceFor(false), kSpear, Q4{}, kBase, kBase);
    CHECK(t.slideUu == 0.0);
}

static void TestSlideAlongNotAcross() {
    const HeldTolerance t = HeldToleranceForShaft(HeldToleranceFor(false), kSpear, Q4{}, kBase, kTip);
    CHECK(!HeldDiffers(kSpear, HeldPose{V3{-35, 0, 0}, Q4{}}, t));                 // 45 uu along: the slide, kept
    CHECK(!HeldDiffers(kSpear, HeldPose{V3{-130, 5, 0}, Q4{}}, t));                // 50 along, 5 across
    CHECK(HeldDiffers(kSpear, HeldPose{V3{-80, 0, 15}, Q4{}}, t));                 // 15 across: off the shaft
    CHECK(HeldDiffers(kSpear, HeldPose{V3{-80 + kHeldSlideUu + 5, 0, 0}, Q4{}}, t));   // past the slide
    CHECK(HeldDiffers(kSpear, HeldPose{V3{-80, 0, 0}, AxisAngle(V3{0, 1, 0}, kHeldReseatDeg + 5)}, t));   // turned
}

static void TestCheckDoesNotReseatTheSlide() {   // the logged 35-55 uu re-seats: none while the hand only slid
    const HeldTolerance t = HeldToleranceForShaft(HeldToleranceFor(false), kSpear, Q4{}, kBase, kTip);
    const HeldPose mine{V3{-30, 0, 0}, Q4{}};
    HeldCheck plain, shaft;
    CHECK(!plain.Update(0, kSpear, mine));
    CHECK(plain.Update(400, kSpear, mine));   // the old behaviour: a jump and a broken off hand
    CHECK(!shaft.Update(0, kSpear, mine, t));
    CHECK(!shaft.Update(400, kSpear, mine, t));
    CHECK(!shaft.Update(5000, kSpear, mine, t));
}

int main() {
    TestSpearGetsSlide();
    TestAxisInHandFrame();
    TestSwordKeepsPlain();
    TestShortOrMissingTip();
    TestSlideAlongNotAcross();
    TestCheckDoesNotReseatTheSlide();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_held_shaft: ok\n");
    return 0;
}

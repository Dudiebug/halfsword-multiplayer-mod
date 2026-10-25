// Unit tests for hsm_held.h. Run via implementation/shared/run_tests.sh test_hsm_held.
#include "sync/hsm_held.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static Q4 AxisAngle(V3 axis, double deg) {
    const double l = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    const double h = deg * 3.14159265358979323846 / 360.0;
    const double s = std::sin(h) / l;
    return Q4{axis.x * s, axis.y * s, axis.z * s, std::cos(h)};
}
static bool Near(V3 a, V3 b, double tol = 1e-6) { return Vec3Dist(a, b) < tol; }

static void TestHeldRoundTrips() {
    const V3 handPos{100, -40, 120};
    const Q4 handRot = AxisAngle(V3{0.3, 1, -0.2}, 70);
    const V3 weaponPos{110, -30, 95};
    const Q4 weaponRot = AxisAngle(V3{1, 0.5, 0}, 140);
    const HeldPose h = HeldIn(handPos, handRot, weaponPos, weaponRot);
    V3 p;
    Q4 r;
    HeldWorld(handPos, handRot, h, &p, &r);
    CHECK(Near(p, weaponPos));
    CHECK(QuatAngleDeg(r, weaponRot) < 1e-4);
}

static void TestHeldMovesWithTheHand() {   // the same seat on another hand: the weapon follows that hand
    const HeldPose h = HeldIn(V3{0, 0, 0}, Q4{}, V3{20, 0, 0}, AxisAngle(V3{0, 0, 1}, 30));
    CHECK(Near(h.pos, V3{20, 0, 0}));
    V3 p;
    Q4 r;
    HeldWorld(V3{5, 5, 5}, AxisAngle(V3{0, 0, 1}, 90), h, &p, &r);
    CHECK(Near(p, V3{5, 25, 5}));
    CHECK(QuatAngleDeg(r, AxisAngle(V3{0, 0, 1}, 120)) < 1e-4);
}

static void TestHeldIsInTheHandFrame() {   // turning hand and weapon together leaves the seat unchanged
    const Q4 turn = AxisAngle(V3{1, 1, 0}, 75);
    const HeldPose a = HeldIn(V3{1, 2, 3}, Q4{}, V3{11, 2, 3}, Q4{});
    const HeldPose b = HeldIn(V3{1, 2, 3}, turn, Vec3Add(V3{1, 2, 3}, QuatRotateVec(turn, V3{10, 0, 0})), turn);
    CHECK(!HeldDiffers(a, b, 0.01, 0.01));
}

static void TestUpsideDownIsWrong() {
    const HeldPose owner{V3{0, 0, 10}, Q4{}};
    const HeldPose flipped{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, 180)};
    CHECK(HeldDiffers(owner, flipped, kHeldReseatDeg, kHeldReseatUu));
    CHECK(!HeldDiffers(owner, HeldPose{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, kHeldReseatDeg - 1)}, kHeldReseatDeg, kHeldReseatUu));
    CHECK(HeldDiffers(owner, HeldPose{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, kHeldReseatDeg + 1)}, kHeldReseatDeg, kHeldReseatUu));
    CHECK(!HeldDiffers(owner, HeldPose{V3{0, 0, 10 + kHeldReseatUu - 1}, Q4{}}, kHeldReseatDeg, kHeldReseatUu));
    CHECK(HeldDiffers(owner, HeldPose{V3{0, 0, 10 + kHeldReseatUu + 1}, Q4{}}, kHeldReseatDeg, kHeldReseatUu));
}

static void TestCheckWaitsForAPersistentSteadyMismatch() {
    const HeldPose owner{V3{0, 0, 10}, Q4{}};
    const HeldPose wrong{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, 180)};
    HeldCheck c;
    CHECK(!c.Update(0, owner, wrong));
    CHECK(!c.Update(kHeldPersistMs - 1, owner, wrong));
    CHECK(c.Update(kHeldPersistMs, owner, wrong));
}

static void TestCheckIgnoresAMatchingSeat() {
    const HeldPose owner{V3{0, 0, 10}, Q4{}};
    HeldCheck c;
    for (double t = 0; t < 5000; t += 16) CHECK(!c.Update(t, owner, owner));
}

static void TestCheckRestartsWhenTheSeatsAgreeForAMoment() {
    const HeldPose owner{V3{0, 0, 10}, Q4{}};
    const HeldPose wrong{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, 180)};
    HeldCheck c;
    CHECK(!c.Update(0, owner, wrong));
    CHECK(!c.Update(200, owner, owner));
    CHECK(!c.Update(250, owner, wrong));
    CHECK(!c.Update(250 + kHeldPersistMs - 1, owner, wrong));
    CHECK(c.Update(250 + kHeldPersistMs, owner, wrong));
}

static void TestCheckWaitsWhileTheOwnerSlides() {   // the owner's hand moving along the shaft is not a seat yet
    const HeldPose wrong{V3{0, 0, 60}, AxisAngle(V3{0, 1, 0}, 180)};
    HeldCheck c;
    double t = 0;
    for (; t < 2000; t += 16) CHECK(!c.Update(t, HeldPose{V3{0, 0, t * 0.02}, Q4{}}, wrong));   // 0.32 uu a step, 40 uu in all
    const HeldPose still{V3{0, 0, 40}, Q4{}};
    CHECK(!c.Update(t, still, wrong));
    CHECK(c.Update(t + kHeldPersistMs, still, wrong));
}

static void TestCheckSpacesReseats() {
    const HeldPose owner{V3{0, 0, 10}, Q4{}};
    const HeldPose wrong{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, 180)};
    HeldCheck c;
    CHECK(!c.Update(0, owner, wrong));
    CHECK(c.Update(kHeldPersistMs, owner, wrong));
    CHECK(!c.Update(kHeldPersistMs + 1, owner, wrong));
    CHECK(!c.Update(2 * kHeldPersistMs + 1, owner, wrong));   // persisted again, but the joint is still carrying it
    const HeldPose moved{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, 90)};   // the owner turned the weapon in the hand
    CHECK(!c.Update(2 * kHeldPersistMs + 2, moved, wrong));
    CHECK(!c.Update(kHeldPersistMs + kHeldMinGapMs - 1, moved, wrong));
    CHECK(c.Update(kHeldPersistMs + kHeldMinGapMs, moved, wrong));
}

static void TestCheckDoesNotRepeatASeatThatDidNotTake() {   // mirror test 2026-10-25: "28 deg, 0.6 uu off" every second
    const HeldPose owner{V3{0, 0, 10}, Q4{}};
    const HeldPose stuck{V3{0, 0, 10.6}, AxisAngle(V3{0, 1, 0}, kHeldReseatDeg + 3)};
    HeldCheck c;
    int seats = 0;
    double t = 0;
    for (; t < 10000; t += 16) seats += c.Update(t, owner, stuck);
    CHECK(seats == 1);
    const HeldPose nearly{V3{0, 0, 10 + kHeldSteadyUu - 1}, AxisAngle(V3{0, 1, 0}, 2 - kHeldSteadyDeg)};   // the same seat, give or take
    for (; t < 20000; t += 16) seats += c.Update(t, nearly, stuck);
    CHECK(seats == 1);
    const HeldPose slid{V3{0, 0, 10 + kHeldSteadyUu + 1}, Q4{}};   // the owner's hand slid along the shaft: a new seat
    for (; t < 30000; t += 16) seats += c.Update(t, slid, stuck);
    CHECK(seats == 2);
    c.Reset();   // the stretch only: the seat already made still stands
    for (; t < 40000; t += 16) seats += c.Update(t, slid, stuck);
    CHECK(seats == 2);
    c.Forget();  // another joint or weapon: seat it again
    for (; t < 50000; t += 16) seats += c.Update(t, slid, stuck);
    CHECK(seats == 3);
}

static void TestResetForgetsTheStretch() {
    const HeldPose owner{V3{0, 0, 10}, Q4{}};
    const HeldPose wrong{V3{0, 0, 10}, AxisAngle(V3{0, 1, 0}, 180)};
    HeldCheck c;
    CHECK(!c.Update(0, owner, wrong));
    c.Reset();
    CHECK(!c.Update(kHeldPersistMs, owner, wrong));
    CHECK(c.Update(2 * kHeldPersistMs, owner, wrong));
}

int main() {
    TestHeldRoundTrips();
    TestHeldMovesWithTheHand();
    TestHeldIsInTheHandFrame();
    TestUpsideDownIsWrong();
    TestCheckWaitsForAPersistentSteadyMismatch();
    TestCheckIgnoresAMatchingSeat();
    TestCheckRestartsWhenTheSeatsAgreeForAMoment();
    TestCheckWaitsWhileTheOwnerSlides();
    TestCheckSpacesReseats();
    TestCheckDoesNotRepeatASeatThatDidNotTake();
    TestResetForgetsTheStretch();
    if (g_fail) { std::printf("test_hsm_held: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_held: ok\n");
    return 0;
}

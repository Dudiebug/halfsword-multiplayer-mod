// Unit tests for hsm_grip.h. Run via implementation/shared/run_tests.sh test_hsm_grip.
#include "sync/hsm_grip.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static void TestHolderNeverGetsZeroFromThePose() {
    CHECK(GripToApply(0, true, 3) == 3);       // the weapon's own default stands in for the pose's 0
    CHECK(GripToApply(0, true, 1) == 1);
    CHECK(GripToApply(0, true, 31) == 31);
    CHECK(GripToApply(0, true, 0) == 0);       // a weapon whose own default is 0: nothing better to give (caller skips)
}

static void TestNonZeroPoseGripIsAppliedUnchanged() {   // negative control: the default never overrides a real grip
    const uint8_t defaults[] = {0, 1, 3, 31};
    for (unsigned g = 1; g < 256; ++g) {
        for (uint8_t d : defaults) {
            CHECK(GripToApply(uint8_t(g), true, d) == g);
            CHECK(GripToApply(uint8_t(g), false, d) == g);
        }
    }
}

static void TestEmptyHandGetsThePoseValue() {
    const uint8_t defaults[] = {0, 3, 31};
    for (unsigned g = 0; g < 256; ++g) for (uint8_t d : defaults) CHECK(GripToApply(uint8_t(g), false, d) == g);
    CHECK(GripToApply(0, false, 3) == 0);      // an empty hand stays open even when a default is known
}

static void TestExhaustiveInvariants() {
    for (unsigned pose = 0; pose < 256; ++pose) {
        for (int holds = 0; holds < 2; ++holds) {
            for (unsigned def = 0; def < 32; ++def) {
                const uint8_t r = GripToApply(uint8_t(pose), holds != 0, uint8_t(def));
                if (!holds || pose != 0) CHECK(r == pose);
                else CHECK(r == def);
                if (holds && r == 0) CHECK(pose == 0 && def == 0);          // a holder is never left at 0 by us
                CHECK(GripToApply(r, holds != 0, uint8_t(def)) == r);        // idempotent
            }
        }
    }
}

// The two-player test 12 longsword: half-sword grip 15 (alt), then Event Reverse Grip makes mordhau (grip 10, reverse,
// reverse type 1); the default two-handed grip is 13.
static void TestSeatPoint() {
    CHECK(SeatFor(13, true, false, false, false, 1).point == SeatPoint::Default);    // two hands on the grip
    CHECK(SeatFor(13, false, false, false, false, 1).point == SeatPoint::Alt);       // 13/14/16 one-handed: the alt place
    CHECK(SeatFor(14, false, false, false, false, 1).point == SeatPoint::Alt);
    CHECK(SeatFor(16, false, false, false, false, 1).point == SeatPoint::Alt);
    CHECK(SeatFor(16, true, false, false, false, 1).point == SeatPoint::Default);
    CHECK(SeatFor(3, true, false, false, false, 1).point == SeatPoint::Alt);         // grip 3 always
    CHECK(SeatFor(15, true, true, false, false, 1).point == SeatPoint::Alt);         // half-sword
    CHECK(SeatFor(10, true, true, true, true, 1).point == SeatPoint::Reverse);       // mordhau, alt flag or not
    CHECK(SeatFor(10, true, false, true, true, 1).point == SeatPoint::Reverse);
    CHECK(SeatFor(1, false, false, true, false, 0).point == SeatPoint::Reverse);
    // Negative control: every other grip type with no flags holds at the default place.
    for (unsigned g = 0; g < 32; ++g) {
        if (g == 3 || g == 13 || g == 14 || g == 16) continue;
        CHECK(SeatFor(uint8_t(g), false, false, false, false, 1).point == SeatPoint::Default);
        CHECK(SeatFor(uint8_t(g), true, false, false, false, 1).point == SeatPoint::Default);
    }
}

static void TestSeatFlip() {
    CHECK(SeatFor(10, true, false, true, true, 1).flip == SeatFlip::Pitch180);       // mordhau turns the sword over
    CHECK(SeatFor(1, false, false, false, true, 0).flip == SeatFlip::Yaw180);
    CHECK(SeatFor(1, false, false, false, true, 1).flip == SeatFlip::Pitch180);
    CHECK(SeatFor(1, false, false, false, true, 2).flip == SeatFlip::Pitch180);
    CHECK(SeatFor(1, false, false, false, true, 3).flip == SeatFlip::None);
    for (unsigned t = 0; t < 4; ++t) {                                                 // not reversed: never turned
        CHECK(SeatFor(1, false, false, false, false, uint8_t(t)).flip == SeatFlip::None);
        CHECK(SeatFor(10, true, false, true, false, uint8_t(t)).flip == SeatFlip::None);
    }
}

static void TestSeatSocketAndBone() {
    for (unsigned g = 0; g < 32; ++g) {
        const GripSeat s = SeatFor(uint8_t(g), false, false, false, false, 1);
        CHECK(s.hammerSocket == (g == 5 || g == 8 || g == 15));
        CHECK(s.lowerArm == (g == 6 || g == 7));
    }
    CHECK(SeatFor(15, true, true, false, false, 1).hammerSocket);
    CHECK(!SeatFor(10, true, true, true, true, 1).hammerSocket);                      // mordhau is grip 10: the plain socket
}

static void TestSeatKey() {
    bool seen[8] = {};
    for (int a = 0; a < 2; ++a) for (int m = 0; m < 2; ++m) for (int r = 0; r < 2; ++r) {
        const uint8_t k = SeatKey(a != 0, m != 0, r != 0);
        CHECK(k < 8);
        if (k < 8) { CHECK(!seen[k]); seen[k] = true; }                                // every toggle changes the key
    }
    CHECK(SeatKey(false, false, false) == 0);                                          // a new joint's seat
}

int main() {
    TestHolderNeverGetsZeroFromThePose();
    TestNonZeroPoseGripIsAppliedUnchanged();
    TestEmptyHandGetsThePoseValue();
    TestExhaustiveInvariants();
    TestSeatPoint();
    TestSeatFlip();
    TestSeatSocketAndBone();
    TestSeatKey();
    if (g_fail) std::printf("%d CHECK(S) FAILED\n", g_fail);
    else std::printf("all tests passed\n");
    return g_fail != 0;
}

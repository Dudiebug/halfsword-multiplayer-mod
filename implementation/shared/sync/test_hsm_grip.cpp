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

static void TestSeatKeyFor() {
    for (int g = 0; g <= 255; ++g) CHECK(TwoHandedGrip(uint8_t(g)) == (g >= 10 && g <= 16));   // the game's R Two Handed Grip set
    for (int g = 0; g <= 31; ++g) for (int m = 0; m < 2; ++m) for (int r = 0; r < 2; ++r) {
        const uint8_t grip = uint8_t(g);
        // The alt flag seats only a two-handed grip; in any other grip the key is the one without it.
        CHECK(SeatKeyFor(grip, true, m != 0, r != 0, false) == SeatKey(g >= 10 && g <= 16, m != 0, r != 0));
        CHECK(SeatKeyFor(grip, false, m != 0, r != 0, false) == SeatKey(false, m != 0, r != 0));
        // The earlier rule: every flag counts in every grip.
        CHECK(SeatKeyFor(grip, true, m != 0, r != 0, true) == SeatKey(true, m != 0, r != 0));
        CHECK(SeatKeyFor(grip, false, m != 0, r != 0, true) == SeatKey(false, m != 0, r != 0));
    }
    CHECK(SeatKeyFor(1, true, false, false, false) == 0);    // a sword beside a shield: both buttons held, no re-seat
    CHECK(SeatKeyFor(2, true, false, false, false) == 0);
    CHECK(SeatKeyFor(3, true, false, false, false) == 0);
    CHECK(SeatKeyFor(9, true, false, false, false) == 0);
    CHECK(SeatKeyFor(17, true, false, false, false) == 0);
    CHECK(SeatKeyFor(15, true, false, false, false) == 1);   // the half-sword grip still seats
    CHECK(SeatKeyFor(10, true, false, false, false) == 1);
    CHECK(SeatKeyFor(16, true, false, false, false) == 1);
    CHECK(SeatKeyFor(1, true, true, false, false) == 2);     // mordhau and the reverse grip seat in any grip
    CHECK(SeatKeyFor(1, true, false, true, false) == 4);
    CHECK(SeatKeyFor(1, true, false, false, true) == 1);
}

// Protocol 54: the master stroke grip (Willie_BP.cpp Event Master Stroke Switch, :15841; the alt re-seat, :25862).
static void TestMasterSeat() {
    // The alt / mordhau re-seat adds Rot(0,90,0) while the master stroke is on and the grip is not reversed.
    CHECK(SeatFor(11, true, false, false, false, 1, true).flip == SeatFlip::Yaw90);
    CHECK(SeatFor(11, true, false, false, false, 1, false).flip == SeatFlip::None);
    CHECK(SeatFor(11, true, false, false, false, 1).flip == SeatFlip::None);              // the default: no master stroke
    for (unsigned t = 0; t < 4; ++t) {
        CHECK(SeatFor(11, true, false, false, true, uint8_t(t), true).flip == ReverseFlip(uint8_t(t)));   // reverse wins
        CHECK(SeatFor(11, true, false, false, true, uint8_t(t), false).flip == ReverseFlip(uint8_t(t)));
    }
    CHECK(ReverseFlip(0) == SeatFlip::Yaw180 && ReverseFlip(1) == SeatFlip::Pitch180 && ReverseFlip(2) == SeatFlip::Pitch180 &&
          ReverseFlip(3) == SeatFlip::None);
    CHECK(SeatFor(11, true, false, false, false, 1, true).point == SeatPoint::Default);   // the turn does not move the point
    CHECK(SeatFor(11, true, true, false, false, 1, true).point == SeatPoint::Alt);
    // The switch's own seat: the weapon's origin, no attach point; the joint at the hammer socket.
    for (unsigned d = 0; d < 32; ++d) for (int sw = 0; sw < 2; ++sw) {
        const GripSeat s = MasterSeatFor(11, uint8_t(d), sw != 0, true, false, 1);
        CHECK(s.point == SeatPoint::Socket);
        CHECK(s.jointHammer);
        CHECK(s.flip == SeatFlip::Yaw90);
        CHECK(s.hammerSocket == (d == 5 || d == 8 || sw != 0));                          // the weapon's default, not gripR
        CHECK(s.shieldSocket == (!s.hammerSocket && (d == 6 || d == 7)));
        CHECK(!(s.hammerSocket && s.shieldSocket));
        CHECK(!s.lowerArm);                                                              // gripR 11 holds hand_r
    }
    CHECK(!MasterSeatFor(15, 1, false, true, false, 1).hammerSocket);                   // gripR 15 alone is not the hammer here
    CHECK(MasterSeatFor(6, 6, false, true, false, 1).lowerArm && MasterSeatFor(7, 1, false, true, false, 1).lowerArm);
    CHECK(!MasterSeatFor(10, 6, false, true, false, 1).lowerArm);                        // the bone follows the current grip
    CHECK(MasterSeatFor(11, 1, false, false, false, 1).flip == SeatFlip::None);          // turned off: straight again
    for (unsigned t = 0; t < 4; ++t) {
        CHECK(MasterSeatFor(11, 1, false, true, true, uint8_t(t)).flip == ReverseFlip(uint8_t(t)));
        CHECK(MasterSeatFor(11, 1, false, false, true, uint8_t(t)).flip == ReverseFlip(uint8_t(t)));
    }
    // The key: bit 8, only when the rig counts the master stroke; the other bits are unchanged.
    for (int g = 0; g <= 31; ++g) for (int a = 0; a < 2; ++a) for (int m = 0; m < 2; ++m) for (int r = 0; r < 2; ++r) {
        const uint8_t off = SeatKeyFor(uint8_t(g), a != 0, m != 0, r != 0, false, false);
        const uint8_t on = SeatKeyFor(uint8_t(g), a != 0, m != 0, r != 0, false, true);
        CHECK(off == SeatKeyFor(uint8_t(g), a != 0, m != 0, r != 0, false));
        CHECK(on == uint8_t(off | kSeatKeyMaster));
        CHECK((off & kSeatKeyMaster) == 0);
        CHECK(MasterSeatChange(off, on) && MasterSeatChange(on, off));
        CHECK(!MasterSeatChange(off, off) && !MasterSeatChange(on, on));
    }
    CHECK(!MasterSeatChange(0, kSeatKeyMaster | 1));   // the alt grip came on with it: the alt re-seat stays
    CHECK(!MasterSeatChange(kSeatKeyMaster, 1));
    CHECK(!MasterSeatChange(0, 4) && !MasterSeatChange(kSeatKeyMaster, kSeatKeyMaster | 2));
}

// Build 26820056's attach point (Willie_BP.cpp:25864): grip 3 and the reach.
static SeatInput In(uint8_t grip, bool off, bool alt, bool mord, bool search) {
    SeatInput in;
    in.gripR = grip; in.offhand = off; in.alt = alt; in.mordhau = mord; in.offSearching = search;
    return in;
}
static void TestSeatForBuild() {
    CHECK(SeatForBuild(In(3, false, false, false, false)).point == SeatPoint::LeftDefault);   // not the alt point
    CHECK(SeatForBuild(In(3, true, false, false, false)).point == SeatPoint::LeftDefault);
    CHECK(SeatForBuild(In(3, false, true, false, false)).point == SeatPoint::Alt);           // the alt grip wins
    CHECK(SeatForBuild(In(3, false, false, true, false)).point == SeatPoint::Reverse);       // mordhau wins
    const uint8_t longGrips[3] = {13, 14, 16};
    for (uint8_t g : longGrips) {
        CHECK(SeatForBuild(In(g, false, false, false, false)).point == SeatPoint::Alt);      // one hand on it
        CHECK(SeatForBuild(In(g, false, false, false, true)).point == SeatPoint::Default);   // the off hand reaching
        CHECK(SeatForBuild(In(g, true, false, false, false)).point == SeatPoint::Default);   // both hands on it
        CHECK(SeatForBuild(In(g, false, true, false, true)).point == SeatPoint::Alt);        // the alt grip still
    }
    for (unsigned g = 0; g < 32; ++g) {
        if (g == 3 || g == 13 || g == 14 || g == 16) continue;
        for (int s = 0; s < 2; ++s) {
            CHECK(SeatForBuild(In(uint8_t(g), false, false, false, s != 0)).point == SeatPoint::Default);
            CHECK(SeatForBuild(In(uint8_t(g), false, true, false, s != 0)).point == SeatPoint::Alt);
            CHECK(SeatForBuild(In(uint8_t(g), false, false, true, s != 0)).point == SeatPoint::Reverse);
        }
    }
    // The rest is SeatFor's: flip, sockets, bone.
    for (unsigned g = 0; g < 32; ++g) for (int r = 0; r < 2; ++r) for (int m = 0; m < 2; ++m) for (unsigned t = 0; t < 4; ++t) {
        SeatInput in = In(uint8_t(g), false, false, false, false);
        in.reverse = r != 0; in.master = m != 0; in.reverseType = uint8_t(t);
        const GripSeat a = SeatForBuild(in), b = SeatFor(uint8_t(g), false, false, false, r != 0, uint8_t(t), m != 0);
        CHECK(a.flip == b.flip && a.hammerSocket == b.hammerSocket && a.lowerArm == b.lowerArm && !a.shieldSocket && !a.jointHammer);
    }
}

static void TestShieldHandGrip() {
    for (int g = 0; g <= 255; ++g) CHECK(ShieldHandGrip(uint8_t(g)) == (g >= 5 && g <= 9));
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
    TestSeatKeyFor();
    TestMasterSeat();
    TestSeatForBuild();
    TestShieldHandGrip();
    if (g_fail) std::printf("%d CHECK(S) FAILED\n", g_fail);
    else std::printf("all tests passed\n");
    return g_fail != 0;
}

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

int main() {
    TestHolderNeverGetsZeroFromThePose();
    TestNonZeroPoseGripIsAppliedUnchanged();
    TestEmptyHandGetsThePoseValue();
    TestExhaustiveInvariants();
    if (g_fail) std::printf("%d CHECK(S) FAILED\n", g_fail);
    else std::printf("all tests passed\n");
    return g_fail != 0;
}

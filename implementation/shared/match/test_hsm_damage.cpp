// Unit tests for match/hsm_damage.h. Run via implementation/shared/run_tests.sh test_hsm_damage.
#include "match/hsm_damage.h"
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestRangeFromSlider() {
    DamageRange r = DamageRangeFrom(0.0, 2.0, 0.1, 1.0);
    CHECK(r.minPct == 0 && r.maxPct == 200 && r.stepPct == 10 && r.stockPct == 100);
    r = DamageRangeFrom(0.1, 1.0, 0.0, 0.5);               // no step on the slider: 10%
    CHECK(r.minPct == 10 && r.maxPct == 100 && r.stepPct == 10 && r.stockPct == 50);
    r = DamageRangeFrom(0.0, 1.0, 0.25, 1.5);              // a stock value past the slider still fits
    CHECK(r.maxPct == 150 && r.stepPct == 25);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    r = DamageRangeFrom(nan, 5.0, nan, nan);               // nothing usable: the defaults
    CHECK(r.minPct == 0 && r.maxPct == 200 && r.stepPct == 10 && r.stockPct == 100);
    r = DamageRangeFrom(1.0, 0.5, 0.1, 1.0);               // min above max: the defaults
    CHECK(r.minPct == 0 && r.maxPct == 200);
}

static void TestStepStopsAtStock() {
    const DamageRange r = DamageRangeFrom(0.0, 1.0, 0.3, 0.5);   // 0, 30, 50 (stock), 80, 100
    CHECK(DamagePct(kDamageStock, r) == 50);
    CHECK(StepDamage(kDamageStock, 1, r) == 80 && StepDamage(kDamageStock, -1, r) == 20);
    CHECK(StepDamage(20, 1, r) == kDamageStock);           // 20 + 30 passes the stock value: stops on it
    CHECK(StepDamage(80, -1, r) == kDamageStock);
    CHECK(StepDamage(80, 1, r) == 100 && StepDamage(100, 1, r) == 100);
    CHECK(StepDamage(20, -1, r) == 0 && StepDamage(0, -1, r) == 0);
    CHECK(DamagePct(900, r) == 100);                       // out of the slider's range: clamped
    CHECK(DamageRate(kDamageStock, r) == 0.5 && DamageRate(80, r) == 0.8);
    CHECK(DamageLabel(kDamageStock, r) == "50% (stock)" && DamageLabel(80, r) == "80%");
}

static void TestOwnBeforeSave() {
    double own = 0.2;                                      // the player's own slider, at the minimum
    CHECK(OwnBeforeSave(own, 1.0, 1.0) == 0.2 && own == 0.2);   // the forced value in the game instance: not theirs
    CHECK(OwnBeforeSave(own, 0.7, 1.0) == 0.7 && own == 0.7);   // they moved their slider: kept as theirs
    CHECK(OwnBeforeSave(own, 0.7, 1.0) == 0.7);            // a level load read their save back: the same value
    own = 1.0;
    CHECK(OwnBeforeSave(own, 1.0, 1.0) == 1.0);            // own equal to the forced one
}

int main() {
    TestRangeFromSlider();
    TestStepStopsAtStock();
    TestOwnBeforeSave();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_damage: ok\n");
    return 0;
}

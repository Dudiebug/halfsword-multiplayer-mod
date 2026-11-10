// Rev 42 drift twin: the per-frame comparison and its window statistics. Run via implementation/shared/run_tests.sh
// test_hsm_twin.
#include "sync/hsm_twin.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static PoseFrame At(double x) {
    PoseFrame f;
    for (int i = 0; i < kPoseBones; ++i) f.bones[size_t(i)].pos = V3{x, double(i), 0};
    f.weapons[0].present = true; f.weapons[0].hand = 0; f.weapons[0].tip = V3{x + 100, 0, 0};
    return f;
}

static void TestCompare() {
    // The twin stands 250 uu to the side: identical bodies compare as zero.
    TwinDiff d = CompareTwin(At(0), At(250), V3{250, 0, 0});
    CHECK(d.pelvis == 0 && d.handL == 0 && d.handR == 0 && d.tip == 0);
    PoseFrame twin = At(250);
    twin.bones[size_t(kBoneHandR)].pos.z = 12;
    twin.weapons[0].tip.y = 5;
    d = CompareTwin(At(0), twin, V3{250, 0, 0});
    CHECK(d.handR == 12 && d.handL == 0 && d.tip == 5);
    twin.weapons[0].hand = 1;   // a different hand: no tip to compare
    CHECK(CompareTwin(At(0), twin, V3{250, 0, 0}).tip == -1);
}

static void TestStats() {
    TwinWindow w;
    for (int i = 0; i < 100; ++i) { TwinDiff d; d.pelvis = i; d.handL = 1; d.handR = 3; w.Add(d); }
    CHECK(w.frames == 100 && w.pelvis.Count() == 100 && w.hands.Count() == 200 && w.tip.Count() == 0);   // -1: not counted
    CHECK(std::fabs(w.pelvis.Mean() - 49.5) < 1e-9 && w.pelvis.Max() == 99);
    CHECK(w.pelvis.Quantile(0.99) == 99 && w.hands.Quantile(0.5) == 2);
    TwinStat big; big.Add(1e6); CHECK(big.Quantile(1) == 400 && big.Max() == 1e6);
    TwinStat none; none.Add(std::nan("")); CHECK(none.Count() == 0 && none.Quantile(0.5) == 0);
    w.predicted = 25;
    CHECK(TwinLine(w, "1s").find("predicted 25.0%") != std::string::npos);
}

int main() {
    TestCompare();
    TestStats();
    if (g_fail == 0) std::printf("test_hsm_twin: all passed\n");
    return g_fail == 0 ? 0 : 1;
}

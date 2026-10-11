// Unit tests for hsm_roottrack.h: the owner's capsule root and yaw, looked up at the stand-in's play time.
// Run via implementation/shared/run_tests.sh test_hsm_roottrack.
#include "sync/hsm_roottrack.h"
#include <cmath>
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;

static bool Near(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
static bool NearV(const V3& a, const V3& b) { return Near(a.x, b.x) && Near(a.y, b.y) && Near(a.z, b.z); }

static void TestEmptyAndSingle() {
    RootTrack t;
    V3 p{ 7, 7, 7 };
    double yaw = 7;
    CHECK(!t.At(1000, p, yaw));
    CHECK(NearV(p, V3{ 7, 7, 7 }) && yaw == 7);                    // untouched on false
    CHECK(t.Push(1000, V3{ 10, 20, 30 }, 45));
    CHECK(t.At(1000, p, yaw) && NearV(p, V3{ 10, 20, 30 }) && Near(yaw, 45));
    CHECK(t.At(0, p, yaw) && NearV(p, V3{ 10, 20, 30 }) && Near(yaw, 45));      // before: the oldest
    CHECK(t.At(9000, p, yaw) && NearV(p, V3{ 10, 20, 30 }) && Near(yaw, 45));   // after: held, never extrapolated
    CHECK(t.Size() == 1);
}

static void TestInterpolation() {
    RootTrack t;
    CHECK(t.Push(1000, V3{ 0, 0, 100 }, 10));
    CHECK(t.Push(1100, V3{ 100, -50, 100 }, 30));
    V3 p;
    double yaw;
    CHECK(t.At(1050, p, yaw) && NearV(p, V3{ 50, -25, 100 }) && Near(yaw, 20));
    CHECK(t.At(1025, p, yaw) && NearV(p, V3{ 25, -12.5, 100 }) && Near(yaw, 15));
    CHECK(t.At(1100, p, yaw) && NearV(p, V3{ 100, -50, 100 }) && Near(yaw, 30));   // exactly the newest
    CHECK(t.At(1000, p, yaw) && NearV(p, V3{ 0, 0, 100 }) && Near(yaw, 10));       // exactly the oldest
    CHECK(t.Push(1200, V3{ 100, 50, 100 }, 30));
    CHECK(t.At(1150, p, yaw) && NearV(p, V3{ 100, 0, 100 }) && Near(yaw, 30));     // the right pair of three
}

static void TestYawWrap() {
    RootTrack t;
    CHECK(t.Push(1000, V3{}, 170));
    CHECK(t.Push(1100, V3{}, -170));                                // 20 degrees the short way, across 180
    V3 p;
    double yaw;
    CHECK(t.At(1050, p, yaw) && Near(std::fabs(yaw), 180));
    CHECK(t.At(1025, p, yaw) && Near(yaw, 175));
    CHECK(t.At(1075, p, yaw) && Near(yaw, -175));
    RootTrack u;
    CHECK(u.Push(1000, V3{}, 10));
    CHECK(u.Push(1100, V3{}, 350));                                 // = -10: through 0, not through 180
    CHECK(u.At(1050, p, yaw) && Near(yaw, 0));
    CHECK(u.At(1100, p, yaw) && Near(yaw, -10));                    // outputs are wrapped to (-180, 180]
    RootTrack w;
    CHECK(w.Push(1000, V3{}, 540));                                 // a stored yaw is wrapped too
    CHECK(w.At(1000, p, yaw) && Near(yaw, 180));
    CHECK(w.Push(1100, V3{}, -180));
    CHECK(w.At(1100, p, yaw) && Near(yaw, 180));                    // -180 is written as 180
}

static void TestOrderingAndRestart() {
    RootTrack t;
    CHECK(t.Push(10000, V3{ 0, 0, 0 }, 0));
    CHECK(t.Push(10100, V3{ 100, 0, 0 }, 0));
    CHECK(!t.Push(10100, V3{ 999, 0, 0 }, 0));                      // duplicate time: the first one stays
    CHECK(!t.Push(10050, V3{ 999, 0, 0 }, 0));                      // late: never inserted between
    V3 p;
    double yaw;
    CHECK(t.At(10050, p, yaw) && NearV(p, V3{ 50, 0, 0 }));
    CHECK(t.At(10100, p, yaw) && NearV(p, V3{ 100, 0, 0 }));
    CHECK(t.Size() == 2);
    CHECK(!t.Push(10100 - uint32_t(kRootRestartBackMs), V3{ 5, 0, 0 }, 0));   // exactly the restart distance: still late
    CHECK(t.Size() == 2);
    CHECK(t.Push(10099 - uint32_t(kRootRestartBackMs), V3{ 5, 0, 0 }, 90));   // beyond it: the owner's clock restarted
    CHECK(t.Size() == 1);
    CHECK(t.At(50000, p, yaw) && NearV(p, V3{ 5, 0, 0 }) && Near(yaw, 90));
}

static void TestRejectsNonFinite() {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    RootTrack t;
    CHECK(!t.Push(1000, V3{ nan, 0, 0 }, 0));
    CHECK(!t.Push(1000, V3{ 0, inf, 0 }, 0));
    CHECK(!t.Push(1000, V3{ 0, 0, -inf }, 0));
    CHECK(!t.Push(1000, V3{ 0, 0, 0 }, nan));
    CHECK(!t.Push(1000, V3{ 0, 0, 0 }, inf));
    CHECK(t.Size() == 0);
    CHECK(t.Push(1000, V3{ 1, 2, 3 }, 4));
}

static void TestCapacityAndReset() {
    RootTrack t;
    for (uint32_t i = 0; i < 200; ++i) CHECK(t.Push(1000 + i * 16, V3{ double(i), 0, 0 }, 0));
    CHECK(t.Size() == kRootTrackCap);
    V3 p;
    double yaw;
    CHECK(t.At(1000, p, yaw) && Near(p.x, double(200 - kRootTrackCap)));   // the oldest kept, not the first pushed
    CHECK(t.At(1000 + 199 * 16, p, yaw) && Near(p.x, 199));
    CHECK(t.At(1000 + 150 * 16 + 8, p, yaw) && Near(p.x, 150.5));
    t.Reset();
    CHECK(t.Size() == 0 && !t.At(1000, p, yaw));
}

int main() {
    TestEmptyAndSingle();
    TestInterpolation();
    TestYawWrap();
    TestOrderingAndRestart();
    TestRejectsNonFinite();
    TestCapacityAndReset();
    std::printf(g_fail ? "%d FAILED\n" : "all root track tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

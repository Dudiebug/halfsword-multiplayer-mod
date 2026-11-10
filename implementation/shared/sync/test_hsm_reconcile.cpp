// Rev 42 error-offset reconciliation: the error at the pose's time, its decay through the nudges, snaps, rotations and
// netstats. Run via implementation/shared/run_tests.sh test_hsm_reconcile.
#include "sync/hsm_reconcile.h"
#include "sync/hsm_netstats.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::sync;

static FighterNow Body(double x) {
    FighterNow n;
    for (int i = 0; i < kPoseBones; ++i) { n.bones[size_t(i)].pos = V3{x, double(i), 100}; n.valid[size_t(i)] = kPoseHasBody[size_t(i)]; }
    n.weapons[0].pos = V3{x + 50, 0, 120}; n.weaponValid[0] = true;
    return n;
}
static PoseFrame Pose(uint32_t ts, double x) {
    PoseFrame f; f.tsMs = ts;
    for (int i = 0; i < kPoseBones; ++i) f.bones[size_t(i)].pos = V3{x, double(i), 100};
    f.weapons[0].present = true; f.weapons[0].pos = V3{x + 50, 0, 120};
    return f;
}

// The stand-in ran 10 uu behind the truth at t = 1050: the error is 10, and the nudges work it off at exp(-t/tau).
static void TestErrorAndDecay() {
    Reconciler r;
    for (int k = 0; k <= 10; ++k) r.Record(1000 + k * 10, Body(k * 10.0));   // stand-in at x = (t - 1000)
    Reconciler::Result res = r.OnAuth(Pose(1050, 60.0));                     // truth at 1050 is x = 60
    CHECK(res.applied && !res.snap && std::fabs(res.pelvisErrUu - 10.0) < 1e-9 && std::fabs(res.maxErrUu - 10.0) < 1e-9);
    // A body keeps the velocity it is given: the nudges are changes of the carried correction velocity.
    double moved = 0, carried = 0;
    for (int f = 0; f < 6; ++f) {   // 100 ms at 60 FPS
        const Reconciler::Nudge n = r.Step(1000.0 / 60.0);
        CHECK(n.any[size_t(kBonePelvis)] && n.linUu[size_t(kBonePelvis)].y == 0);
        carried += n.linUu[size_t(kBonePelvis)].x;
        CHECK(carried > 0);
        moved += carried / 60.0;
    }
    const double expect = 10.0 * (1.0 - std::exp(-100.0 / kReconcileTauMs));
    CHECK(std::fabs(moved - expect) < 1e-6);
    CHECK(std::fabs(r.Pending() - (10.0 - expect)) < 1e-6);
    // Same total at another frame rate.
    Reconciler r2;
    for (int k = 0; k <= 10; ++k) r2.Record(1000 + k * 10, Body(k * 10.0));
    r2.OnAuth(Pose(1050, 60.0));
    double moved2 = 0, carried2 = 0;
    for (int f = 0; f < 12; ++f) { carried2 += r2.Step(1000.0 / 120.0).linUu[size_t(kBonePelvis)].x; moved2 += carried2 / 120.0; }
    CHECK(std::fabs(moved2 - expect) < 1e-6);
    // Dropped: the next step takes the carried velocity back, so the bodies carry none.
    r2.Drop();
    carried2 += r2.Step(1000.0 / 120.0).linUu[size_t(kBonePelvis)].x;
    CHECK(std::fabs(carried2) < 1e-9);
}

static void TestOutsideHistoryAndOrder() {
    Reconciler r;
    CHECK(!r.OnAuth(Pose(1000, 0)).applied);                 // nothing recorded
    r.Record(1000, Body(0)); r.Record(1100, Body(10));
    r.Record(1050, Body(5));                                  // out of order: ignored
    CHECK(r.HistorySize() == 2);
    CHECK(!r.OnAuth(Pose(900, 0)).applied);                  // older than the history
    CHECK(!r.OnAuth(Pose(1200, 0)).applied);                 // newer than the history
    CHECK(r.OnAuth(Pose(1100, 12)).applied);
    CHECK(!r.OnAuth(Pose(1050, 0)).applied);                 // older than the last pose used
}

static void TestSnap() {
    Reconciler r;
    r.Record(1000, Body(0)); r.Record(1020, Body(0));
    Reconciler::Result res = r.OnAuth(Pose(1010, kReconcileSnapUu + 50));
    CHECK(res.applied && res.snap);
    CHECK(r.Pending() == 0);   // a snap is placed by the caller, never slewed
}

static void TestRotation() {
    Reconciler r;
    FighterNow a = Body(0);
    r.Record(1000, a); r.Record(1020, a);
    PoseFrame p = Pose(1010, 0);
    const double half = 0.5 * 10.0 * 3.14159265358979323846 / 180.0;   // 10 degrees about z
    p.bones[size_t(kBonePelvis)].rot = Q4{0, 0, std::sin(half), std::cos(half)};
    r.OnAuth(p);
    double turned = 0, spin = 0;
    for (int f = 0; f < 600; ++f) { spin += r.Step(1000.0 / 60.0).angDeg[size_t(kBonePelvis)].z; turned += spin / 60.0; }
    CHECK(std::fabs(turned - 10.0) < 0.05);                  // the whole 10 degrees in the end
}

static void TestWeaponAbsent() {
    Reconciler r;
    r.Record(1000, Body(0)); r.Record(1020, Body(0));
    PoseFrame p = Pose(1010, 5);
    p.weapons[0].present = false;
    r.OnAuth(p);
    const Reconciler::Nudge n = r.Step(16);
    CHECK(!n.any[size_t(kPoseBones)]);                        // no weapon in the pose: nothing steered
}

// A big error is worked off at no more than the speed cap.
static void TestSpeedCap() {
    Reconciler r;
    r.Record(1000, Body(0)); r.Record(1020, Body(0));
    r.OnAuth(Pose(1010, kReconcileSnapUu - 1));   // just under a snap, at 1/60 s a frame far beyond the cap
    double carried = 0;
    for (int f = 0; f < 30; ++f) {
        carried += r.Step(1000.0 / 60.0).linUu[size_t(kBonePelvis)].x;
        CHECK(carried <= kReconcileMaxSpeedUu + 1e-6);
    }
}

// Closed loop: the stand-in runs 20 uu behind a still owner; the owner's pose arrives 60 ms late every frame. The
// correction given since a pose's time is not given again, so the error goes to zero without overshooting.
static void TestClosedLoopNoOvershoot() {
    Reconciler r;
    const double dt = 1000.0 / 60.0, delay = 60.0;
    double x = -20.0, carried = 0, worst = 0;
    std::vector<std::pair<double, double>> history;   // (owner time, stand-in x): what Record saw
    for (int f = 0; f < 240; ++f) {
        const double t = 1000.0 + f * dt;
        r.Record(t, Body(x));
        history.push_back({t, x});
        if (t - delay >= 1000.0) r.OnAuth(Pose(uint32_t(std::lround(t - delay)), 0.0));   // the owner never moved: x = 0
        carried += r.Step(dt).linUu[size_t(kBonePelvis)].x;
        x += carried * dt / 1000.0;
        worst = std::max(worst, x);   // overshoot: past 0
    }
    CHECK(std::fabs(x) < 0.5);       // converged
    CHECK(worst < 2.0);              // overshot by less than 10% of the error
}

static void TestNetStats() {
    Series s;
    for (int i = 1; i <= 100; ++i) s.Add(1000 + i, double(i));
    CHECK(s.Count(1200) == 100);
    CHECK(std::fabs(s.Quantile(1200, 0.5) - 50) < 1e-9 && std::fabs(s.Quantile(1200, 0.95) - 95) < 1e-9);
    CHECK(s.Quantile(1200, 0.0) == 1 && s.Quantile(1200, 1.0) == 100);
    CHECK(std::fabs(s.ShareBelow(1200, 11) - 0.10) < 1e-9);
    CHECK(s.Count(1000 + 100 + kStatsWindowMs + 1) == 0);    // the window moved past them
    CHECK(std::isnan(s.Quantile(1e9, 0.5)));
    for (int i = 0; i < int(kStatsCap) + 50; ++i) s.Add(9000, 1);
    CHECK(s.Count(9000) == kStatsCap);
    CHECK(StalenessMs(30, 60, 0) == 60 && StalenessMs(30, 60, 100) == 0);
    FighterNetStats f;
    f.staleness.Add(10, 80); f.error.Add(10, 5);
    const std::string line = FighterLine(4, f, 10);
    CHECK(line.find("e4 stale p50 80") == 0);
    f.predicted.Add(10, 1); f.predicted.Add(10, 0); f.predicted.Add(10, 0); f.predicted.Add(10, 0);
    CHECK(FighterLine(4, f, 10).find("predicted 25%") != std::string::npos);
    ClaimNetStats c; c.ok = 3; c.dist.Add(10, 12);
    CHECK(ClaimLine(c, 10).find("ok 3") != std::string::npos);
}

static void TestPanelAndFloor() {
    PanelRow a; a.name = "Bob"; a.rttMs = 62; a.relayed = 0; a.quality = 0.99; a.staleP50 = 48; a.lateShare = 0.03;
    PanelRow b; b.name = "Al";
    const std::vector<std::string> p = NetPanel(50, true, {a, b});
    CHECK(p.size() == 3 && p[0] == "netcode: input delay 50 ms, poses sent right after physics");
    CHECK(p[1] == "Bob: 62 ms direct, loss 1% | shown 48 ms late | inputs late 3%");
    CHECK(p[2] == "Al: ping ?");
    CHECK(NetPanel(0, false, {}).back() == "netcode: no other players");
    FrameFloor off;
    CHECK(!off.Push(100, 0) && !off.Push(100, 10000));
    FrameFloor f(60);
    double t = 0; bool warned = false;
    for (int i = 0; i < 400; ++i) { t += 25; warned = f.Push(25, t) || warned; }   // 40 FPS for 10 s
    CHECK(warned && std::fabs(f.LastFps() - 40) < 1e-6);
    bool again = false;
    for (int i = 0; i < 400; ++i) { t += 25; again = f.Push(25, t) || again; }       // still low: not again within a minute
    CHECK(!again);
    FrameFloor g(60); bool any = false; t = 0;
    for (int i = 0; i < 1200; ++i) { t += 10; any = g.Push(10, t) || any; }           // 100 FPS: never
    CHECK(!any);
}

int main() {
    TestPanelAndFloor();
    TestErrorAndDecay();
    TestOutsideHistoryAndOrder();
    TestSnap();
    TestRotation();
    TestWeaponAbsent();
    TestSpeedCap();
    TestClosedLoopNoOvershoot();
    TestNetStats();
    if (g_fail == 0) std::printf("test_hsm_reconcile: all passed\n");
    return g_fail == 0 ? 0 : 1;
}

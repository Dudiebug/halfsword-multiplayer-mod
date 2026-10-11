// Unit tests for hsm_jitter.h. Run via implementation/shared/run_tests.sh test_hsm_jitter.
#include "hsm_jitter.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

// One-body willie at x (cm), identity rotation.
static WillieState Pose(float x) { WillieState w; w.bodies.resize(1); w.bodies[0].px = x; return w; }

// Constant link latency: host time of tick t is t * period; the packet arrives latencyMs later.
static constexpr int64_t kLatencyMs = 500;

static void TestEmptyAndSingle() {
    JitterBuffer<> jb(20.0);
    SampledPose p;
    CHECK(!jb.Sample(1000, p));
    CHECK(jb.size() == 0);
    CHECK(Near(jb.delayMs(), kJitterInitialDelayMs, 1e-9));   // starts at 100 ms
    CHECK(jb.Push(10, 1000, Pose(50.f)));
    CHECK(Near(jb.delayMs(), kJitterInitialDelayMs, 1e-9));
    for (int64_t now : {int64_t(0), int64_t(1000), int64_t(5000)}) {
        CHECK(jb.Sample(now, p));
        CHECK(p.count == 1 && p.bodies[0].px == 50.f && !p.extrapolated && !p.capped);   // single sample: hold
        CHECK(p.bodies[0].qw == 1.f && p.bodies[0].qx == 0.f);
    }
    jb.Reset();
    CHECK(jb.size() == 0 && !jb.Sample(1000, p));
}

static void TestInterpolation() {
    JitterBuffer<> jb(20.0);                       // 50 Hz snapshots
    for (uint32_t t = 0; t <= 20; ++t) CHECK(jb.Push(t, 20 * int64_t(t) + kLatencyMs, Pose(10.f * float(t))));
    // Zero jitter: delay clamps to the 50 ms floor; offset is exactly the link latency.
    CHECK(Near(jb.delayMs(), kJitterMinDelayMs, 1e-9));
    SampledPose p;
    const double delay = jb.delayMs();
    // Host time 210 ms is halfway between ticks 10 (200 ms) and 11 (220 ms): expect x = 105.
    CHECK(jb.Sample(int64_t(210 + delay + kLatencyMs), p));
    CHECK(p.count == 1 && !p.extrapolated && Near(p.bodies[0].px, 105.0));
    // Exactly on a tick.
    CHECK(jb.Sample(int64_t(200 + delay + kLatencyMs), p) && Near(p.bodies[0].px, 100.0));
    // Between ticks 3 (60 ms) and 4 (80 ms), at 75 ms: x = 37.5.
    CHECK(jb.Sample(int64_t(75 + delay + kLatencyMs), p) && Near(p.bodies[0].px, 37.5));
    // Before the oldest buffered snapshot: hold the oldest.
    CHECK(jb.Sample(0, p) && Near(p.bodies[0].px, 0.0));
}

static void TestWrapAround() {
    JitterBuffer<> jb(20.0);
    const uint32_t start = 0xFFFFFFF8u;            // ticks cross 2^32 - 1 -> 0 mid-run
    for (uint32_t k = 0; k <= 10; ++k) {
        uint32_t tick = start + k;
        int64_t hostMs = 20 * (int64_t(4294967288ll) + int64_t(k));   // unwrapped tick * period
        CHECK(jb.Push(tick, hostMs + kLatencyMs, Pose(10.f * float(k))));
    }
    CHECK(!jb.Push(start + 4, 0, Pose(0.f)));       // an older tick after wrap-around is rejected
    const double delay = jb.delayMs(), base = 20.0 * 4294967288.0;
    SampledPose p;
    // Between ticks start+3 (k=3) and start+4 (k=4), halfway: x = 35.
    CHECK(jb.Sample(int64_t(base + 20.0 * 3.5 + delay + kLatencyMs), p));
    CHECK(Near(p.bodies[0].px, 35.0));
    // Across the seam between k=7 (tick 0xFFFFFFFF) and k=8 (tick 0): x = 75.
    CHECK(jb.Sample(int64_t(base + 20.0 * 7.5 + delay + kLatencyMs), p));
    CHECK(Near(p.bodies[0].px, 75.0));
}

static void TestDuplicatesAndOlder() {
    JitterBuffer<> jb(20.0);
    CHECK(jb.Push(5, 600, Pose(1.f)));
    CHECK(!jb.Push(5, 610, Pose(2.f)));             // duplicate
    CHECK(!jb.Push(4, 620, Pose(3.f)));             // older
    CHECK(jb.Push(6, 640, Pose(4.f)));
    CHECK(jb.size() == 2);
    SampledPose p;
    CHECK(jb.Sample(0, p) && Near(p.bodies[0].px, 1.0));   // only the first accepted sample is held
}

static void TestBounded() {
    JitterBuffer<8> jb(20.0);
    for (uint32_t t = 0; t < 100; ++t) CHECK(jb.Push(t, 20 * int64_t(t) + kLatencyMs, Pose(float(t))));
    CHECK(jb.size() == 8);
    SampledPose p;
    // Only the last eight (ticks 92..99) are kept, so an early render time holds tick 92.
    CHECK(jb.Sample(0, p) && Near(p.bodies[0].px, 92.0));
}

static void TestExtrapolationCap() {
    JitterBuffer<> jb(20.0);
    // Velocity: x goes 0 -> 10 cm over one 20 ms period, i.e. 0.5 cm/ms.
    CHECK(jb.Push(0, 0 + kLatencyMs, Pose(0.f)));
    CHECK(jb.Push(1, 20 + kLatencyMs, Pose(10.f)));
    const double delay = jb.delayMs();
    SampledPose p;
    auto at = [&](double renderHostMs) {
        CHECK(jb.Sample(int64_t(renderHostMs + delay + kLatencyMs), p));
    };
    at(20 + 100);                                   // 100 ms past the newest snapshot
    CHECK(p.extrapolated && !p.capped && Near(p.bodies[0].px, 60.0));
    at(20 + 150);                                   // exactly the cap
    CHECK(p.extrapolated && !p.capped && Near(p.bodies[0].px, 85.0));
    at(20 + 400);                                   // past the cap: frozen at the 150 ms position
    CHECK(p.extrapolated && p.capped && Near(p.bodies[0].px, 85.0));
    at(20 + 5000);
    CHECK(p.capped && Near(p.bodies[0].px, 85.0));  // holds, no runaway
}

static void TestQuaternions() {
    JitterBuffer<> jb(20.0);
    // Tick 0: identity. Tick 1: the same rotation with the sign flipped (opposite hemisphere).
    WillieState a = Pose(0.f), b = Pose(10.f);
    b.bodies[0].rot = Quat16{0, 0, 0, -32767};
    CHECK(jb.Push(0, kLatencyMs, a));
    CHECK(jb.Push(1, 20 + kLatencyMs, b));
    const double delay = jb.delayMs();
    SampledPose p;
    CHECK(jb.Sample(int64_t(10 + delay + kLatencyMs), p));
    const SmoothBody& s = p.bodies[0];
    CHECK(Near(s.qw, 1.0, 1e-4) && Near(s.qx, 0.0, 1e-4) && Near(s.qy, 0.0, 1e-4) && Near(s.qz, 0.0, 1e-4));  // shortest arc
    CHECK(Near(std::sqrt(s.qx * s.qx + s.qy * s.qy + s.qz * s.qz + s.qw * s.qw), 1.0, 1e-5));

    // 90 degrees about z, blended halfway: unit length, between identity and the target.
    JitterBuffer<> jq(20.0);
    WillieState c = Pose(0.f), d = Pose(0.f);
    d.bodies[0].rot = PackQuat(0.f, 0.f, 0.70710678f, 0.70710678f);
    CHECK(jq.Push(0, kLatencyMs, c));
    CHECK(jq.Push(1, 20 + kLatencyMs, d));
    const double dq = jq.delayMs();
    CHECK(jq.Sample(int64_t(10 + dq + kLatencyMs), p));
    const SmoothBody& m = p.bodies[0];
    CHECK(Near(std::sqrt(m.qx * m.qx + m.qy * m.qy + m.qz * m.qz + m.qw * m.qw), 1.0, 1e-5));
    CHECK(m.qz > 0.1f && m.qz < 0.7072f && m.qw > 0.7f && m.qw < 1.0f);

    // Hold path returns a unit quaternion too.
    CHECK(jq.Sample(0, p));
    const SmoothBody& h = p.bodies[0];
    CHECK(Near(std::sqrt(h.qx * h.qx + h.qy * h.qy + h.qz * h.qz + h.qw * h.qw), 1.0, 1e-5));
}

static void TestBodyCountChange() {
    JitterBuffer<> jb(20.0);
    WillieState two = Pose(10.f);
    two.bodies.resize(2); two.bodies[1].px = 99.f;
    CHECK(jb.Push(0, kLatencyMs, Pose(0.f)));
    CHECK(jb.Push(1, 20 + kLatencyMs, two));
    const double delay = jb.delayMs();
    SampledPose p;
    CHECK(jb.Sample(int64_t(10 + delay + kLatencyMs), p));
    CHECK(p.count == 2 && Near(p.bodies[0].px, 10.0));   // no blend across a body-list change: take the newer
    WillieState big; big.bodies.resize(kMaxBodies + 1);
    CHECK(!jb.Push(2, 40 + kLatencyMs, big));
}

// Interarrival jitter: alternate arrival spacing by +/- dev ms around the host spacing.
template <size_t C>
static void FeedJitter(JitterBuffer<C>& jb, uint32_t n, int64_t dev) {
    for (uint32_t t = 0; t < n; ++t) {
        int64_t wobble = (t % 2) ? dev : -dev;
        CHECK(jb.Push(t, 20 * int64_t(t) + kLatencyMs + wobble, Pose(float(t))));
    }
}

static void TestAdaptiveDelay() {
    JitterBuffer<> steady(20.0);
    for (uint32_t t = 0; t < 200; ++t) CHECK(steady.Push(t, 20 * int64_t(t) + kLatencyMs, Pose(0.f)));
    CHECK(Near(steady.delayMs(), kJitterMinDelayMs, 1e-9));          // no jitter -> lower clamp

    JitterBuffer<> mild(20.0);
    FeedJitter(mild, 200, 20);                                       // |D| ~ 40 ms
    CHECK(mild.jitterMs() > 30.0 && mild.jitterMs() < 45.0);
    CHECK(mild.delayMs() > 120.0 && mild.delayMs() < 220.0);

    JitterBuffer<> wild(20.0);
    FeedJitter(wild, 400, 300);                                      // |D| ~ 600 ms -> upper clamp
    CHECK(Near(wild.delayMs(), kJitterMaxDelayMs, 1e-9));

    // A fast host rate also clamps at the floor: 2 periods of 10 ms is 20 ms, below 50.
    JitterBuffer<> fast(10.0);
    for (uint32_t t = 0; t < 50; ++t) CHECK(fast.Push(t, 10 * int64_t(t) + kLatencyMs, Pose(0.f)));
    CHECK(Near(fast.delayMs(), kJitterMinDelayMs, 1e-9));
}

static void TestBoundedDelayAndSizeAfterReset() {
    JitterBuffer<4> jb(20.0);
    FeedJitter(jb, 30, 5);
    CHECK(jb.size() == 4);
    CHECK(jb.delayMs() >= kJitterMinDelayMs && jb.delayMs() <= kJitterMaxDelayMs);
    jb.Reset();
    CHECK(jb.size() == 0);
    CHECK(Near(jb.delayMs(), kJitterInitialDelayMs, 1e-9));
    CHECK(jb.Push(0, 5, Pose(7.f)));                               // accepts ticks again after reset
    SampledPose p;
    CHECK(jb.Sample(0, p) && Near(p.bodies[0].px, 7.0));
}

int main() {
    TestEmptyAndSingle();
    TestInterpolation();
    TestWrapAround();
    TestDuplicatesAndOlder();
    TestBounded();
    TestExtrapolationCap();
    TestQuaternions();
    TestBodyCountChange();
    TestAdaptiveDelay();
    TestBoundedDelayAndSizeAfterReset();
    if (g_fail) { std::printf("test_hsm_jitter: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_jitter: all passed\n");
    return 0;
}

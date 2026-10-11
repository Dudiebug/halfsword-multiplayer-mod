// Unit tests for hsm_prediction.h. Run via implementation/shared/run_tests.sh test_hsm_prediction.
#include "hsm_prediction.h"
#include <cmath>
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static BodyState B(float x, float y = 0, float z = 0) { BodyState b; b.px = x; b.py = y; b.pz = z; return b; }
static bool Near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) <= tol; }

static void TestPoseHistory() {
    PoseHistory<8> h;
    CHECK(h.empty() && h.Find(0) == nullptr);
    for (uint32_t s = 0; s < 20; ++s) { BodyState v = B(float(s)); CHECK(h.Push(s, &v, 1)); }
    BodyState two[2] = {B(1), B(2)};
    CHECK(!h.Push(19, two, 2));                       // duplicate rejected
    CHECK(!h.Push(10, two, 2));                       // older rejected
    CHECK(h.newest() == 19);
    const PoseEntry* e = h.Find(19);
    CHECK(e && e->seq == 19 && e->count == 1 && e->bodies[0].px == 19.f);
    CHECK(h.Find(12) != nullptr && h.Find(12)->bodies[0].px == 12.f);   // last 8 retained
    CHECK(h.Find(11) == nullptr);                     // evicted (shares slot with 19)
    CHECK(h.Find(20) == nullptr);                     // never pushed
    CHECK(h.Push(20, two, 2) && h.Find(20)->count == 2);   // two-body entry after eviction still works
    CHECK(h.Find(12) == nullptr);                     // 12 evicted by 20 (same slot)
    h.Clear();
    CHECK(h.empty() && h.Find(19) == nullptr);
    CHECK(h.Push(3, two, 2) && h.Find(3) != nullptr);  // accepts any seq after Clear
    BodyState big[kMaxBodies + 1] = {};
    CHECK(!h.Push(4, big, kMaxBodies + 1));            // too many bodies
}

static void TestPoseHistoryWrap() {
    PoseHistory<8> w;
    const uint32_t start = 0xFFFFFFFCu;               // seqs cross 2^32 - 1 -> 0
    for (uint32_t k = 0; k < 10; ++k) { BodyState v = B(float(k)); CHECK(w.Push(start + k, &v, 1)); }
    CHECK(w.newest() == 5u);
    CHECK(!w.Push(0xFFFFFFFFu, nullptr, 0));          // older across the wrap: rejected
    for (uint32_t k = 2; k < 10; ++k) {
        const PoseEntry* e = w.Find(start + k);
        CHECK(e && e->bodies[0].px == float(k));
    }
    CHECK(w.Find(start) == nullptr && w.Find(start + 1) == nullptr);   // evicted
    CHECK(w.Push(6, nullptr, 0) && w.Find(6) && w.Find(6)->count == 0);
}

static void TestReconcilerBands() {
    PoseHistory<16> h;
    Reconciler r;
    Reconciler::Correction c;
    const float dt = 1.f / 60.f;
    BodyState host[2];

    // Within the deadzone: nothing happens.
    BodyState pred[2] = {B(0), B(0, 0, 0)};
    h.Push(5, pred, 2);
    host[0] = B(1.0f); host[1] = B(0, 1.9f, 0);
    CHECK(r.Evaluate(h, 5, host, 2, dt, c) == Reconciler::Mode::None);
    CHECK(c.velocity[0].x == 0 && c.velocity[1].y == 0);
    host[0] = B(1.99f);
    h.Push(6, pred, 2);
    CHECK(r.Evaluate(h, 6, host, 2, dt, c) == Reconciler::Mode::None);   // 1.99 cm < deadzone

    // Between deadzone and snap: velocity = error * gain / dt.
    host[0] = B(10.f); host[1] = B(0);
    h.Push(7, pred, 2);
    CHECK(r.Evaluate(h, 7, host, 2, dt, c) == Reconciler::Mode::Correct);
    CHECK(Near(c.velocity[0].x, 90.f) && Near(c.velocity[0].y, 0.f) && Near(c.velocity[0].z, 0.f));
    CHECK(c.velocity[1].x == 0 && c.velocity[1].y == 0 && c.velocity[1].z == 0);
    CHECK(Near(c.maxErrorCm, 10.f));
    // Halving dt doubles the velocity, so displacement over a frame stays gain * error.
    h.Push(8, pred, 2); host[0] = B(10.f);
    CHECK(r.Evaluate(h, 8, host, 2, dt / 2, c) == Reconciler::Mode::Correct && Near(c.velocity[0].x, 180.f));

    // Speed cap.
    host[0] = B(100.f);
    h.Push(9, pred, 2);
    CHECK(r.Evaluate(h, 9, host, 2, dt, c) == Reconciler::Mode::Correct);
    CHECK(Near(c.velocity[0].x, kReconcileMaxSpeedCmS, 1e-2f));

    // Per-body: one body inside the deadzone gets zero while the other is corrected.
    BodyState p3[2] = {B(0), B(0)};
    host[0] = B(1.f); host[1] = B(0, 20.f, 0);
    h.Push(10, p3, 2);
    CHECK(r.Evaluate(h, 10, host, 2, dt, c) == Reconciler::Mode::Correct);
    CHECK(c.velocity[0].x == 0 && Near(c.velocity[1].y, 20.f * kReconcileGain / dt));

    // Snap threshold (inclusive) and beyond.
    host[0] = B(kPoseSnapCm); host[1] = B(0);
    h.Push(11, pred, 2);
    CHECK(r.Evaluate(h, 11, host, 2, dt, c) == Reconciler::Mode::Snap);
    CHECK(Near(c.maxErrorCm, kPoseSnapCm) && c.velocity[0].x == 0);
    host[0] = B(200.f);
    h.Push(12, pred, 2);
    CHECK(r.Evaluate(h, 12, host, 2, dt, c) == Reconciler::Mode::Snap);
    host[0] = B(std::numeric_limits<float>::quiet_NaN());
    h.Push(13, pred, 2);
    CHECK(r.Evaluate(h, 13, host, 2, dt, c) == Reconciler::Mode::Snap);

    // Stale: no history, body count mismatch, ack not newer than the last evaluated one.
    host[0] = B(10.f);
    CHECK(r.Evaluate(h, 99, host, 2, dt, c) == Reconciler::Mode::Stale);
    BodyState one[1] = {B(0)};
    h.Push(14, one, 1);
    CHECK(r.Evaluate(h, 14, host, 2, dt, c) == Reconciler::Mode::Stale);
    h.Push(15, pred, 2);
    CHECK(r.Evaluate(h, 15, host, 2, dt, c) == Reconciler::Mode::Correct);
    CHECK(r.Evaluate(h, 15, host, 2, dt, c) == Reconciler::Mode::Stale);     // same ack evaluated once
    h.Push(16, pred, 2);
    CHECK(r.Evaluate(h, 14, host, 2, dt, c) == Reconciler::Mode::Stale);     // older ack
    CHECK(r.Evaluate(h, 16, host, 2, dt, c) == Reconciler::Mode::Correct);
    r.Reset();
    CHECK(r.Evaluate(h, 16, host, 2, dt, c) == Reconciler::Mode::Correct);  // allowed again after Reset
}

// Zero-latency closed loop: each frame records the corrected pose, then acks it and applies the velocity.
// The error must shrink monotonically, never reverse direction, and settle inside the deadzone.
static void TestConvergence() {
    const float dt = 1.f / 60.f;
    const float starts[3] = {30.f, 90.f, 140.f};     // 140 hits the speed cap at first
    for (float start : starts) {
        const float dx = 0.70710678f, dy = 0.70710678f;   // diagonal start direction
        BodyState cur[2] = {B(start * dx, start * dy, 0), B(0)};
        BodyState host[2] = {B(0), B(0)};
        PoseHistory<64> h;
        Reconciler r;
        Reconciler::Correction c;
        float prevErr = start;
        bool settled = false;
        uint32_t seq = 1;
        for (int f = 0; f < 600; ++f, ++seq) {
            h.Push(seq, cur, 2);
            Reconciler::Mode m = r.Evaluate(h, seq, host, 2, dt, c);
            CHECK(m == Reconciler::Mode::Correct || m == Reconciler::Mode::None);
            if (settled) CHECK(m == Reconciler::Mode::None);            // no re-entry once settled
            if (m == Reconciler::Mode::Correct) {
                cur[0].px += c.velocity[0].x * dt; cur[0].py += c.velocity[0].y * dt; cur[0].pz += c.velocity[0].z * dt;
            } else {
                settled = true;
            }
            float err = std::sqrt(cur[0].px * cur[0].px + cur[0].py * cur[0].py + cur[0].pz * cur[0].pz);
            CHECK(err <= prevErr + 1e-3f);                                   // monotone shrink
            float along = cur[0].px * dx + cur[0].py * dy;
            CHECK(along >= -1e-3f);                                          // never overshoots past the host
            prevErr = err;
        }
        CHECK(settled);
        CHECK(prevErr < kPoseDeadzoneCm && prevErr > 0.8f * kPoseDeadzoneCm * (1.f - kReconcileGain));
        CHECK(cur[1].px == 0 && cur[1].py == 0);                             // untouched body stays put
    }
}

static void TestNonFiniteAndSize() {
    PoseHistory<4> h;
    BodyState p = B(0);
    h.Push(1, &p, 1);
    Reconciler r;
    Reconciler::Correction c;
    BodyState host = B(std::numeric_limits<float>::infinity());
    CHECK(r.Evaluate(h, 1, &host, 1, 1.f / 60.f, c) == Reconciler::Mode::Snap);
    CHECK(r.Evaluate(h, 1, &host, 1, 0.f, c) == Reconciler::Mode::Stale);   // dt must be positive
}

int main() {
    TestPoseHistory();
    TestPoseHistoryWrap();
    TestReconcilerBands();
    TestConvergence();
    TestNonFiniteAndSize();
    if (g_fail) { std::printf("test_hsm_prediction: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_prediction: all passed\n");
    return 0;
}

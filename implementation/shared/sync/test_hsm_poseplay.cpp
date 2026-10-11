// Unit tests for hsm_poseplay.h. Run via implementation/shared/run_tests.sh test_hsm_poseplay.
//
// Several tests below are ported/adapted from Cyrex0/HalfSword-MP crates/hsmp-pose/src/poseplay.rs
// `#[cfg(test)] mod tests`, Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see
// THIRD_PARTY_NOTICES.md). HSMP features the header deliberately skips (see its top-of-file
// comment) have no corresponding test here.
#include "sync/hsm_poseplay.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

static bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

static double Dist3(const V3& a, const V3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
static double QLen(const Q4& q) { return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); }

static bool FiniteV3(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
static bool FiniteQ4(const Q4& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }
static bool FiniteFrame(const PoseFrame& f) {
    for (const auto& b : f.bones) {
        if (!FiniteV3(b.pos) || !FiniteQ4(b.rot) || !FiniteV3(b.vel) || !FiniteV3(b.angVelDeg)) return false;
    }
    for (const auto& w : f.weapons) {
        if (!FiniteV3(w.pos) || !FiniteQ4(w.rot) || !FiniteV3(w.vel) || !FiniteV3(w.angVelDeg) || !FiniteV3(w.base) || !FiniteV3(w.tip)) return false;
    }
    return true;
}
static bool UnitRotations(const PoseFrame& f, double tol = 1e-4) {
    for (const auto& b : f.bones) {
        if (!Near(QLen(b.rot), 1.0, tol)) return false;
    }
    for (const auto& w : f.weapons) {
        if (w.present && !Near(QLen(w.rot), 1.0, tol)) return false;
    }
    return true;
}

// Every bone/weapon placed at a shared rigid-body pose: pos0 + vel*(tsMs-t0Ms)/1000, a fixed
// rotation and angular velocity -- enough to drive Push/Sample without per-bone FK detail.
static PoseFrame LinearFrame(uint32_t tsMs, double t0Ms, const V3& pos0, const V3& vel, const Q4& rot = Q4{0.0, 0.0, 0.0, 1.0},
                              const V3& angVel = V3{}) {
    PoseFrame f{};
    f.tsMs = tsMs;
    const double dt = (static_cast<double>(tsMs) - t0Ms) / 1000.0;
    const V3 pos{pos0.x + vel.x * dt, pos0.y + vel.y * dt, pos0.z + vel.z * dt};
    for (auto& b : f.bones) {
        b.pos = pos;
        b.rot = rot;
        b.vel = vel;
        b.angVelDeg = angVel;
    }
    for (auto& w : f.weapons) {
        w.present = true;
        w.hand = 0;
        w.classHash = 1;
        w.pos = pos;
        w.rot = rot;
        w.vel = vel;
        w.angVelDeg = angVel;
        w.base = pos;
        w.tip = pos;
    }
    return f;
}

// Body translating along +X at `speed` uu/s; the right hand (kBoneHandR) and weapon 0 swing on a
// `radius`-uu circle in the XY plane at `w` rad/s, with analytic position + exact velocity and a
// matching Z rotation -- same shape as poseplay.rs's test helper `frame_at` (poseplay.rs:858-868).
static V3 SwingPos(double tsMs, double speed, double w, double radius) {
    const double t = tsMs / 1000.0;
    return V3{speed * t + radius * std::cos(w * t), radius * std::sin(w * t), 140.0};
}
static PoseFrame SwingFrame(uint32_t tsMs, double speed, double w, double radius) {
    PoseFrame f{};
    f.tsMs = tsMs;
    const double t = static_cast<double>(tsMs) / 1000.0;
    const V3 bodyPos{speed * t, 0.0, 100.0};
    const V3 bodyVel{speed, 0.0, 0.0};
    for (auto& b : f.bones) {
        b.pos = bodyPos;
        b.rot = Q4{0.0, 0.0, 0.0, 1.0};
        b.vel = bodyVel;
        b.angVelDeg = V3{};
    }
    const double c = std::cos(w * t), s = std::sin(w * t);
    const double degPerSec = w * (180.0 / 3.14159265358979323846);
    const Q4 swingRot{0.0, 0.0, std::sin(w * t / 2.0), std::cos(w * t / 2.0)};
    const V3 swingAngVel{0.0, 0.0, degPerSec};
    f.bones[kBoneHandR].pos = V3{speed * t + radius * c, radius * s, 140.0};
    f.bones[kBoneHandR].vel = V3{speed - radius * w * s, radius * w * c, 0.0};
    f.bones[kBoneHandR].rot = swingRot;
    f.bones[kBoneHandR].angVelDeg = swingAngVel;
    const double wr = radius + 50.0;
    f.weapons[0].present = true;
    f.weapons[0].pos = V3{speed * t + wr * c, wr * s, 140.0};
    f.weapons[0].vel = V3{speed - wr * w * s, wr * w * c, 0.0};
    f.weapons[0].rot = swingRot;
    f.weapons[0].angVelDeg = swingAngVel;
    return f;
}

// ---------------------------------------------------------------------------------------------
// Ported from poseplay.rs `duplicates_and_reordering_are_handled`.
// ---------------------------------------------------------------------------------------------
static void TestDuplicatesAndReorderingHandled() {
    PosePlayer pp;
    PoseFrame f0 = LinearFrame(0, 0.0, V3{0, 0, 0}, V3{});
    PoseFrame f33 = LinearFrame(33, 0.0, V3{0, 0, 100}, V3{});
    PoseFrame f17 = LinearFrame(17, 0.0, V3{0, 0, 999}, V3{});  // distinctive marker: reveals mis-ordering
    CHECK(pp.Push(f0, 100));
    CHECK(pp.Push(f33, 140));
    CHECK(pp.Push(f17, 141));   // late-arriving: must be reordered into the middle, not appended
    CHECK(!pp.Push(f17, 142));  // exact duplicate (same tsMs): rejected
    CHECK(pp.size() == 3);

    // Sampling exactly at frame 17's sender time must reproduce it exactly: if the reorder had
    // instead appended it after frame 33 (or dropped it), this would interpolate between frame 0
    // and frame 33 (landing near z=50), not frame 17's distinctive z=999.
    const double offset = pp.offsetMs();
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(17.0 + offset + pp.delayMs())), out, &info));
    CHECK(Near(out.bones[kBonePelvis].pos.z, 999.0, 5.0));

    // Once played past, an old frame is late and ignored.
    CHECK(pp.Sample(1000, out, &info));
    CHECK(!pp.Push(LinearFrame(20, 0.0, V3{}, V3{}), 1001));
}

// ---------------------------------------------------------------------------------------------
// Ported from poseplay.rs `hermite_is_exact_on_endpoints_and_smooth` (the exactness half; the
// smoothness half is covered separately below with real frame-boundary sampling).
// ---------------------------------------------------------------------------------------------
static void TestHermiteExactOnEndpointsAndConstantVelocity() {
    CHECK(Near(ppdetail::Hermite(1.0, 0.0, 5.0, 0.0, 0.0, 10.0), 1.0, 1e-6));
    CHECK(Near(ppdetail::Hermite(1.0, 0.0, 5.0, 0.0, 1.0, 10.0), 5.0, 1e-6));
    // Constant velocity (matching tangents) is reproduced exactly: linear motion stays linear.
    const double v = 0.3;  // uu/ms
    for (double s : {0.1, 0.33, 0.5, 0.9}) {
        const double p = ppdetail::Hermite(0.0, v, 10.0 * v, v, s, 10.0);
        CHECK(Near(p, 10.0 * v * s, 1e-9));
    }
}

// New: Hermite continuity across a real frame boundary with *different* endpoint velocities (real
// curvature, not a line) -- exact at both frame times, and no per-step position jump far beyond
// what either endpoint's velocity could produce.
static void TestHermiteContinuityAcrossFrameBoundary() {
    PoseFrame a{}, b{};
    a.tsMs = 1000;
    b.tsMs = 1100;  // 100 ms segment, well under kPlayHermiteMaxGapMs (270 ms)
    const V3 pa{0, 0, 0}, va{50, 10, -5};
    const V3 pb{8, 2, -0.6}, vb{90, -30, 15};  // different end velocity: real curvature
    for (auto& bone : a.bones) {
        bone.pos = pa;
        bone.vel = va;
        bone.rot = Q4{0, 0, 0, 1};
    }
    for (auto& bone : b.bones) {
        bone.pos = pb;
        bone.vel = vb;
        bone.rot = Q4{0, 0, 0, 1};
    }

    PoseFrame atA, atB;
    ppdetail::InterpolateFrame(a, b, static_cast<double>(a.tsMs), atA);
    ppdetail::InterpolateFrame(a, b, static_cast<double>(b.tsMs), atB);
    CHECK(Near(Dist3(atA.bones[0].pos, pa), 0.0, 1e-6));
    CHECK(Near(Dist3(atB.bones[0].pos, pb), 0.0, 1e-6));

    const double maxSpeed = std::max(Dist3(V3{}, va), Dist3(V3{}, vb));  // uu/s
    const double stepMs = 0.25;
    PoseFrame prev = atA;
    for (double t = static_cast<double>(a.tsMs) + stepMs; t <= static_cast<double>(b.tsMs) + 1e-9; t += stepMs) {
        PoseFrame cur;
        ppdetail::InterpolateFrame(a, b, t, cur);
        const double step = Dist3(prev.bones[0].pos, cur.bones[0].pos);
        const double bound = 3.0 * maxSpeed * (stepMs / 1000.0) + 0.1;  // generous: catches gross bugs, not float noise
        CHECK(step <= bound);
        prev = cur;
    }
}

// New: a perfectly steady constant-velocity stream is reproduced exactly by Hermite interpolation
// (matching tangents make the cubic degenerate to the line), sampled through the full PosePlayer.
static void TestConstantVelocityStreamReproducedExactly() {
    PosePlayer pp;
    const V3 pos0{10.0, -5.0, 2.0}, vel{37.5, -12.25, 6.0};  // uu/s, constant for every frame
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 20; ++k) {
        const double ts = t0 + k * period;
        const int64_t rx = static_cast<int64_t>(std::llround(ts + 40.0));  // steady 40 ms latency, zero jitter
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, pos0, vel), rx));
    }
    CHECK(pp.hasOffset());

    const double atTarget = t0 + 9.37 * period;  // strictly inside [frame9, frame10], not a frame time
    const int64_t now = static_cast<int64_t>(std::llround(atTarget + pp.offsetMs() + pp.delayMs()));
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(now, out, &info));
    CHECK(!info.extrapolated);

    // `now` is whole ms, so the player samples at now - offset - delay (its first sample has no
    // earlier play point to hold), not exactly atTarget. The line through that exact time must match.
    const double at = static_cast<double>(now) - pp.offsetMs() - pp.delayMs();
    CHECK(std::fabs(at - atTarget) <= 0.5 + 1e-9);
    const double dt = (at - t0) / 1000.0;
    const V3 truth{pos0.x + vel.x * dt, pos0.y + vel.y * dt, pos0.z + vel.z * dt};
    for (const auto& bone : out.bones) CHECK(Dist3(bone.pos, truth) < 1e-6);
    for (const auto& w : out.weapons) CHECK(Dist3(w.pos, truth) < 1e-6);
}

// Ported from poseplay.rs `interpolation_tracks_a_fast_swing_closely`.
static void TestFastSwingTracksClosely() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double latencyMs = 40.0;
    const double speed = 300.0, w = 6.0, radius = 60.0;
    const double t0 = 10000.0;
    double lastTs = t0;
    for (int k = 0; k < 60; ++k) {
        const double ts = t0 + k * period;
        lastTs = ts;
        CHECK(pp.Push(SwingFrame(static_cast<uint32_t>(std::llround(ts)), speed, w, radius),
                       static_cast<int64_t>(std::llround(ts + latencyMs))));
    }
    const int64_t now = static_cast<int64_t>(std::llround(lastTs + latencyMs));
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(now, out, &info));
    CHECK(!info.extrapolated);
    CHECK(!info.stale);

    const V3 truthHand = SwingPos(static_cast<double>(out.tsMs), speed, w, radius);
    CHECK(Dist3(out.bones[kBoneHandR].pos, truthHand) < 1.0);
    const V3 truthPelvis{speed * (static_cast<double>(out.tsMs) / 1000.0), 0.0, 100.0};
    CHECK(Dist3(out.bones[kBonePelvis].pos, truthPelvis) < 0.2);
    // Buffer delay adapts to a steady, jitter-free stream: stays well under a frame interval plus
    // margin once settled.
    CHECK(info.delayMs < 40.0);
}

// New: delay adapts to jitter and always stays within [kPlayDelayMinMs, kPlayDelayMaxMs], settling
// near j95 + margin (the measured interval here is always < V2_EXTRAP_OK_MS, so the interval term
// contributes ~0).
static void TestDelayAdaptsToJitterWithinBounds() {
    PosePlayer pp;
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> jitter(-10.0, 10.0);
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 600; ++k) {
        const double ts = t0 + k * period;
        const double rx = ts + 40.0 + jitter(rng);
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(rx))));
        CHECK(pp.delayMs() >= kPlayDelayMinMs - 1e-9);
        CHECK(pp.delayMs() <= kPlayDelayMaxMs + 1e-9);
    }
    CHECK(pp.delayMs() > kPlayDelayMinMs);    // adapted away from the floor given ~20 ms of jitter
    CHECK(pp.delayMs() < kPlayDelayStartMs);  // ... and well below the 60 ms starting value
    const double expected = std::clamp(pp.j95Ms() + kPlayDelayMarginMs, kPlayDelayMinMs, kPlayDelayMaxMs);
    CHECK(std::fabs(pp.delayMs() - expected) < 12.0);

    // Asymmetry: delay must climb toward a sudden *increase* in jitter quickly (grow fast), not
    // just shrink quickly toward a decrease. A short zero-jitter settle, then a short burst of
    // consistently-positive extra jitter (never touching the clock offset, since it is one-sided),
    // is enough for the windowed 95th-percentile to register the burst within those few pushes.
    PosePlayer burstPp;
    std::mt19937 rng2(7);
    std::uniform_real_distribution<double> burstJitter(100.0, 150.0);
    for (int k = 0; k < 20; ++k) {
        const double ts = t0 + k * period;
        CHECK(burstPp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(ts + 40.0))));
    }
    const double delayBeforeBurst = burstPp.delayMs();
    for (int k = 20; k < 40; ++k) {
        const double ts = t0 + k * period;
        const double rx = ts + 40.0 + burstJitter(rng2);
        CHECK(burstPp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(rx))));
    }
    CHECK(burstPp.delayMs() - delayBeforeBurst > 40.0);  // fast growth closes most of the gap within 20 pushes
}

// New: past the newest frame, extrapolation holds full sender velocity through EXTRAP_FULL_MS,
// then eases to exactly zero velocity by EXTRAP_MS, and freezes (Hold) beyond that.
static void TestExtrapolationFullVelocityThenDecaysToZero() {
    PoseFrame prev{}, newest{};
    prev.tsMs = 1000;
    newest.tsMs = 1017;  // ~60 Hz spacing
    const V3 v0{200.0, 0.0, 0.0};
    for (auto& b : prev.bones) {
        b.vel = v0;
        b.rot = Q4{0, 0, 0, 1};
    }
    for (auto& b : newest.bones) {
        b.vel = v0;
        b.rot = Q4{0, 0, 0, 1};
    }

    auto extrapAt = [&](double dt) {
        PoseFrame out;
        ppdetail::Mode mode;
        ppdetail::ExtrapolateFrame(newest, prev, dt, 0.0, out, mode);
        return out;
    };

    for (double dt : {5.0, 20.0, 34.0}) {
        const PoseFrame out = extrapAt(dt);
        CHECK(Near(out.bones[0].pos.x, v0.x * dt / 1000.0, 1e-6));
        CHECK(Near(out.bones[0].vel.x, v0.x, 1e-6));  // undecayed through EXTRAP_FULL_MS
    }
    const PoseFrame at67 = extrapAt(67.0);
    CHECK(at67.bones[0].pos.x > v0.x * 34.0 / 1000.0);               // still advancing...
    CHECK(at67.bones[0].vel.x < v0.x && at67.bones[0].vel.x > 0.0);  // ...but slower than full speed
    const PoseFrame at100 = extrapAt(100.0);
    CHECK(Near(at100.bones[0].vel.x, 0.0, 1e-6));  // exactly zero at EXTRAP_MS
    const PoseFrame at150 = extrapAt(150.0);       // past EXTRAP_MS: frozen (Hold) at the dt=100 values
    CHECK(Near(at150.bones[0].pos.x, at100.bones[0].pos.x, 1e-9));
    CHECK(Near(at150.bones[0].vel.x, 0.0, 1e-6));
}

// New: extrapolated displacement never exceeds EXTRAP_MAX_UU, however fast the sender velocity.
static void TestExtrapolationDisplacementCapped() {
    PoseFrame prev{}, newest{};
    prev.tsMs = 1000;
    newest.tsMs = 1017;
    const V3 v0{100000.0, -50000.0, 20000.0};  // absurdly fast: guarantees the cap engages
    for (auto& b : prev.bones) {
        b.vel = v0;
        b.rot = Q4{0, 0, 0, 1};
    }
    for (auto& b : newest.bones) {
        b.vel = v0;
        b.rot = Q4{0, 0, 0, 1};
    }
    for (double dt : {1.0, 34.0, 80.0, 150.0}) {
        PoseFrame out;
        ppdetail::Mode mode;
        ppdetail::ExtrapolateFrame(newest, prev, dt, 0.0, out, mode);
        CHECK(Dist3(out.bones[0].pos, newest.bones[0].pos) <= kPlayExtrapMaxUu + 1e-6);
    }
}

// Ported from poseplay.rs `loss_extrapolates_bounded_then_holds_then_stale`.
static void TestLossExtrapolatesThenHoldsThenStale() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 30; ++k) {
        const double ts = t0 + k * period;
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{50, 0, 0}),
                       static_cast<int64_t>(std::llround(ts + 30.0))));
    }
    const double lastTs = t0 + 29 * period;
    const double now0 = lastTs + 30.0 + pp.delayMs() + 20.0;
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(now0)), out, &info));
    CHECK(info.extrapolated);
    CHECK(!info.stale);

    double t = now0;
    for (; t < now0 + 400.0; t += 8.0) {
        CHECK(pp.Sample(static_cast<int64_t>(std::llround(t)), out, &info));
        if (!info.extrapolated) break;
    }
    CHECK(!info.extrapolated);  // settled into Hold
    CHECK(!info.stale);

    double now = now0 + 100.0;
    bool stale = false;
    while (now < now0 + kPlayStaleMs + 400.0) {
        now += 16.0;
        CHECK(pp.Sample(static_cast<int64_t>(std::llround(now)), out, &info));
        if (info.stale) {
            stale = true;
            break;
        }
    }
    CHECK(stale);
}

// New: dedicated, minimal check of the 1000 ms stale threshold's boundary.
static void TestStaleAfter1000ms() {
    PosePlayer pp;
    CHECK(pp.Push(LinearFrame(10000, 10000.0, V3{}, V3{}), 10000));
    const double offset = pp.offsetMs();
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(10000.0 + offset + kPlayStaleMs - 50.0)), out, &info));
    CHECK(!info.stale);
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(10000.0 + offset + kPlayStaleMs + 50.0)), out, &info));
    CHECK(info.stale);
}

// Ported from poseplay.rs `teleport_cuts_history_and_snaps`; also exercises the explicit
// "cut true once, then false" requirement.
static void TestTeleportCutsHistoryAndSnaps() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 20; ++k) {
        const double ts = t0 + k * period;
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(ts + 30.0))));
    }
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(t0 + 19 * period + 30.0)), out, &info));
    CHECK(info.cut);  // first-ever sample always reports a cut

    PoseFrame teleported = LinearFrame(static_cast<uint32_t>(std::llround(t0 + 20 * period)), t0, V3{400.0, 0.0, 0.0}, V3{});
    CHECK(pp.Push(teleported, static_cast<int64_t>(std::llround(t0 + 20 * period + 30.0))));
    CHECK(pp.size() == 1);  // history dropped: a 400 uu teleport exceeds CUT_DIST_UU at well over CUT_SPEED_UUPS

    CHECK(pp.Sample(static_cast<int64_t>(std::llround(t0 + 20 * period + 31.0)), out, &info));
    CHECK(info.cut);
    CHECK(out.bones[kBonePelvis].pos.x > 300.0);  // shows the new place immediately, no blend-back

    CHECK(pp.Sample(static_cast<int64_t>(std::llround(t0 + 20 * period + 40.0)), out, &info));
    CHECK(!info.cut);  // the cut flag clears after being reported once
}

// Ported from poseplay.rs `a_long_gap_starts_over`; exercises the 700 ms stream-gap cut condition.
static void TestLongGapStartsOver() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 10; ++k) {
        const double ts = t0 + k * period;
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(ts + 30.0))));
    }
    const double ts = t0 + 9 * period + 700.0;  // 700 ms later, well past GAP_CUT_MS (600 ms)
    PoseFrame gapFrame = LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{50.0, 0.0, 0.0}, V3{});  // slow enough for a walk
    CHECK(pp.Push(gapFrame, static_cast<int64_t>(std::llround(ts + 30.0))));
    CHECK(pp.size() == 1);  // cut via the gap condition alone (distance is small, well under CUT_DIST_UU)
}

// New: the very first sample after construction always reports a cut.
static void TestFirstSampleReportsCutTrue() {
    PosePlayer pp;
    CHECK(pp.Push(LinearFrame(1000, 1000.0, V3{}, V3{}), 1000));
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(1001, out, &info));
    CHECK(info.cut);
}

// Ported from poseplay.rs `sender_restart_resets_buffer`.
static void TestSenderRestartResetsBuffer() {
    PosePlayer pp;
    const double period = 16.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 10; ++k) {
        const double ts = t0 + k * period;
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(ts + 20.0))));
    }
    CHECK(pp.size() == 10);
    // A sender timestamp far enough back (> RESTART_BACK_MS before the newest) restarts the stream.
    CHECK(pp.Push(LinearFrame(10, t0, V3{}, V3{}), 500));
    CHECK(pp.size() == 1);
}

// Ported from poseplay.rs `clock_drift_is_tracked`.
static void TestClockDriftIsTracked() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    int64_t now = 0;
    PoseFrame out;
    SampleInfo info;
    for (uint32_t k = 0; k < 3600; ++k) {
        const double ts = t0 + static_cast<double>(k) * period * 1.0005;  // sender clock runs 0.05% fast
        const double rx = static_cast<double>(k) * period + 40.0;
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{50, 0, 0}), static_cast<int64_t>(std::llround(rx))));
        now = static_cast<int64_t>(std::llround(rx));
        CHECK(pp.Sample(now, out, &info));
    }
    CHECK(pp.Sample(now + 1, out, &info));
    CHECK(!info.extrapolated);
    CHECK(!info.stale);
}

// Ported from poseplay.rs `a_two_second_step_back_resets_the_stream`: distinguishes a genuine
// restart (timestamp more than RESTART_BACK_MS behind the newest) from an ordinary reorder.
static void TestTwoSecondStepBackResetsStream() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 30; ++k) {
        const double ts = t0 + k * period;
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(ts + 30.0))));
    }
    const double newestTs = t0 + 29 * period;
    CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(newestTs - kPlayRestartBackMs - 1.0)), t0, V3{}, V3{}), 20000));
    CHECK(pp.size() == 1);  // old frames dropped (restart)

    PosePlayer pp2;
    for (int k = 0; k < 30; ++k) {
        const double ts = t0 + k * period;
        CHECK(pp2.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, V3{}), static_cast<int64_t>(std::llround(ts + 30.0))));
    }
    // A small reorder (well under RESTART_BACK_MS) is still a reorder, not a restart.
    CHECK(pp2.Push(LinearFrame(static_cast<uint32_t>(std::llround(t0 + 5.5)), t0, V3{}, V3{}), 20000));
    CHECK(pp2.size() == 31);  // inserted, old history kept (reorder, not restart)
    CHECK(Near(kPlayRestartBackMs, 2000.0));
}

// Ported from poseplay.rs `look_ahead_samples_past_the_clock_and_keeps_velocity`.
static void TestLookAheadKeepsVelocityPastClock() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    const V3 vel{300.0, 0.0, 0.0};
    int64_t rx = 0;
    for (int k = 0; k < 60; ++k) {
        const double ts = t0 + k * period;
        rx = static_cast<int64_t>(std::llround(ts + 40.0));
        CHECK(pp.Push(LinearFrame(static_cast<uint32_t>(std::llround(ts)), t0, V3{}, vel), rx));
    }
    PoseFrame out0, out1, out2, out3;
    SampleInfo info0, info1, info2, info3;
    CHECK(pp.Sample(rx, out0, &info0));
    CHECK(pp.Sample(rx, out1, &info1, 30.0));
    CHECK(Near(static_cast<double>(out1.tsMs) - static_cast<double>(out0.tsMs), 30.0, 1.5));
    const double dx = out1.bones[kBonePelvis].pos.x - out0.bones[kBonePelvis].pos.x;
    CHECK(Near(dx, vel.x * 30.0 / 1000.0, 0.5));  // 9 uu in 30 ms

    CHECK(pp.Sample(rx, out2, &info2, 110.0));
    CHECK(info2.extrapolated);
    CHECK(Near(out2.bones[kBonePelvis].vel.x, vel.x, 1.0));  // past the newest frame, lead keeps full velocity

    CHECK(pp.Sample(rx, out3, &info3));  // the playback clock itself is not moved by the look-ahead
    CHECK(Near(static_cast<double>(out3.tsMs), static_cast<double>(out0.tsMs), 1.0));
}

// New: rotations stay unit length across interpolation, extrapolation and hold/stale.
static void TestRotationsStayUnitLength() {
    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 10000.0;
    for (int k = 0; k < 40; ++k) {
        const double ts = t0 + k * period;
        CHECK(pp.Push(SwingFrame(static_cast<uint32_t>(std::llround(ts)), 300.0, 6.0, 60.0), static_cast<int64_t>(std::llround(ts + 30.0))));
    }
    const double lastTs = t0 + 39 * period;
    PoseFrame out;
    SampleInfo info;
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(t0 + 20 * period + 30.0)), out, &info));  // interpolating
    CHECK(UnitRotations(out));
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(lastTs + 30.0 + pp.delayMs() + 20.0)), out, &info));  // extrapolating
    CHECK(UnitRotations(out));
    CHECK(pp.Sample(static_cast<int64_t>(std::llround(lastTs + 30.0 + pp.delayMs() + 2000.0)), out, &info));  // held/stale
    CHECK(UnitRotations(out));
}

// New: no NaN/Inf and unit-length rotations for any sampled output under a randomized stream with
// jitter, loss and reordering (seeded, deterministic).
static void TestNoNaNUnderRandomizedStream() {
    std::mt19937 rng(20261009u);
    std::uniform_real_distribution<double> jitter(-25.0, 25.0);
    std::uniform_real_distribution<double> reorderShift(-40.0, 5.0);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_real_distribution<double> velRange(-400.0, 400.0);
    std::uniform_real_distribution<double> angVelRange(-720.0, 720.0);

    PosePlayer pp;
    const double period = 1000.0 / 60.0;
    const double t0 = 100000.0;
    for (int k = 0; k < 2000; ++k) {
        const double ts = t0 + k * period;
        const double rx = ts + 40.0 + jitter(rng);
        const bool lost = unit(rng) < 0.05;
        if (!lost) {
            double pushTs = ts;
            if (unit(rng) < 0.1) pushTs = ts + reorderShift(rng);
            const V3 pos0{unit(rng) * 2000.0 - 1000.0, unit(rng) * 2000.0 - 1000.0, unit(rng) * 200.0};
            const V3 vel{velRange(rng), velRange(rng), velRange(rng)};
            const Q4 rot = ppdetail::QNorm(Q4{unit(rng) - 0.5, unit(rng) - 0.5, unit(rng) - 0.5, unit(rng) - 0.5});
            const V3 angVel{angVelRange(rng), angVelRange(rng), angVelRange(rng)};
            const uint32_t tsMs = static_cast<uint32_t>(std::llround(std::max(0.0, pushTs)));
            pp.Push(LinearFrame(tsMs, pushTs, pos0, vel, rot, angVel), static_cast<int64_t>(std::llround(std::max(0.0, rx))));
        }
        PoseFrame out;
        SampleInfo info;
        if (pp.Sample(static_cast<int64_t>(std::llround(std::max(0.0, rx))), out, &info)) {
            CHECK(FiniteFrame(out));
            CHECK(UnitRotations(out, 1e-3));
        }
    }
}

int main() {
    TestDuplicatesAndReorderingHandled();
    TestHermiteExactOnEndpointsAndConstantVelocity();
    TestHermiteContinuityAcrossFrameBoundary();
    TestConstantVelocityStreamReproducedExactly();
    TestFastSwingTracksClosely();
    TestDelayAdaptsToJitterWithinBounds();
    TestExtrapolationFullVelocityThenDecaysToZero();
    TestExtrapolationDisplacementCapped();
    TestLossExtrapolatesThenHoldsThenStale();
    TestStaleAfter1000ms();
    TestTeleportCutsHistoryAndSnaps();
    TestLongGapStartsOver();
    TestFirstSampleReportsCutTrue();
    TestSenderRestartResetsBuffer();
    TestClockDriftIsTracked();
    TestTwoSecondStepBackResetsStream();
    TestLookAheadKeepsVelocityPastClock();
    TestRotationsStayUnitLength();
    TestNoNaNUnderRandomizedStream();

    if (g_fail == 0) {
        std::printf("test_hsm_poseplay: all passed\n");
        return 0;
    }
    std::printf("test_hsm_poseplay: %d failure(s)\n", g_fail);
    return 1;
}

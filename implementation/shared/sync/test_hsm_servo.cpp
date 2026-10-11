// Unit tests for hsm_servo.h. Run via implementation/shared/run_tests.sh test_hsm_servo.
#include "sync/hsm_servo.h"
#include "sync/hsm_pose.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;
using hsm::sync::detail::QConj;
using hsm::sync::detail::QMul;
using hsm::sync::detail::QRot;

static bool Near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

static double Dist3(const V3& a, const V3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Degrees between two rotations (avatars_pure.lua PURE.qangle).
static double QAngleDeg(const Q4& a, const Q4& b) {
    double d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    if (d > 1.0) d = 1.0;
    return 2.0 * std::acos(d) * 180.0 / detail::kPi;
}

static Q4 QAxisAngle(V3 axis, double deg) {
    const double l = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    const double h = (deg * detail::kPi / 180.0) * 0.5;
    const double s = std::sin(h) / l;
    return Q4{ axis.x * s, axis.y * s, axis.z * s, std::cos(h) };
}

// Free rigid-body integration: the COM moves with `cmd.vel`, the body rotates about its COM with
// `cmd.angVelDeg` (world axes), over dtS. Ported from tools/pose-truth/tests/servo_pure.lua `step`,
// used here to simulate a body driven by ServoBody over many frames.
static BodyNow IntegrateBody(const BodyNow& cur, const ServoCommand& cmd, double dtS) {
    const V3 comWorld = QRot(cur.rot, cur.comLocal);
    const V3 comNext{ cur.pos.x + comWorld.x + cmd.vel.x * dtS, cur.pos.y + comWorld.y + cmd.vel.y * dtS,
                       cur.pos.z + comWorld.z + cmd.vel.z * dtS };
    const double wr = detail::kPi / 180.0;
    const double ax = cmd.angVelDeg.x * wr * dtS, ay = cmd.angVelDeg.y * wr * dtS, az = cmd.angVelDeg.z * wr * dtS;
    const double a = detail::Len3(ax, ay, az);
    Q4 nextRot = cur.rot;
    if (a > 1e-12) {
        const double h = std::sin(a * 0.5) / a;
        const Q4 d{ ax * h, ay * h, az * h, std::cos(a * 0.5) };
        nextRot = QMul(d, cur.rot);
    }
    const V3 comWorldNext = QRot(nextRot, cur.comLocal);
    BodyNow out;
    out.rot = nextRot;
    out.comLocal = cur.comLocal;
    out.pos = V3{ comNext.x - comWorldNext.x, comNext.y - comWorldNext.y, comNext.z - comWorldNext.z };
    return out;
}

// -------------------------------------------------------------------------------------------
// 1. A body at rest with a static target converges, monotonically, without overshoot.
static void TestRestConverges() {
    const ServoParams p;
    BodyNow body;
    body.pos = V3{ 10, -5, 2 };
    body.rot = Q4{ 0, 0, 0, 1 };
    body.comLocal = V3{ 3, 0, 0 };
    BodyTarget tg;
    tg.pos = V3{ 100, 40, -20 };
    tg.rot = QAxisAngle(V3{ 0, 1, 0 }, 60);
    tg.vel = V3{ 0, 0, 0 };
    tg.angVelDeg = V3{ 0, 0, 0 };

    const double dt = 1.0 / 60.0;
    double prevErr = Dist3(body.pos, tg.pos);
    // |dot(cur.rot, tg.rot)| rises monotonically to 1 as the angular error shrinks to 0. Track
    // that instead of the angle itself: acos's derivative blows up near 0, so the angle computed
    // from a dot product within a few ULPs of 1 jitters by far more than the angle actually
    // changed -- a measurement artifact, not a sign the servo overshot.
    double prevDot = std::fabs(body.rot.x * tg.rot.x + body.rot.y * tg.rot.y + body.rot.z * tg.rot.z + body.rot.w * tg.rot.w);
    const double err0 = prevErr;
    for (int step = 0; step < 600; ++step) {
        const ServoCommand cmd = ServoBody(body, tg, dt, p.gain, p.capLinUu, p.capAngDeg);
        body = IntegrateBody(body, cmd, dt);
        const double err = Dist3(body.pos, tg.pos);
        double dot = std::fabs(body.rot.x * tg.rot.x + body.rot.y * tg.rot.y + body.rot.z * tg.rot.z + body.rot.w * tg.rot.w);
        if (dot > 1.0) dot = 1.0;
        // Monotonic decrease (allow float noise) and never overshoots past the original gap.
        CHECK(err <= prevErr + 1e-7);
        CHECK(dot >= prevDot - 1e-9);
        CHECK(err <= err0 + 1e-6);
        prevErr = err;
        prevDot = dot;
    }
    CHECK(prevErr < 0.05);
    CHECK(QAngleDeg(body.rot, tg.rot) < 0.05);
}

// -------------------------------------------------------------------------------------------
// 2. A target moving at constant linear + angular velocity is tracked with ~zero steady-state
// error thanks to the feed-forward term. The discrete law's exact steady-state residual is
// |v|*dt (derived from its recursion e[k+1] = (1-gain)*e[k] + gain*v*dt, independent of gain),
// not literally zero; picked here well under the 0.5 uu/deg bound, same as HSMP sees at typical
// walk speed and a 60 Hz step.
static void TestConstantVelocityTracking() {
    const ServoParams p;
    BodyNow body;
    body.pos = V3{ 0, 0, 0 };
    body.rot = Q4{ 0, 0, 0, 1 };
    body.comLocal = V3{ 2, -1, 0.5 };

    BodyTarget tg;
    tg.pos = V3{ 50, 20, 0 };
    tg.rot = QAxisAngle(V3{ 0, 0, 1 }, 10);
    tg.vel = V3{ 15, -8, 5 };
    tg.angVelDeg = V3{ 0, 0, 20 };

    const double dt = 1.0 / 60.0;
    for (int step = 0; step < 600; ++step) {
        const ServoCommand cmd = ServoBody(body, tg, dt, p.gain, p.capLinUu, p.capAngDeg);
        body = IntegrateBody(body, cmd, dt);
        tg = ExtrapolateTarget(tg, dt);
    }
    CHECK(Dist3(body.pos, tg.pos) < 0.5);
    CHECK(QAngleDeg(body.rot, tg.rot) < 0.5);
}

// -------------------------------------------------------------------------------------------
// 3. Caps: the correction (feed-forward excluded) never exceeds capLinUu / capAngDeg, and the
// returned command actually differs from an uncapped run (i.e. the cap engaged).
static void TestCapsEnforced() {
    BodyNow body;
    body.pos = V3{ 0, 0, 0 };
    body.rot = Q4{ 0, 0, 0, 1 };
    body.comLocal = V3{ 0, 0, 0 };
    BodyTarget tg;
    tg.pos = V3{ 5000, -3000, 1000 };  // huge one-step error
    tg.rot = QAxisAngle(V3{ 0.3, 1, 0.1 }, 150);
    tg.vel = V3{ 10, 10, 10 };
    tg.angVelDeg = V3{ 5, 5, 5 };

    const double dt = 1.0 / 60.0, gain = 1.0, capLin = 100.0, capAng = 50.0;
    const ServoCommand capped = ServoBody(body, tg, dt, gain, capLin, capAng);
    const ServoCommand uncapped = ServoBody(body, tg, dt, gain, 1e9, 1e9);
    CHECK(!Near(capped.vel.x, uncapped.vel.x, 1e-3) || !Near(capped.vel.y, uncapped.vel.y, 1e-3));  // cap engaged

    // Recompute the feed-forward f and tg.angVelDeg to check the correction magnitude directly.
    const V3 tc = QRot(tg.rot, body.comLocal);
    const double wr = detail::kPi / 180.0;
    const V3 f{ tg.vel.x + tg.angVelDeg.y * wr * tc.z - tg.angVelDeg.z * wr * tc.y,
                tg.vel.y + tg.angVelDeg.z * wr * tc.x - tg.angVelDeg.x * wr * tc.z,
                tg.vel.z + tg.angVelDeg.x * wr * tc.y - tg.angVelDeg.y * wr * tc.x };
    const double dl = Dist3(capped.vel, f);
    const double gl = Dist3(capped.angVelDeg, tg.angVelDeg);
    CHECK(dl <= capLin + 1e-6);
    CHECK(gl <= capAng + 1e-6);
    // The uncapped pre-cap delta really did exceed the cap (otherwise this test proves nothing).
    const double dlUncapped = Dist3(uncapped.vel, f);
    CHECK(dlUncapped > capLin);
}

// -------------------------------------------------------------------------------------------
// 3b. Caps engage as soon as the correction exceeds them, not only for huge errors: a correction
// of twice the cap comes out at exactly the cap, along the error.
static void TestCapsEngageJustAboveLimit() {
    BodyNow body;
    body.pos = V3{ 0, 0, 0 };
    body.rot = Q4{ 0, 0, 0, 1 };
    body.comLocal = V3{ 0, 0, 0 };
    const double dt = 1.0 / 60.0, capLin = 100.0, capAng = 50.0;
    BodyTarget tg;
    tg.pos = V3{ 2.0 * capLin * dt, 0, 0 };              // one-step need: 200 uu/s along +x
    tg.rot = QAxisAngle(V3{ 0, 0, 1 }, 2.0 * capAng * dt);  // one-step need: 100 deg/s about +z
    const ServoCommand cmd = ServoBody(body, tg, dt, /*gain=*/1.0, capLin, capAng);
    CHECK(Near(cmd.vel.x, capLin, 1e-6));
    CHECK(Near(cmd.vel.y, 0.0, 1e-9));
    CHECK(Near(cmd.vel.z, 0.0, 1e-9));
    CHECK(Near(cmd.angVelDeg.z, capAng, 1e-6));
    CHECK(Near(cmd.angVelDeg.x, 0.0, 1e-9));
    CHECK(Near(cmd.angVelDeg.y, 0.0, 1e-9));
}

// -------------------------------------------------------------------------------------------
// 4. COM offset: rotating a body in place about its own (offset) COM must give ~zero linear
// command, even though the bone ORIGIN moved a long way -- proof the cc/tc terms are applied.
static void TestComOffsetRotationInPlace() {
    BodyNow body;
    body.comLocal = V3{ 10, 0, 0 };
    body.rot = Q4{ 0, 0, 0, 1 };
    body.pos = V3{ 0, 0, 0 };  // world COM = (10, 0, 0)

    BodyTarget tg;
    tg.rot = QAxisAngle(V3{ 0, 0, 1 }, 180);  // same world COM, rotated 180 deg about it
    const V3 tc = QRot(tg.rot, body.comLocal);
    tg.pos = V3{ 10.0 - tc.x, -tc.y, -tc.z };  // tg.pos + tc == (10,0,0)
    tg.vel = V3{ 0, 0, 0 };
    tg.angVelDeg = V3{ 0, 0, 0 };

    CHECK(Dist3(tg.pos, body.pos) > 15.0);  // bone origins are far apart...

    const ServoCommand cmd = ServoBody(body, tg, /*dtS=*/1.0 / 60.0, /*gain=*/1.0, 1e9, 1e9);
    CHECK(Near(cmd.vel.x, 0.0, 1e-6));  // ...but the COM doesn't need to move at all
    CHECK(Near(cmd.vel.y, 0.0, 1e-6));
    CHECK(Near(cmd.vel.z, 0.0, 1e-6));
}

// -------------------------------------------------------------------------------------------
// 5. Quaternion shortest arc: q and -q targets give the same command (double cover).
static void TestQuaternionShortestArc() {
    BodyNow body;
    body.pos = V3{ 3, -2, 1 };
    body.rot = QAxisAngle(V3{ 0.2, 1, 0.1 }, 40);
    body.comLocal = V3{ 1, 2, -1 };

    BodyTarget tg;
    tg.pos = V3{ 12, 7, 83 };
    tg.rot = QAxisAngle(V3{ 1, 0.3, -0.2 }, 75);
    tg.vel = V3{ 7, -18, 18 };
    tg.angVelDeg = V3{ 5, -5, 5 };

    BodyTarget tgNeg = tg;
    tgNeg.rot = Q4{ -tg.rot.x, -tg.rot.y, -tg.rot.z, -tg.rot.w };

    const ServoCommand a = ServoBody(body, tg, 1.0 / 100.0, 0.3, 900, 900);
    const ServoCommand b = ServoBody(body, tgNeg, 1.0 / 100.0, 0.3, 900, 900);
    CHECK(Near(a.vel.x, b.vel.x) && Near(a.vel.y, b.vel.y) && Near(a.vel.z, b.vel.z));
    CHECK(Near(a.angVelDeg.x, b.angVelDeg.x) && Near(a.angVelDeg.y, b.angVelDeg.y) && Near(a.angVelDeg.z, b.angVelDeg.z));
}

// -------------------------------------------------------------------------------------------
// 6. Leg bones (hsm_pose.h IsLegBone) use ServoParams::legGain, and that gain is left alone by
// both the impact-yield window and the start ramp (only the shared caps are narrowed/scaled).
static void TestLegGainSelection() {
    CHECK(IsLegBone(kBonePelvis) == false);
    CHECK(IsLegBone(17) == true);   // thigh_l
    CHECK(IsLegBone(16) == false);  // hand_r

    const ServoParams p;
    ServoState st;
    st.NotifyImpact(1000.0, p);
    const EffectiveParams eff = st.Effective(1050.0, p);  // inside the impact window
    CHECK(Near(eff.legGain, p.legGain));                  // unaffected by impact yield
    CHECK(Near(eff.gain, p.impactGain));                  // body gain narrowed
    CHECK(Near(eff.capLinUu, p.impactCapUu));
    CHECK(Near(eff.capAngDeg, p.impactCapUu));

    ServoState st2;
    st2.StartRamp(2000.0);
    const EffectiveParams eff2 = st2.Effective(2050.0, p);  // inside the ramp (k ~ 1/6)
    CHECK(Near(eff2.legGain, p.legGain));                   // unaffected by the ramp too
    CHECK(Near(eff2.gain, p.gain));
    CHECK(eff2.capLinUu < p.capLinUu);
}

// -------------------------------------------------------------------------------------------
// 7. dt clamping to [minDtS, maxDtS].
static void TestDtClamp() {
    const ServoParams p;
    CHECK(Near(ClampDt(0.0, p), p.minDtS));
    CHECK(Near(ClampDt(0.001, p), p.minDtS));
    CHECK(Near(ClampDt(0.004, p), 0.004));
    CHECK(Near(ClampDt(0.016, p), 0.016));
    CHECK(Near(ClampDt(0.080, p), 0.080));
    CHECK(Near(ClampDt(0.5, p), p.maxDtS));
}

// -------------------------------------------------------------------------------------------
// 8. Start ramp: caps scale 0.3 -> 1.0 linearly over rampMs, gain/legGain untouched, and the
// ramp turns itself off once it has run past rampMs.
static void TestRampState() {
    const ServoParams p;  // rampMs = 300, rampStartFrac = 0.3
    ServoState st;
    st.StartRamp(1000.0);

    EffectiveParams e0 = st.Effective(1000.0, p);  // k = 0
    CHECK(Near(e0.capLinUu, p.capLinUu * 0.3, 1e-9));
    CHECK(Near(e0.capAngDeg, p.capAngDeg * 0.3, 1e-9));
    CHECK(Near(e0.gain, p.gain));

    EffectiveParams eHalf = st.Effective(1150.0, p);  // k = 0.5
    CHECK(Near(eHalf.capLinUu, p.capLinUu * 0.65, 1e-9));

    EffectiveParams eDone = st.Effective(1300.0, p);  // k = 1: ramp ends, full caps
    CHECK(Near(eDone.capLinUu, p.capLinUu, 1e-9));
    CHECK(Near(eDone.capAngDeg, p.capAngDeg, 1e-9));

    EffectiveParams eAfter = st.Effective(5000.0, p);  // long after: still full caps (ramp expired)
    CHECK(Near(eAfter.capLinUu, p.capLinUu, 1e-9));
}

// -------------------------------------------------------------------------------------------
// 9. Impact yield: gain/caps narrowed for exactly impactMs after NotifyImpact, then back to normal.
static void TestImpactYieldState() {
    const ServoParams p;  // impactMs = 200, impactGain = 0.08, impactCapUu = 200
    ServoState st;
    st.NotifyImpact(2000.0, p);

    EffectiveParams during = st.Effective(2100.0, p);
    CHECK(Near(during.gain, p.impactGain));
    CHECK(Near(during.capLinUu, p.impactCapUu));
    CHECK(Near(during.capAngDeg, p.impactCapUu));

    EffectiveParams edge = st.Effective(2199.999, p);
    CHECK(Near(edge.gain, p.impactGain));

    EffectiveParams after = st.Effective(2200.0, p);  // window closed (strict <)
    CHECK(Near(after.gain, p.gain));
    CHECK(Near(after.capLinUu, p.capLinUu));

    // A fresh impact re-opens the window.
    st.NotifyImpact(3000.0, p);
    CHECK(Near(st.Effective(3050.0, p).gain, p.impactGain));
}

// -------------------------------------------------------------------------------------------
// 10. Snap: sustained pelvis error over snapMs triggers WantsSnap, which then stays true until
// ConfirmSnap(); dropping back under the threshold resets the timer.
static void TestSnapState() {
    const ServoParams p;  // snapErrUu = 150, snapMs = 150
    ServoState st;
    CHECK(!st.WantsSnap(0.0, 200.0, p));
    CHECK(!st.WantsSnap(50.0, 200.0, p));
    CHECK(!st.WantsSnap(100.0, 200.0, p));
    CHECK(!st.WantsSnap(149.0, 200.0, p));
    CHECK(st.WantsSnap(150.0, 200.0, p));   // sustained for exactly snapMs
    CHECK(st.WantsSnap(400.0, 200.0, p));   // keeps asking until confirmed
    st.ConfirmSnap();
    CHECK(!st.WantsSnap(400.5, 200.0, p));  // timer restarts after confirm
    CHECK(st.WantsSnap(551.0, 200.0, p));

    // Error below the threshold resets the timer even mid-accumulation; the clock restarts from
    // whichever frame the error NEXT exceeds the threshold, not from the dip itself.
    ServoState st2;
    CHECK(!st2.WantsSnap(0.0, 200.0, p));
    CHECK(!st2.WantsSnap(100.0, 50.0, p));   // dips back under threshold -> resets
    CHECK(!st2.WantsSnap(140.0, 200.0, p));  // exceeds again -> timer restarts at 140
    CHECK(!st2.WantsSnap(289.0, 200.0, p));  // 149 ms since 140
    CHECK(st2.WantsSnap(290.0, 200.0, p));   // 150 ms since 140
}

// -------------------------------------------------------------------------------------------
// 11. Parity with tools/pose-truth/tests/servo_pure.lua: "servo one step" (free body, gain=1,
// effectively uncapped) lands within the Lua test's own tolerances (pos err / rot err < 1e-3).
static void TestParityServoOneStep() {
    BodyNow body;
    body.comLocal = V3{ 7.0, -3.0, 2.5 };
    body.pos = V3{ 100, 50, 80 };
    body.rot = QAxisAngle(V3{ 0.2, 1, 0.1 }, 40);

    BodyTarget tg;
    const Q4 tq = QAxisAngle(V3{ 1, 0.3, -0.2 }, 75);
    tg.rot = tq;
    tg.pos = V3{ 112, 47, 83 };
    tg.vel = V3{ 700, -180, 180 };
    tg.angVelDeg = V3{ 0, 0, 0 };

    const double dt = 1.0 / 100.0;
    const ServoCommand cmd = ServoBody(body, tg, dt, /*gain=*/1.0, /*capLin=*/1e9, /*capAng=*/1e9);
    const BodyNow next = IntegrateBody(body, cmd, dt);
    const double posErr = Dist3(next.pos, tg.pos);
    const double rotErr = QAngleDeg(next.rot, tq);
    CHECK(posErr < 1e-3);
    CHECK(rotErr < 1e-3);
}

// -------------------------------------------------------------------------------------------
// 12. Parity with servo_pure.lua's PURE.advance case: 90 deg/s about z for 0.5 s = 45 deg, and
// the position advances by velocity * time (ExtrapolateTarget).
static void TestParityAdvance() {
    BodyTarget tg;
    tg.pos = V3{ 0, 0, 0 };
    tg.rot = Q4{ 0, 0, 0, 1 };
    tg.vel = V3{ 10, 0, 0 };
    tg.angVelDeg = V3{ 0, 0, 90 };

    const BodyTarget a = ExtrapolateTarget(tg, 0.5);
    CHECK(Near(a.pos.x, 5.0, 1e-9));
    CHECK(Near(a.pos.y, 0.0, 1e-9));
    CHECK(Near(a.pos.z, 0.0, 1e-9));
    const Q4 expect = QAxisAngle(V3{ 0, 0, 1 }, 45);
    CHECK(QAngleDeg(a.rot, expect) < 1e-6);
    // velocity/angular velocity are carried forward unchanged (constant-velocity model).
    CHECK(Near(a.vel.x, 10.0) && Near(a.angVelDeg.z, 90.0));
}

// -------------------------------------------------------------------------------------------
// 13. QRot cross-checked against the textbook quaternion-to-rotation-matrix formula, independent
// of the "sandwich product expanded by hand" form used in ServoBody/QRot itself. Stands in for
// the servo_parity.rs Lua-vs-native fuzz (no Lua runtime is available to this C++ test binary);
// this instead catches a hand-transcription slip in the rotation math across a battery of cases.
static V3 RotateByMatrix(const Q4& q, const V3& v) {
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    const double xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    return V3{
        (1 - 2 * (yy + zz)) * v.x + 2 * (xy - wz) * v.y + 2 * (xz + wy) * v.z,
        2 * (xy + wz) * v.x + (1 - 2 * (xx + zz)) * v.y + 2 * (yz - wx) * v.z,
        2 * (xz - wy) * v.x + 2 * (yz + wx) * v.y + (1 - 2 * (xx + yy)) * v.z,
    };
}
static void TestQRotAgainstMatrixFormula() {
    struct Case { V3 axis; double deg; V3 v; };
    const Case cases[] = {
        { { 0, 0, 1 }, 90, { 1, 0, 0 } },
        { { 0, 1, 0 }, 37, { 3, -2, 5 } },
        { { 1, 1, 1 }, 123, { -4, 8, -1 } },
        { { 0.2, 1, 0.1 }, 40, { 7, -3, 2.5 } },
        { { 1, 0.3, -0.2 }, 179, { 10, 0, 0 } },
        { { -1, -1, 2 }, 61, { 0, 0, 0 } },
    };
    for (const auto& c : cases) {
        const Q4 q = QAxisAngle(c.axis, c.deg);
        const V3 a = QRot(q, c.v);
        const V3 b = RotateByMatrix(q, c.v);
        CHECK(Near(a.x, b.x, 1e-9));
        CHECK(Near(a.y, b.y, 1e-9));
        CHECK(Near(a.z, b.z, 1e-9));
    }
}

int main() {
    TestRestConverges();
    TestConstantVelocityTracking();
    TestCapsEnforced();
    TestCapsEngageJustAboveLimit();
    TestComOffsetRotationInPlace();
    TestQuaternionShortestArc();
    TestLegGainSelection();
    TestDtClamp();
    TestRampState();
    TestImpactYieldState();
    TestSnapState();
    TestParityServoOneStep();
    TestParityAdvance();
    TestQRotAgainstMatrixFormula();
    if (g_fail) { std::printf("test_hsm_servo: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_servo: all passed\n");
    return 0;
}

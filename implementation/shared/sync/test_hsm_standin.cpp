// Unit and pipeline tests for hsm_standin.h. Run via implementation/shared/run_tests.sh test_hsm_standin.
//
// The pipeline tests drive a synthetic fighter (walking on a curve, a fast two-arm swing, a held sword) through the
// whole receive path -- EncodePose, a simulated link, DecodePose, PosePlayer, AimBuilder, ServoFighter -- into free
// rigid bodies (no joints, no gravity, no contacts: what a neutralised stand-in is between hits), and measure how far
// each stand-in body stands from the target sampled for that frame. Free bodies are the optimistic bound for the real
// ragdoll, whose joints and collisions also act on the bodies; the in-game mirror mode measures the real thing.
#include "sync/hsm_standin.h"
#include "sync/hsm_track.h"
#include <cmath>
#include <cstdio>
#include <memory>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;
using detail::QMul;
using detail::QRot;

static constexpr double kPiD = 3.14159265358979323846;

static double Dist(const V3& a, const V3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
static bool Near(const V3& a, const V3& b, double eps) { return Dist(a, b) <= eps; }
static double QAngle(const Q4& a, const Q4& b) {   // degrees between two rotations; atan2 form, exactly 0 for equal inputs
    const Q4 d = QMul(a, detail::QConj(b));
    const double s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return 2.0 * std::atan2(s, std::fabs(d.w)) * 180.0 / kPiD;
}
static Q4 QAxis(V3 axis, double ang) {
    const double l = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    const double s = std::sin(ang * 0.5) / l;
    return Q4{ axis.x * s, axis.y * s, axis.z * s, std::cos(ang * 0.5) };
}
static bool SameCmd(const ServoCommand& a, const ServoCommand& b) { return Near(a.vel, b.vel, 1e-9) && Near(a.angVelDeg, b.angVelDeg, 1e-9); }

// ---- AimBody -------------------------------------------------------------------------------------------------

// 1. Zero acceleration: the aim is ExtrapolateTarget's, feed-forward unchanged. With an acceleration: position gets
// a*h^2/2, the feed-forward is the chord v + a*h/2, rotation and angular velocity are unaffected.
static void TestAimBody() {
    const BodyTarget tg{ V3{ 10, 20, 30 }, QAxis(V3{ 0, 0, 1 }, 0.3), V3{ 100, -50, 20 }, V3{ 0, 0, 90 } };
    const double h = 0.02;
    const BodyTarget ex = ExtrapolateTarget(tg, h);
    const BodyTarget a0 = AimBody(tg, V3{}, h);
    CHECK(Near(a0.pos, ex.pos, 1e-12) && QAngle(a0.rot, ex.rot) < 1e-9);
    CHECK(Near(a0.vel, tg.vel, 1e-12) && Near(a0.angVelDeg, tg.angVelDeg, 1e-12));
    CHECK(Near(a0.pos, V3{ 12, 19, 30.4 }, 1e-12));

    const V3 acc{ 1000, 0, -2000 };
    const BodyTarget a1 = AimBody(tg, acc, h);
    CHECK(Near(a1.pos, V3{ 12 + 0.2, 19, 30.4 - 0.4 }, 1e-12));
    CHECK(Near(a1.vel, V3{ 110, -50, 0 }, 1e-12));
    CHECK(QAngle(a1.rot, ex.rot) < 1e-9 && Near(a1.angVelDeg, tg.angVelDeg, 1e-12));
    // A body moving with exactly the chord velocity for h lands on the aim (PURE.aim's contract).
    CHECK(Near(V3{ tg.pos.x + a1.vel.x * h, tg.pos.y + a1.vel.y * h, tg.pos.z + a1.vel.z * h }, a1.pos, 1e-12));
}

// ---- AimBuilder ----------------------------------------------------------------------------------------------

static PoseFrame FrameWithVel(double vx, bool weapon = true) {
    PoseFrame f;
    for (BoneSample& b : f.bones) b.vel = V3{ vx, 0, 0 };
    f.weapons[0].present = weapon;
    f.weapons[0].vel = V3{ 2 * vx, 0, 0 };
    return f;
}

// 2. Gap rules, main.lua:4017-4029: first sample -> 0; gap in [2, 80] -> dv/dt; gap < 2 keeps the estimate; gap > 80
// clears it; both ends of the window are inclusive.
static void TestAccGapRules() {
    AimBuilder ab;
    ab.Observe(FrameWithVel(0), 1000, false, false);
    CHECK(Near(ab.Acc(0), V3{}, 0));
    ab.Observe(FrameWithVel(100), 1010, false, false);           // +100 uu/s in 10 ms
    CHECK(Near(ab.Acc(0), V3{ 10000, 0, 0 }, 1e-9));
    CHECK(Near(ab.Acc(kPoseBones), V3{ 20000, 0, 0 }, 1e-9));    // weapon slot
    ab.Observe(FrameWithVel(500), 1011, false, false);           // 1 ms later: estimate kept...
    CHECK(Near(ab.Acc(0), V3{ 10000, 0, 0 }, 1e-9));
    ab.Observe(FrameWithVel(520), 1021, false, false);           // ...but the 1 ms sample became the reference
    CHECK(Near(ab.Acc(0), V3{ 2000, 0, 0 }, 1e-9));
    ab.Observe(FrameWithVel(520), 1023, false, false);           // exactly 2 ms: used
    CHECK(Near(ab.Acc(0), V3{}, 1e-9));
    ab.Observe(FrameWithVel(600), 1103, false, false);           // exactly 80 ms: used
    CHECK(Near(ab.Acc(0), V3{ 1000, 0, 0 }, 1e-9));
    ab.Observe(FrameWithVel(700), 1184, false, false);           // 81 ms: cleared
    CHECK(Near(ab.Acc(0), V3{}, 0));
    ab.Observe(FrameWithVel(710), 1194, false, false);           // and estimated again from there
    CHECK(Near(ab.Acc(0), V3{ 1000, 0, 0 }, 1e-9));
    CHECK(Near(ab.Acc(-1), V3{}, 0) && Near(ab.Acc(kAimSlots), V3{}, 0));
}

// 3. The estimate is clamped to 40000 uu/s^2 keeping its direction (PURE.clamp_acc3).
static void TestAccClamp() {
    AimBuilder ab;
    PoseFrame a = FrameWithVel(0), b = FrameWithVel(0);
    b.bones[5].vel = V3{ 300, 400, 0 };                          // 500 uu/s in 10 ms = 50000 uu/s^2
    ab.Observe(a, 0, false, false);
    ab.Observe(b, 10, false, false);
    CHECK(Near(ab.Acc(5), V3{ 24000, 32000, 0 }, 1e-6));
    b.bones[6].vel = V3{ 0, 0, -399.99 };                        // just under the clamp: untouched
    ab.Observe(a, 20, false, false);
    ab.Observe(b, 30, false, false);
    CHECK(Near(ab.Acc(6), V3{ 0, 0, -39999 }, 1e-6));
}

// 4. A cut forgets everything: the next sample has no reference. Frozen (stale) samples read zero and leave the
// estimate and its reference alone; fresh data resumes from the last fresh sample.
static void TestAccCutAndFrozen() {
    AimBuilder ab;
    ab.Observe(FrameWithVel(0), 0, false, false);
    ab.Observe(FrameWithVel(100), 10, false, false);
    ab.Observe(FrameWithVel(300), 20, true, false);              // cut: no estimate from across it
    CHECK(Near(ab.Acc(0), V3{}, 0));
    ab.Observe(FrameWithVel(400), 30, false, false);
    CHECK(Near(ab.Acc(0), V3{ 10000, 0, 0 }, 1e-9));
    ab.Observe(FrameWithVel(9999), 40, false, true);             // frozen: reads zero, not folded in
    CHECK(Near(ab.Acc(0), V3{}, 0));
    ab.Observe(FrameWithVel(500), 50, false, false);             // 20 ms after the last fresh sample (400 @ 30)
    CHECK(Near(ab.Acc(0), V3{ 5000, 0, 0 }, 1e-9));
    ab.Observe(FrameWithVel(500), 60, false, true);
    ab.Observe(FrameWithVel(600), 150, false, false);            // 100 ms after the last fresh one: cleared
    CHECK(Near(ab.Acc(0), V3{}, 0));
    ab.Reset();
    ab.Observe(FrameWithVel(700), 160, false, false);            // Reset: no reference either
    CHECK(Near(ab.Acc(0), V3{}, 0));
}

// 5. A weapon that is missing from either of the two samples gets no estimate.
static void TestAccWeaponPresence() {
    AimBuilder ab;
    ab.Observe(FrameWithVel(0, false), 0, false, false);
    ab.Observe(FrameWithVel(100, true), 10, false, false);
    CHECK(Near(ab.Acc(kPoseBones), V3{}, 0) && Near(ab.Acc(0), V3{ 10000, 0, 0 }, 1e-9));
    ab.Observe(FrameWithVel(200, false), 20, false, false);
    CHECK(Near(ab.Acc(kPoseBones), V3{}, 0));
    ab.Observe(FrameWithVel(300, true), 30, false, false);
    CHECK(Near(ab.Acc(kPoseBones), V3{}, 0));
    ab.Observe(FrameWithVel(400, true), 40, false, false);
    CHECK(Near(ab.Acc(kPoseBones), V3{ 20000, 0, 0 }, 1e-9));
}

// ---- ServoFighter --------------------------------------------------------------------------------------------

static FighterNow NowAtOffset(const PoseFrame& tg, const V3& off) {
    FighterNow n;
    for (int i = 0; i < kPoseBones; ++i) {
        n.bones[i] = BodyNow{ V3{ tg.bones[i].pos.x + off.x, tg.bones[i].pos.y + off.y, tg.bones[i].pos.z + off.z }, tg.bones[i].rot, V3{ 3, 1, -2 } };
        n.valid[i] = true;
    }
    for (int w = 0; w < kPoseWeapons; ++w) {
        n.weapons[w] = BodyNow{ V3{ tg.weapons[w].pos.x + off.x, tg.weapons[w].pos.y, tg.weapons[w].pos.z }, tg.weapons[w].rot, V3{ 40, 0, 0 } };
        n.weaponValid[w] = true;
    }
    return n;
}

static PoseFrame MovingTarget() {
    PoseFrame f;
    for (int i = 0; i < kPoseBones; ++i) {
        f.bones[i].pos = V3{ 10.0 * i, 5, 90 };
        f.bones[i].rot = QAxis(V3{ 0, 0, 1 }, 0.1 * i);
        f.bones[i].vel = V3{ 200, 0, 0 };
        f.bones[i].angVelDeg = V3{ 0, 0, 45 };
    }
    f.weapons[0] = WeaponSample{ true, 0, 7, V3{ 50, 0, 100 }, Q4{}, V3{ 300, 0, 0 }, V3{ 0, 90, 0 }, V3{}, V3{} };
    f.weapons[1] = WeaponSample{ false, 1, 0, V3{ -50, 0, 100 }, Q4{}, V3{}, V3{}, V3{}, V3{} };
    return f;
}

// 6. Which bodies are driven, and with what: spine_01 (no body) and unread bones never; legs with legGain, the rest
// with gain; each command equals ServoBody on that body's aim; weapons only when present in the target and read.
static void TestServoFighterSelection() {
    const PoseFrame tg = MovingTarget();
    FighterNow now = NowAtOffset(tg, V3{ 20, 0, 0 });
    now.valid[3] = false;
    now.weaponValid[1] = true;
    AimBuilder ab;
    ab.Observe(tg, 100, false, false);
    const EffectiveParams p{ 0.3, 0.5, 900, 900 };
    const double dt = 1.0 / 60.0;
    const FighterCommand c = ServoFighter(now, tg, ab, false, dt, p);
    for (int i = 0; i < kPoseBones; ++i) {
        const bool want = kPoseHasBody[i] && i != 3;
        CHECK(c.drive[i] == want);
        if (!want) continue;
        const BodyTarget aim = AimBody(TargetOf(tg.bones[i]), V3{}, dt);
        CHECK(SameCmd(c.bones[i], ServoBody(now.bones[i], aim, dt, IsLegBone(i) ? 0.5 : 0.3, 900, 900)));
    }
    CHECK(!c.drive[1]);
    // the leg/arm gains genuinely differ here (the body is 20 uu off)
    CHECK(!SameCmd(ServoBody(now.bones[17], AimBody(TargetOf(tg.bones[17]), V3{}, dt), dt, 0.3, 900, 900), c.bones[17]));
    CHECK(c.weaponDrive[0] && !c.weaponDrive[1]);
    CHECK(SameCmd(c.weapons[0], ServoBody(now.weapons[0], AimBody(TargetOf(tg.weapons[0]), V3{}, dt), dt, 0.3, 900, 900)));
    now.weaponValid[0] = false;
    CHECK(!ServoFighter(now, tg, ab, false, dt, p).weaponDrive[0]);
}

// 7. The acceleration estimate reaches the command (chord feed-forward), per slot.
static void TestServoFighterUsesAcc() {
    PoseFrame a = MovingTarget(), b = MovingTarget();
    for (BoneSample& bs : b.bones) bs.vel.x += 100;              // +100 uu/s over 10 ms: 10000 uu/s^2
    b.weapons[0].vel.x += 50;
    AimBuilder ab;
    ab.Observe(a, 0, false, false);
    ab.Observe(b, 10, false, false);
    const FighterNow now = NowAtOffset(b, V3{});
    const EffectiveParams p{ 0.3, 0.5, 900, 900 };
    const double dt = 0.02;
    const FighterCommand c = ServoFighter(now, b, ab, false, dt, p);
    const BodyTarget aim = AimBody(TargetOf(b.bones[0]), V3{ 10000, 0, 0 }, dt);
    CHECK(SameCmd(c.bones[0], ServoBody(now.bones[0], aim, dt, 0.3, 900, 900)));
    CHECK(SameCmd(c.weapons[0], ServoBody(now.weapons[0], AimBody(TargetOf(b.weapons[0]), V3{ 5000, 0, 0 }, dt), dt, 0.3, 900, 900)));
    AimBuilder none;
    CHECK(!SameCmd(c.bones[0], ServoFighter(now, b, none, false, dt, p).bones[0]));
}

// 8. Frozen: every body aims at the held pose standing still -- no advance, no feed-forward -- so a stand-in that is
// on the held pose is commanded to stay exactly there, whatever velocities the stale sample carries.
static void TestServoFighterFrozen() {
    const PoseFrame tg = MovingTarget();
    const FighterNow now = NowAtOffset(tg, V3{});
    AimBuilder ab;
    ab.Observe(tg, 0, false, false);
    const FighterCommand c = ServoFighter(now, tg, ab, true, 1.0 / 60.0, EffectiveParams{ 0.3, 0.5, 900, 900 });
    for (int i = 0; i < kPoseBones; ++i) {
        if (!c.drive[i]) continue;
        CHECK(Near(c.bones[i].vel, V3{}, 1e-9) && Near(c.bones[i].angVelDeg, V3{}, 1e-9));
    }
    CHECK(c.weaponDrive[0] && Near(c.weapons[0].vel, V3{}, 1e-9) && Near(c.weapons[0].angVelDeg, V3{}, 1e-9));
    const BodyTarget held = AimOrHold(TargetOf(tg.bones[4]), V3{ 1, 2, 3 }, 0.05, true);
    CHECK(Near(held.pos, tg.bones[4].pos, 0) && QAngle(held.rot, tg.bones[4].rot) == 0.0);
}

// 9. OffsetPose moves positions (bones, weapon roots, blades) and nothing else; BoneError reads distance or -1.
static void TestOffsetPoseAndError() {
    PoseFrame f = MovingTarget();
    f.weapons[0].base = V3{ 1, 2, 3 };
    f.weapons[0].tip = V3{ 4, 5, 6 };
    const PoseFrame o = f;
    OffsetPose(f, V3{ 0, 250, -1 });
    for (int i = 0; i < kPoseBones; ++i) {
        CHECK(Near(f.bones[i].pos, V3{ o.bones[i].pos.x, o.bones[i].pos.y + 250, o.bones[i].pos.z - 1 }, 1e-12));
        CHECK(QAngle(f.bones[i].rot, o.bones[i].rot) == 0.0 && Near(f.bones[i].vel, o.bones[i].vel, 0));
    }
    CHECK(Near(f.weapons[0].pos, V3{ 50, 250, 99 }, 1e-12) && Near(f.weapons[0].base, V3{ 1, 252, 2 }, 1e-12) &&
          Near(f.weapons[0].tip, V3{ 4, 255, 5 }, 1e-12));
    CHECK(Near(f.weapons[1].pos, V3{ -50, 250, 99 }, 1e-12) && Near(f.weapons[0].vel, o.weapons[0].vel, 0));
    FighterNow n = NowAtOffset(o, V3{ 3, 0, 0 });
    n.bones[2].pos.y += 4;
    CHECK(std::fabs(BoneError(n, o, 0) - 3.0) < 1e-12 && std::fabs(BoneError(n, o, 2) - 5.0) < 1e-12);
    n.valid[0] = false;
    CHECK(BoneError(n, o, 0) == -1.0 && BoneError(n, o, -1) == -1.0 && BoneError(n, o, kPoseBones) == -1.0);
}

// ---- synthetic owner -----------------------------------------------------------------------------------------

struct OwnerPose {
    std::array<V3, kPoseBones> pos{};
    std::array<Q4, kPoseBones> rot{};
    V3 wpos, wbase, wtip;
    Q4 wrot;
};

static constexpr V3 kWeaponRootInHand{ 4, 0, -3 };
static constexpr V3 kBladeBase{ 12, 0, 0 }, kBladeTip{ 112, 0, 0 };   // in the weapon root frame

// FK through the stream's skeleton (kPoseRefT): walking a 714 uu circle at 250 uu/s, a 1.6 Hz swing of the right
// arm (hand ~1000 uu/s at peak, sword tip ~2500 uu/s), the left arm and the legs swinging in counter-phase.
static OwnerPose OwnerAt(double tS) {
    OwnerPose o;
    const double yaw = 0.35 * tS, w = 2 * kPiD * 1.6 * tS, gait = 2 * kPiD * 1.5 * tS;
    o.pos[0] = V3{ 714.0 * std::sin(yaw), 714.0 * (1 - std::cos(yaw)), 95 + 2 * std::sin(2 * gait) };
    o.rot[0] = QMul(QAxis(V3{ 0, 0, 1 }, yaw), QAxis(V3{ 1, 0, 0 }, 0.05 * std::sin(gait)));
    std::array<Q4, kPoseBones> local{};
    for (int i = 2; i <= 5; ++i) local[i] = QAxis(V3{ 0, 1, 0 }, 0.04 * std::sin(gait + i));
    local[6] = local[7] = QAxis(V3{ 0, 0, 1 }, 0.1 * std::sin(0.5 * w));
    local[14] = QAxis(V3{ 0, 1, 0.3 }, 1.4 * std::sin(w));
    local[15] = QAxis(V3{ 0, 0, 1 }, 0.7 + 0.5 * std::sin(w + 0.8));
    local[16] = QAxis(V3{ 1, 0, 0 }, 0.6 * std::sin(w));
    local[10] = QAxis(V3{ 0, 1, -0.3 }, 0.6 * std::sin(w + kPiD));
    local[11] = QAxis(V3{ 0, 0, 1 }, 0.5 + 0.3 * std::sin(w));
    local[12] = QAxis(V3{ 1, 0, 0 }, 0.2 * std::sin(w));
    local[17] = QAxis(V3{ 1, 0, 0 }, 0.45 * std::sin(gait));
    local[18] = QAxis(V3{ 1, 0, 0 }, 0.35 + 0.35 * std::sin(gait - 1));
    local[19] = QAxis(V3{ 1, 0, 0 }, 0.2 * std::sin(gait - 2));
    local[20] = QAxis(V3{ 1, 0, 0 }, -0.45 * std::sin(gait));
    local[21] = QAxis(V3{ 1, 0, 0 }, 0.35 - 0.35 * std::sin(gait - 1));
    local[22] = QAxis(V3{ 1, 0, 0 }, -0.2 * std::sin(gait - 2));
    for (int i = 1; i < kPoseBones; ++i) {
        const int p = kPoseParent[i];
        o.rot[i] = QMul(o.rot[p], local[i]);
        const V3 r = QRot(o.rot[p], kPoseRefT[i]);
        o.pos[i] = V3{ o.pos[p].x + r.x, o.pos[p].y + r.y, o.pos[p].z + r.z };
    }
    o.wrot = QMul(o.rot[kBoneHandR], QAxis(V3{ 0, 0, 1 }, 0.3));
    const V3 wr = QRot(o.rot[kBoneHandR], kWeaponRootInHand);
    o.wpos = V3{ o.pos[kBoneHandR].x + wr.x, o.pos[kBoneHandR].y + wr.y, o.pos[kBoneHandR].z + wr.z };
    const V3 b = QRot(o.wrot, kBladeBase), t = QRot(o.wrot, kBladeTip);
    o.wbase = V3{ o.wpos.x + b.x, o.wpos.y + b.y, o.wpos.z + b.z };
    o.wtip = V3{ o.wpos.x + t.x, o.wpos.y + t.y, o.wpos.z + t.z };
    return o;
}

static V3 DiffPos(const V3& a, const V3& b, double h2) { return V3{ (a.x - b.x) / h2, (a.y - b.y) / h2, (a.z - b.z) / h2 }; }
static V3 DiffRotDeg(const Q4& qa, const Q4& qb, double h2) {   // world angular velocity taking qb to qa over h2
    Q4 d = QMul(qa, detail::QConj(qb));
    if (d.w < 0) d = Q4{ -d.x, -d.y, -d.z, -d.w };
    const double s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (s < 1e-15) return V3{};
    const double k = 2.0 * std::atan2(s, d.w) / s / h2 * 180.0 / kPiD;
    return V3{ d.x * k, d.y * k, d.z * k };
}

// The owner's frame at tS: exact pose, velocities by central difference (error ~1e-3 uu/s), stamped with its
// clock in whole ms as the game does.
static PoseFrame OwnerFrame(double tS, double clockOffsetMs) {
    const double h = 1e-4;
    const OwnerPose o = OwnerAt(tS), a = OwnerAt(tS + h), b = OwnerAt(tS - h);
    PoseFrame f;
    f.tsMs = static_cast<uint32_t>(std::llround(tS * 1000.0 + clockOffsetMs));
    f.stepUs = 16667;
    for (int i = 0; i < kPoseBones; ++i) {
        f.bones[i] = BoneSample{ o.pos[i], o.rot[i], DiffPos(a.pos[i], b.pos[i], 2 * h), DiffRotDeg(a.rot[i], b.rot[i], 2 * h) };
    }
    f.weapons[0] = WeaponSample{ true, 0, WeaponClassHash("ModularWeaponBP_C"), o.wpos, o.wrot, DiffPos(a.wpos, b.wpos, 2 * h),
                                 DiffRotDeg(a.wrot, b.wrot, 2 * h), o.wbase, o.wtip };
    return f;
}

// ---- free-body stand-in ----------------------------------------------------------------------------------------

static BodyNow Integrate(const BodyNow& cur, const ServoCommand& cmd, double dtS) {
    const V3 c0 = QRot(cur.rot, cur.comLocal);
    const V3 com{ cur.pos.x + c0.x + cmd.vel.x * dtS, cur.pos.y + c0.y + cmd.vel.y * dtS, cur.pos.z + c0.z + cmd.vel.z * dtS };
    const double wr = kPiD / 180.0;
    const V3 ax{ cmd.angVelDeg.x * wr * dtS, cmd.angVelDeg.y * wr * dtS, cmd.angVelDeg.z * wr * dtS };
    const double a = std::sqrt(ax.x * ax.x + ax.y * ax.y + ax.z * ax.z);
    Q4 rot = cur.rot;
    if (a > 1e-12) rot = QMul(QAxis(ax, a), cur.rot);
    const V3 c1 = QRot(rot, cur.comLocal);
    return BodyNow{ V3{ com.x - c1.x, com.y - c1.y, com.z - c1.z }, rot, cur.comLocal };
}

// Body centres of mass: halfway to the bone's first child (or a few uu along X for end bones); the sword's well
// out along the blade, so rotation matters to where its COM goes.
static V3 ComLocal(int bone) {
    for (int j = bone + 1; j < kPoseBones; ++j) {
        if (kPoseParent[j] == bone) return V3{ kPoseRefT[j].x * 0.5, kPoseRefT[j].y * 0.5, kPoseRefT[j].z * 0.5 };
    }
    return V3{ 6, 0, 0 };
}
static constexpr V3 kWeaponCom{ 45, 0, 0 };

static void PlaceOn(FighterNow& n, const PoseFrame& tg) {
    for (int i = 0; i < kPoseBones; ++i) { n.bones[i] = BodyNow{ tg.bones[i].pos, tg.bones[i].rot, ComLocal(i) }; n.valid[i] = kPoseHasBody[i]; }
    n.weapons[0] = BodyNow{ tg.weapons[0].pos, tg.weapons[0].rot, kWeaponCom };
    n.weaponValid[0] = tg.weapons[0].present;
}

struct PipeResult {
    ErrorStats pelvis, hands, tip, buffer;   // stand-in vs target (pelvis, both hands, sword tip); target vs owner truth (hands)
    uint64_t frames = 0, extrapolated = 0, cuts = 0, snaps = 0, stale = 0;
    double maxCmdUu = 0, delayMs = 0;
    LinkStats link{};
    PosePipeStats pipe{};
};

struct PipeConfig {
    double fps = 60, frameJitter = 0.15, seconds = 30, warmupS = 2;
    LinkConfig link{};
    bool useAcc = true;
    uint64_t frameSeed = 77;
};

// The receive side as the game runs it (mirror mode): per frame, StandInDriver::Step on the stand-in's bodies, the
// placement it asks for applied as a rigid move, then its commands integrated over the coming step. The stand-in
// spawns in a pose of its own (the owner's pose at t = 0) 300 uu to the side, as a spawned stand-in does. Errors
// are measured every counted frame on the bodies as they stand when the frame starts, before any placement.
static PipeResult RunPipe(const PipeConfig& cfg) {
    PipeResult r;
    const double clockOffsetMs = 123456.0;   // the owner's clock is not ours
    auto pipe = std::make_unique<PosePipe>(cfg.link);
    StandInDriver driver(ServoParams{}, cfg.useAcc);
    FighterNow now{};
    PlaceOn(now, OwnerFrame(0.0, clockOffsetMs));
    ShiftFighterNow(now, V3{ 0, 300, 0 });
    SplitMix64 rng(cfg.frameSeed);
    const double base = 1000.0 / cfg.fps;
    double tMs = 1000.0, nextSendMs = tMs;   // local clock; the owner's motion is evaluated on it (mirror mode)
    PoseFrame target;
    while (tMs < 1000.0 + cfg.seconds * 1000.0) {
        // owner side, this frame: stream at most 60/s
        if (tMs + 1e-9 >= nextSendMs) {
            pipe->Send(OwnerFrame(tMs / 1000.0, clockOffsetMs), tMs);
            nextSendMs += 1000.0 / 60.0;
            if (nextSendMs < tMs) nextSendMs = tMs;
        }
        // receive side, same frame, before the physics step
        pipe->Pump(tMs);
        const double nextDtMs = base * (1.0 + cfg.frameJitter * (2.0 * rng.Unit() - 1.0));
        const DriverStep s = driver.Step(pipe->Player(), tMs, now, nextDtMs / 1000.0, false, target);
        if (s.have) {
            const bool counting = tMs >= 1000.0 + cfg.warmupS * 1000.0;
            if (s.info.cut) ++r.cuts;
            if (counting) {
                ++r.frames;
                if (s.info.extrapolated) ++r.extrapolated;
                if (s.info.stale) ++r.stale;
                if (s.snap) ++r.snaps;
                r.pelvis.Add(s.pelvisErrUu);
                r.hands.Add(BoneError(now, target, kBoneHandL));
                r.hands.Add(BoneError(now, target, kBoneHandR));
                const V3 t = QRot(now.weapons[0].rot, kBladeTip);
                r.tip.Add(Dist(V3{ now.weapons[0].pos.x + t.x, now.weapons[0].pos.y + t.y, now.weapons[0].pos.z + t.z }, target.weapons[0].tip));
                const OwnerPose truth = OwnerAt((static_cast<double>(s.info.playTsMs) - clockOffsetMs) / 1000.0);
                r.buffer.Add(Dist(truth.pos[kBoneHandR], target.bones[kBoneHandR].pos));
            }
            if (s.place) ShiftFighterNow(now, s.shift);
            for (int i = 0; i < kPoseBones; ++i) {
                if (!s.cmd.drive[i]) continue;
                now.bones[i] = Integrate(now.bones[i], s.cmd.bones[i], nextDtMs / 1000.0);
                r.maxCmdUu = std::max(r.maxCmdUu, std::sqrt(s.cmd.bones[i].vel.x * s.cmd.bones[i].vel.x + s.cmd.bones[i].vel.y * s.cmd.bones[i].vel.y +
                                                            s.cmd.bones[i].vel.z * s.cmd.bones[i].vel.z));
            }
            if (s.cmd.weaponDrive[0]) now.weapons[0] = Integrate(now.weapons[0], s.cmd.weapons[0], nextDtMs / 1000.0);
            // spine_01 has no body: it follows its neighbours in the ragdoll; here it is simply read off the target
            now.bones[1] = BodyNow{ target.bones[1].pos, target.bones[1].rot, ComLocal(1) };
            r.delayMs = s.info.delayMs;
        }
        tMs += nextDtMs;
    }
    r.link = pipe->Link();
    r.pipe = pipe->Stats();
    return r;
}

static void Report(const char* name, const PipeResult& r) {
    std::printf("  %-34s pelvis avg %5.2f p99 %5.2f max %6.2f | hands avg %5.2f p99 %5.2f max %6.2f | tip avg %5.2f p99 %6.2f max %6.2f\n"
                "  %-34s buffer-vs-truth hand avg %5.2f p99 %5.2f | delay %5.1f ms extrap %4.1f%% stale %llu cuts %llu snaps %llu lost %llu maxcmd %.0f\n",
                name, r.pelvis.Mean(), r.pelvis.Percentile(0.99), r.pelvis.Max(), r.hands.Mean(), r.hands.Percentile(0.99), r.hands.Max(),
                r.tip.Mean(), r.tip.Percentile(0.99), r.tip.Max(), "", r.buffer.Mean(), r.buffer.Percentile(0.99), r.delayMs,
                r.frames ? 100.0 * double(r.extrapolated) / double(r.frames) : 0.0, (unsigned long long)r.stale, (unsigned long long)r.cuts,
                (unsigned long long)r.snaps, (unsigned long long)r.link.lost, r.maxCmdUu);
}

// Every error number is real and finite; nothing blew up.
static void CheckSane(const PipeResult& r) {
    CHECK(r.frames > 100);
    CHECK(r.pelvis.Bad() == 0 && r.hands.Bad() == 0 && r.tip.Bad() == 0 && r.buffer.Bad() == 0);
    CHECK(r.pipe.decodeFailed == 0);
    CHECK(r.maxCmdUu < 5000.0);
}

// 10. The pose pipe hands decoded frames to the player: an ideal link delivers every frame once.
static void TestPosePipeIdeal() {
    auto pipe = std::make_unique<PosePipe>(LinkConfig{});
    for (int i = 0; i < 10; ++i) {
        const PoseFrame f = OwnerFrame(i / 60.0, 5000.0);
        CHECK(pipe->Send(f, 1000.0 + i * 16.0));
        pipe->Pump(1000.0 + i * 16.0);
    }
    CHECK(pipe->Stats().encoded == 10 && pipe->Stats().pushed == 10 && pipe->Stats().rejected == 0 && pipe->Stats().decodeFailed == 0);
    CHECK(pipe->Link().delivered == 10 && pipe->Player().size() == 10);
    PoseFrame got;
    CHECK(pipe->Player().Sample(1200, got));
}

// 11. Plan acceptance (Phase 1) on free bodies: mirror at 80 +- 20 ms with 2 % loss, walking + swinging, at 60 and
// 144 fps: pelvis and hand error against the buffered target average under 5 uu, p99 under 25 uu; no cuts or snaps.
static void TestPipelineAcceptance() {
    PipeConfig c60;
    c60.link = LinkConfig{ 80.0, 20.0, 2.0, 9 };
    const PipeResult a = RunPipe(c60);
    PipeConfig c144 = c60;
    c144.fps = 144;
    const PipeResult b = RunPipe(c144);
    Report("60 fps, 80+-20 ms, 2% loss", a);
    Report("144 fps, 80+-20 ms, 2% loss", b);
    for (const PipeResult* r : { &a, &b }) {
        CheckSane(*r);
        CHECK(r->pelvis.Mean() < 5.0 && r->pelvis.Percentile(0.99) < 25.0);
        CHECK(r->hands.Mean() < 5.0 && r->hands.Percentile(0.99) < 25.0);
        CHECK(r->snaps == 0 && r->cuts == 1);   // only the first sample
        CHECK(r->link.lost > 0);
    }
}

// 12. The other plan link settings (ideal; 150 +- 30 ms with 5 % loss): sane, reported. What a better link buys is a
// shorter playback delay and a buffered target closer to the owner's real motion (the stand-in follows whatever
// target it is given equally well either way).
static void TestPipelineOtherLinks() {
    PipeConfig ideal;
    const PipeResult a = RunPipe(ideal);
    PipeConfig slow;
    slow.link = LinkConfig{ 150.0, 30.0, 5.0, 4 };
    const PipeResult b = RunPipe(slow);
    Report("60 fps, ideal link", a);
    Report("60 fps, 150+-30 ms, 5% loss", b);
    CheckSane(a);
    CheckSane(b);
    CHECK(a.link.lost == 0 && a.snaps == 0 && b.snaps == 0);
    CHECK(a.hands.Mean() < 5.0 && a.hands.Percentile(0.99) < 25.0);
    PipeConfig ref;
    ref.link = LinkConfig{ 80.0, 20.0, 2.0, 9 };
    const PipeResult m = RunPipe(ref);
    CHECK(a.delayMs < m.delayMs && m.delayMs < b.delayMs);
    CHECK(a.buffer.Mean() < m.buffer.Mean());
}

// 13. Following the curve: on the same run, the acceleration-aware aim tracks the swinging hands and the sword tip
// better than a constant-velocity aim (HSMP's reason for it).
static void TestAccAimBeatsConstantVelocity() {
    PipeConfig withAcc;
    withAcc.link = LinkConfig{ 80.0, 20.0, 2.0, 9 };
    PipeConfig noAcc = withAcc;
    noAcc.useAcc = false;
    const PipeResult a = RunPipe(withAcc), b = RunPipe(noAcc);
    Report("acc-aware aim", a);
    Report("constant-velocity aim", b);
    CHECK(a.hands.Mean() < b.hands.Mean());
    CHECK(a.tip.Mean() < b.tip.Mean());
}

// 14. Same inputs, same numbers (the link and frame-time draws are seeded).
static void TestPipelineDeterministic() {
    PipeConfig c;
    c.seconds = 5;
    c.link = LinkConfig{ 80.0, 20.0, 2.0, 3 };
    const PipeResult a = RunPipe(c), b = RunPipe(c);
    CHECK(a.hands.Mean() == b.hands.Mean() && a.pelvis.Max() == b.pelvis.Max() && a.link.lost == b.link.lost);
}

// ---- StandInDriver ---------------------------------------------------------------------------------------------

static PoseFrame StaticTarget(const V3& at) {   // MovingTarget's posture standing still at `at` (pelvis)
    PoseFrame f = MovingTarget();
    for (BoneSample& b : f.bones) { b.vel = V3{}; b.angVelDeg = V3{}; }
    f.weapons[0].vel = V3{};
    f.weapons[0].angVelDeg = V3{};
    const V3 d{ at.x - f.bones[kBonePelvis].pos.x, at.y - f.bones[kBonePelvis].pos.y, at.z - f.bones[kBonePelvis].pos.z };
    OffsetPose(f, d);
    return f;
}

// Streams `f` into the player every 16 ms (sender clock = local clock + 5000) up to nowMs.
struct Feeder {
    PosePlayer player;
    double nextMs = 0.0;
    void To(double nowMs, const PoseFrame& f) {
        for (; nextMs <= nowMs; nextMs += 16.0) {
            PoseFrame g = f;
            g.tsMs = static_cast<uint32_t>(std::llround(nextMs + 5000.0));
            player.Push(g, static_cast<int64_t>(std::llround(nextMs)));
        }
    }
};

static double MaxCmd(const FighterCommand& c) {
    double m = 0.0;
    for (int i = 0; i < kPoseBones; ++i) if (c.drive[i]) m = std::max(m, std::sqrt(c.bones[i].vel.x * c.bones[i].vel.x + c.bones[i].vel.y * c.bones[i].vel.y + c.bones[i].vel.z * c.bones[i].vel.z));
    return m;
}
static bool SameFighterCmd(const FighterCommand& a, const FighterCommand& b) {
    for (int i = 0; i < kPoseBones; ++i) if (a.drive[i] != b.drive[i] || (a.drive[i] && !SameCmd(a.bones[i], b.bones[i]))) return false;
    for (int w = 0; w < kPoseWeapons; ++w) if (a.weaponDrive[w] != b.weaponDrive[w] || (a.weaponDrive[w] && !SameCmd(a.weapons[w], b.weapons[w]))) return false;
    return true;
}

// 15. No target yet: nothing sampled, nothing placed or driven.
static void TestDriverNoSample() {
    StandInDriver d;
    PosePlayer empty;
    PoseFrame tg;
    const DriverStep s = d.Step(empty, 1000, NowAtOffset(StaticTarget(V3{}), V3{}), 1.0 / 60.0, false, tg);
    CHECK(!s.have && !s.place && !s.snap && s.pelvisErrUu == -1.0 && MaxCmd(s.cmd) == 0.0 && !s.cmd.weaponDrive[0]);
}

// 16. The first target places the stand-in by the pelvis delta (a rigid move: limbs keep their own posture), then
// every body is servoed from where the move put it, caps ramping from 30 %: the command equals ServoFighter on the
// shifted bodies with the ramp-start params. The next frame does not place again.
static void TestDriverFirstPlacement() {
    const PoseFrame tg0 = StaticTarget(V3{ 100, 50, 95 });
    Feeder feed;
    feed.To(1000, tg0);
    FighterNow now = NowAtOffset(tg0, V3{ 40, -30, 5 });
    now.bones[kBoneHandR].pos.z += 60;   // a posture of its own: after the move the right hand is still 60 uu off
    StandInDriver d;
    PoseFrame tg;
    const double dt = 1.0 / 60.0;
    const DriverStep s = d.Step(feed.player, 1000, now, dt, false, tg);
    CHECK(s.have && s.place && !s.snap && s.info.cut);
    CHECK(Near(s.shift, V3{ -40, 30, -5 }, 1e-9));
    CHECK(std::fabs(s.pelvisErrUu - std::sqrt(40.0 * 40 + 30 * 30 + 5 * 5)) < 1e-9);
    FighterNow moved = now;
    ShiftFighterNow(moved, s.shift);
    CHECK(Near(moved.bones[kBonePelvis].pos, tg.bones[kBonePelvis].pos, 1e-9));
    CHECK(Near(moved.weapons[0].pos, V3{ now.weapons[0].pos.x - 40, now.weapons[0].pos.y + 30, now.weapons[0].pos.z - 5 }, 1e-9));
    ServoState st;
    st.StartRamp(1000);
    AimBuilder ab;
    ab.Observe(tg, s.info.playTsMs, true, false);
    const ServoParams p;
    CHECK(SameFighterCmd(s.cmd, ServoFighter(moved, tg, ab, false, dt, st.Effective(1000, p))));
    CHECK(MaxCmd(s.cmd) > 100.0 && MaxCmd(s.cmd) <= p.capLinUu * p.rampStartFrac + 1e-9);   // the 60 uu hand, ramp-capped
    feed.To(1016, tg0);
    const DriverStep s2 = d.Step(feed.player, 1016, moved, dt, false, tg);
    CHECK(s2.have && !s2.place && !s2.snap && !s2.info.cut);
}

// 17. Pelvis not read: the target is sampled but nothing is placed or driven, and the first placement still waits.
static void TestDriverPelvisUnread() {
    const PoseFrame tg0 = StaticTarget(V3{ 0, 0, 95 });
    Feeder feed;
    feed.To(1000, tg0);
    FighterNow now = NowAtOffset(tg0, V3{ 10, 0, 0 });
    now.valid[kBonePelvis] = false;
    StandInDriver d;
    PoseFrame tg;
    const DriverStep s = d.Step(feed.player, 1000, now, 1.0 / 60.0, false, tg);
    CHECK(s.have && !s.place && s.pelvisErrUu == -1.0 && MaxCmd(s.cmd) == 0.0 && !s.cmd.weaponDrive[0]);
    now.valid[kBonePelvis] = true;
    feed.To(1016, tg0);
    CHECK(d.Step(feed.player, 1016, now, 1.0 / 60.0, false, tg).place);
}

// 18. Snap rules: only a pelvis error above 150 uu held for 150 ms snaps (inclusive); an error back under the line
// restarts the clock; a vetoed snap waits for the veto to lift; each snap restarts the clock and the ramp.
static void TestDriverSnapRules() {
    const PoseFrame tg0 = StaticTarget(V3{ 0, 0, 95 });
    Feeder feed;
    StandInDriver d;
    PoseFrame tg;
    const double dt = 1.0 / 60.0;
    auto step = [&](double t, double off, bool blocked) {
        feed.To(t, tg0);
        return d.Step(feed.player, t, NowAtOffset(tg0, V3{ off, 0, 0 }), dt, blocked, tg);
    };
    CHECK(step(1000, 0, false).place);                       // first placement
    CHECK(!step(1016, 200, false).place);                    // error starts
    CHECK(!step(1100, 200, false).place);
    CHECK(!step(1165, 200, false).place);                    // 149 ms
    const DriverStep sn = step(1166, 200, false);            // 150 ms: snap
    CHECK(sn.place && sn.snap && Near(sn.shift, V3{ -200, 0, 0 }, 1e-9));
    CHECK(MaxCmd(sn.cmd) <= ServoParams{}.capLinUu * ServoParams{}.rampStartFrac + 1e-9);
    CHECK(!step(1182, 200, false).place);                    // the clock restarted at the snap
    CHECK(!step(1250, 100, false).place);                    // under the line: clock reset
    CHECK(!step(1260, 200, false).place);
    CHECK(!step(1409, 200, false).place);                    // 149 ms since 1260
    CHECK(!step(1410, 200, true).place);                     // due, but vetoed
    CHECK(!step(1420, 200, true).place);
    const DriverStep late = step(1430, 200, false);          // veto lifted
    CHECK(late.place && late.snap);
    CHECK(!step(1440, 150, false).place && !step(1700, 150, false).place);   // exactly 150 uu never snaps
}

// 19. A cut in the stream (a teleport-sized jump) places the stand-in again, as the first target did.
static void TestDriverCut() {
    const PoseFrame a = StaticTarget(V3{ 0, 0, 95 }), b = StaticTarget(V3{ 500, 0, 95 });
    Feeder feed;
    StandInDriver d;
    PoseFrame tg;
    const double dt = 1.0 / 60.0;
    feed.To(1000, a);
    CHECK(d.Step(feed.player, 1000, NowAtOffset(a, V3{}), dt, false, tg).place);
    feed.To(1016, a);
    CHECK(!d.Step(feed.player, 1016, NowAtOffset(a, V3{}), dt, false, tg).place);
    feed.To(1032, b);
    const DriverStep s = d.Step(feed.player, 1032, NowAtOffset(a, V3{}), dt, false, tg);
    CHECK(s.info.cut && s.place && !s.snap && Near(s.shift, V3{ 500, 0, 0 }, 1e-6));
}

// 20. An impact narrows the gain and caps for impactMs (ServoState::NotifyImpact), and Reset forgets the placement.
static void TestDriverImpactAndReset() {
    const PoseFrame tg0 = StaticTarget(V3{ 0, 0, 95 });
    Feeder feed;
    feed.To(1000, tg0);
    StandInDriver hit, calm;
    PoseFrame tg;
    const double dt = 1.0 / 60.0;
    const FighterNow placed = NowAtOffset(tg0, V3{});
    hit.Step(feed.player, 1000, placed, dt, false, tg);
    calm.Step(feed.player, 1000, placed, dt, false, tg);
    feed.To(1400, tg0);
    hit.NotifyImpact(1400);
    const FighterNow off = NowAtOffset(tg0, V3{ 0, 0, 30 });
    const DriverStep h = hit.Step(feed.player, 1400, off, dt, false, tg);
    const DriverStep c = calm.Step(feed.player, 1400, off, dt, false, tg);
    const ServoParams p;
    CHECK(!h.place && !c.place);
    CHECK(MaxCmd(h.cmd) <= p.impactCapUu + 1e-9 && MaxCmd(c.cmd) > p.impactCapUu);
    feed.To(1700, tg0);
    CHECK(std::fabs(MaxCmd(hit.Step(feed.player, 1700, off, dt, false, tg).cmd) - MaxCmd(calm.Step(feed.player, 1700, off, dt, false, tg).cmd)) < 1e-9);
    hit.Reset();
    CHECK(hit.Step(feed.player, 1700, off, dt, false, tg).place);
}

// 21. Blows (main.lua:4178-4196): a contact body whose read velocity is more than impactDvUu off the command it was
// given last frame, having jumped by more than impactDvUu since the frame before, is struck; the servo yields from
// that frame. A steady offset (a limb held against a joint or a grip) never counts, nor a jump during the start ramp,
// a body without a velocity read or without a command last frame, or anything with detection off.
static void TestDriverImpactDetection() {
    const PoseFrame tg0 = StaticTarget(V3{ 0, 0, 95 });
    const double dt = 1.0 / 60.0;
    const ServoParams p;
    struct Run {
        Feeder feed;
        StandInDriver d;
        PoseFrame tg;
        DriverStep last;
        explicit Run(const ServoParams& sp) : d(sp) {}
        // A frame where contact bone `b` reads `off` uu/s (along x) beyond the command it was given last frame;
        // the stand-in sits 30 uu above its target, so the servo command is a real correction.
        DriverStep At(double t, const PoseFrame& f, int b, double off, bool velRead = true) {
            feed.To(t, f);
            FighterNow now = NowAtOffset(f, V3{ 0, 0, 30 });
            for (int c : kContactBones) {
                now.vel[c] = last.cmd.bones[c].vel;
                now.velValid[c] = velRead;
            }
            now.vel[b].x += off;
            last = d.Step(feed.player, t, now, 1.0 / 60.0, false, tg);
            return last;
        }
    };
    {
        Run r(p);
        CHECK(r.At(1000, tg0, kBoneHandR, 0).place);           // placed: the ramp runs to 1300
        CHECK(!r.At(1100, tg0, kBoneHandR, 0).struck);
        CHECK(!r.At(1116, tg0, kBoneHandR, 900).struck);        // a jump (+900) during the ramp
        CHECK(!r.At(1310, tg0, kBoneHandR, 900).struck);        // steady 900 off: no step
        CHECK(!r.At(1326, tg0, kBoneHandR, 900).struck);
        const DriverStep hit = r.At(1342, tg0, kBoneHandR, 1300);   // +400 in one frame, 1300 off: struck
        CHECK(hit.struck && !hit.place);
        CHECK(MaxCmd(hit.cmd) <= p.impactCapUu + 1e-9);         // yielding from this very frame
        CHECK(!r.At(1358, tg0, kBoneHandR, 1300).struck);       // holding there is no new blow
        CHECK(MaxCmd(r.At(1400, tg0, kBoneHandR, 0).cmd) <= p.impactCapUu + 1e-9);   // still inside impactMs of 1342
        CHECK(MaxCmd(r.At(1560, tg0, kBoneHandR, 0).cmd) > p.impactCapUu);           // 218 ms later: full strength
    }
    for (int b : kContactBones) {                                // every contact body counts
        Run r(p);
        r.At(1000, tg0, b, 0);
        r.At(1310, tg0, b, 0);
        CHECK(r.At(1326, tg0, b, 301.0).struck);
    }
    {
        Run r(p);                                                // a body that is not a contact body does not
        r.At(1000, tg0, kBoneFootL, 0);
        r.At(1310, tg0, kBoneFootL, 0);
        CHECK(!r.At(1326, tg0, kBoneFootL, 5000).struck);
    }
    {
        Run r(p);                                                // both thresholds are strict
        r.At(1000, tg0, kBonePelvis, 0);
        r.At(1310, tg0, kBonePelvis, 0);
        CHECK(!r.At(1326, tg0, kBonePelvis, 300.0).struck);     // 300 off, +300: neither is over
        CHECK(!r.At(1342, tg0, kBonePelvis, 600.0).struck);     // 600 off but only +300
        CHECK(r.At(1358, tg0, kBonePelvis, 900.5).struck);      // +300.5
    }
    {
        Run r(p);                                                // no velocity read: nothing to compare
        r.At(1000, tg0, kBoneHead, 0, false);
        r.At(1310, tg0, kBoneHead, 0, false);
        CHECK(!r.At(1326, tg0, kBoneHead, 5000, false).struck);
        CHECK(!r.At(1342, tg0, kBoneHead, 0).struck);           // the first read after that has no step to compare
        CHECK(r.At(1358, tg0, kBoneHead, 400).struck);
    }
    {
        Run r(p);                                                // no command last frame (pelvis unread): no blow
        r.At(1000, tg0, kBoneHead, 0);
        r.At(1310, tg0, kBoneHead, 0);
        r.feed.To(1326, tg0);
        FighterNow blind = NowAtOffset(tg0, V3{ 0, 0, 30 });
        blind.valid[kBonePelvis] = false;
        for (int c : kContactBones) {                            // the velocities are read, on their last command
            blind.vel[c] = r.last.cmd.bones[c].vel;
            blind.velValid[c] = true;
        }
        r.last = r.d.Step(r.feed.player, 1326, blind, dt, false, r.tg);
        CHECK(!r.last.struck && MaxCmd(r.last.cmd) == 0.0);
        CHECK(!r.At(1342, tg0, kBoneHead, 5000).struck);        // 5000 off the 1310 command, but 1326 gave none
        CHECK(!r.At(1358, tg0, kBoneHead, 5000).struck);        // steady from here
        CHECK(r.At(1374, tg0, kBoneHead, 5400).struck);         // and live again
    }
    {
        ServoParams off = p;                                     // detection off
        off.impactDvUu = 0.0;
        Run r(off);
        r.At(1000, tg0, kBoneHandL, 0);
        r.At(1310, tg0, kBoneHandL, 0);
        CHECK(!r.At(1326, tg0, kBoneHandL, 5000).struck);
    }
    {
        Run r(p);                                                // Reset forgets the last command and offsets:
        r.At(1000, tg0, kBoneHandL, 0);                          // a respawned stand-in was never given them
        r.At(1310, tg0, kBoneHandL, 0);
        r.d.Reset();
        const DriverStep again = r.At(1326, tg0, kBoneHandL, 900);
        CHECK(again.place && !again.struck);
    }
    {
        ServoParams quick = p;                                   // the same with a 1 ms ramp, so the second frame
        quick.rampMs = 1.0;                                      // after a Reset is no longer masked by it: that
        Run r(quick);                                            // frame has no step history either
        r.At(1000, tg0, kBoneHandL, 0);
        r.At(1016, tg0, kBoneHandL, 0);
        r.At(1032, tg0, kBoneHandL, 0);
        r.d.Reset();
        CHECK(r.At(1048, tg0, kBoneHandL, 0).place);
        CHECK(!r.At(1064, tg0, kBoneHandL, 900).struck);
        CHECK(r.At(1080, tg0, kBoneHandL, 1300).struck);        // +400: detection is live
    }
}

int main() {
    TestAimBody();
    TestAccGapRules();
    TestAccClamp();
    TestAccCutAndFrozen();
    TestAccWeaponPresence();
    TestServoFighterSelection();
    TestServoFighterUsesAcc();
    TestServoFighterFrozen();
    TestOffsetPoseAndError();
    TestPosePipeIdeal();
    TestPipelineAcceptance();
    TestPipelineOtherLinks();
    TestAccAimBeatsConstantVelocity();
    TestPipelineDeterministic();
    TestDriverNoSample();
    TestDriverFirstPlacement();
    TestDriverPelvisUnread();
    TestDriverSnapRules();
    TestDriverCut();
    TestDriverImpactAndReset();
    TestDriverImpactDetection();
    if (g_fail) { std::printf("test_hsm_standin: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_standin: all passed\n");
    return 0;
}

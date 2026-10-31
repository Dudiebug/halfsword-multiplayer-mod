// Feed-forward velocity servo for one stand-in body: given its current bone transform and a
// target transform + replicated velocity, computes the linear/angular velocity to hand the
// engine's SetPhysicsLinearVelocity / SetPhysicsAngularVelocityInDegrees for the coming physics
// step. Pure math; no engine types, no allocation, no exceptions.
//
// Ported from Cyrex0/HalfSword-MP crates/hsmp-native/src/servo.rs and
// mods/HSMPAvatars/Scripts/avatars_pure.lua, Copyright (c) 2026 Cyrex0 and the HalfSword-MP
// contributors, MIT (see THIRD_PARTY_NOTICES.md).
//
// Per body, per step: v = f + gain*clamp(v_need - f, capLinUu), w = tg.angVelDeg +
// gain*clamp(w_need - tg.angVelDeg, capAngDeg), where f is the feed-forward velocity of the
// body's centre of mass (tg.vel + tg.angVelDeg x tc) and v_need / w_need are the one-step linear /
// angular velocity that would land the COM and orientation exactly on target (free body, no cap).
// com is the body's centre of mass in its own bone frame; cc / tc are that offset rotated into
// world space by the current / target orientation.
#pragma once
#include "sync/hsm_pose.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hsm::sync {

namespace detail {

inline constexpr double kPi = 3.14159265358979323846;

// Rotate v by unit quaternion q (x, y, z, w). avatars_pure.lua PURE.qrot / servo.rs qrot.
inline V3 QRot(const Q4& q, const V3& v) {
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    const double tx = 2.0 * (y * v.z - z * v.y);
    const double ty = 2.0 * (z * v.x - x * v.z);
    const double tz = 2.0 * (x * v.y - y * v.x);
    return V3{ v.x + w * tx + (y * tz - z * ty), v.y + w * ty + (z * tx - x * tz), v.z + w * tz + (x * ty - y * tx) };
}

// Hamilton product a*b (x, y, z, w). avatars_pure.lua PURE.qmul / servo.rs qmul.
inline Q4 QMul(const Q4& a, const Q4& b) {
    return Q4{
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

inline Q4 QConj(const Q4& q) { return Q4{ -q.x, -q.y, -q.z, q.w }; }

inline double Len3(double x, double y, double z) { return std::sqrt(x * x + y * y + z * z); }

}  // namespace detail

// Tunables, HSMP defaults. Each cites the mods/HSMPAvatars/Scripts/main.lua line that defines it
// (that file is the dev driver around avatars_pure.lua's pure PURE.servo/PURE.advance).
struct ServoParams {
    double gain = 0.3;          // share of the remaining error corrected per step; main.lua:176 SERVO_GAIN
    double legGain = 0.5;       // legs keep a stiffer servo (driven mesh has no floor friction); main.lua:1071 PX.LEG_GAIN
    double capLinUu = 900.0;    // correction cap beyond the feed-forward, uu/s; main.lua:165 SERVO_CAP_LIN
    double capAngDeg = 900.0;   // correction cap beyond the feed-forward, deg/s; main.lua:166 SERVO_CAP_ANG
    double minDtS = 0.004;      // dt clamp floor (4 ms); main.lua:4871 PX.h_pred = clamp(hp, 4, 80)
    double maxDtS = 0.080;      // dt clamp ceiling (80 ms); main.lua:4871
    double extrapMaxMs = 120.0; // advance a target by its velocity at most this long; main.lua:177 SERVO_EXTRAP_MS
    double rampMs = 300.0;      // soft-servo window after a clean (re)start; main.lua:3404 PX.RAMP_MS
    double rampStartFrac = 0.3; // caps start at this fraction of full strength; main.lua:4162 (0.3 + 0.7*k)
    double impactGain = 0.08;   // gain while yielding to a detected contact; main.lua:174 IMPACT_GAIN
    double impactCapUu = 200.0; // lin+ang cap (uu/s, deg/s) while yielding to a contact; main.lua:175 IMPACT_CAP
    double impactMs = 200.0;    // duration of the impact-yield window; main.lua:173 IMPACT_MS
    double impactDvUu = 300.0;  // a contact body this far (uu/s) off its last command, a step this large from the frame
                                // before, is struck; main.lua:172 IMPACT_DV (0 = detection off)
    double snapErrUu = 150.0;   // pelvis error (uu) that starts the snap timer; main.lua:138 SNAP_BODY_ERR
    double snapMs = 150.0;      // ... sustained this long before a rigid snap; main.lua:139 SNAP_BODY_MS
};

// Host physics (0.9.0): a client's copy of a fighter is only ever the host's picture. No soft start, no yield to contacts
// (a contact here is not the host's), no caps: the whole remaining error is corrected each step, so every body sits on
// the buffered host pose one step later. The rest (dt clamp, extrapolation, snap) as for a stand-in.
inline ServoParams ReplicaServoParams() {
    ServoParams p;
    p.gain = 1.0; p.legGain = 1.0;
    p.capLinUu = 1e6; p.capAngDeg = 1e6;
    p.rampMs = 0.0; p.rampStartFrac = 1.0;
    p.impactDvUu = 0.0;
    return p;
}

// Current state of one driven body: bone origin, bone rotation (both world), and the body's
// centre of mass in the bone's own local frame (constant per body; same vector servo.rs/avatars_pure
// call `com`).
struct BodyNow {
    V3 pos;
    Q4 rot;
    V3 comLocal;
};

// Target bone transform plus the replicated (feed-forward) velocity of the bone ORIGIN and the
// body's angular velocity, both world-frame, deg/s for angular.
struct BodyTarget {
    V3 pos;
    Q4 rot;
    V3 vel;
    V3 angVelDeg;
};

struct ServoCommand {
    V3 vel;       // uu/s, world
    V3 angVelDeg; // deg/s, world
};

// avatars_pure.lua PURE.servo / servo.rs servo(), operation for operation. gain/capLinUu/capAngDeg
// are passed in (not read from ServoParams) so callers can use ServoState::Effective's
// per-frame/per-bone values (legs use legGain; impact yield and the start ramp narrow gain/caps).
inline ServoCommand ServoBody(const BodyNow& cur, const BodyTarget& tg, double dtS, double gain, double capLinUu, double capAngDeg) {
    using namespace detail;
    const V3 cc = QRot(cur.rot, cur.comLocal);
    const V3 tc = QRot(tg.rot, cur.comLocal);

    double vx = (tg.pos.x + tc.x - cur.pos.x - cc.x) / dtS;
    double vy = (tg.pos.y + tc.y - cur.pos.y - cc.y) / dtS;
    double vz = (tg.pos.z + tc.z - cur.pos.z - cc.z) / dtS;

    // Shortest-arc orientation error: q and -q targets must give the same command (double cover).
    Q4 e = QMul(tg.rot, QConj(cur.rot));
    if (e.w < 0.0) { e = Q4{ -e.x, -e.y, -e.z, -e.w }; }
    const double s = Len3(e.x, e.y, e.z);
    double wx = 0.0, wy = 0.0, wz = 0.0;
    if (s > 1e-9) {
        const double k = (2.0 * std::atan2(s, e.w)) * (180.0 / kPi) / s / dtS;
        wx = e.x * k;
        wy = e.y * k;
        wz = e.z * k;
    }

    // Feed-forward velocity of the COM: the replicated origin velocity plus w x tc.
    const double wr = kPi / 180.0;
    const double ax = tg.angVelDeg.x * wr, ay = tg.angVelDeg.y * wr, az = tg.angVelDeg.z * wr;
    const double fx = tg.vel.x + ay * tc.z - az * tc.y;
    const double fy = tg.vel.y + az * tc.x - ax * tc.z;
    const double fz = tg.vel.z + ax * tc.y - ay * tc.x;

    double dx = (vx - fx) * gain, dy = (vy - fy) * gain, dz = (vz - fz) * gain;
    vx = fx + dx;
    vy = fy + dy;
    vz = fz + dz;
    const double dl = Len3(dx, dy, dz);
    if (dl > capLinUu) {
        const double kk = capLinUu / dl;
        vx = fx + dx * kk;
        vy = fy + dy * kk;
        vz = fz + dz * kk;
    }

    double gx = (wx - tg.angVelDeg.x) * gain, gy = (wy - tg.angVelDeg.y) * gain, gz = (wz - tg.angVelDeg.z) * gain;
    wx = tg.angVelDeg.x + gx;
    wy = tg.angVelDeg.y + gy;
    wz = tg.angVelDeg.z + gz;
    const double gl = Len3(gx, gy, gz);
    if (gl > capAngDeg) {
        const double kk = capAngDeg / gl;
        wx = tg.angVelDeg.x + gx * kk;
        wy = tg.angVelDeg.y + gy * kk;
        wz = tg.angVelDeg.z + gz * kk;
    }

    return ServoCommand{ V3{ vx, vy, vz }, V3{ wx, wy, wz } };
}

// Item 7c: a stand-in body never moves much faster than its owner's did. ServoBody adds a correction of up to the caps
// on top of the feed-forward, so a limb behind its target swings past it up to 900 uu/s faster than the owner's limb
// ever moved (whip). The command's speed is clamped to kOwnerSpeedShare x the owner's: the centre-of-mass feed-forward
// for linear, the target's angular velocity for angular. Never below the floors, so a body standing still can still be
// pulled back onto its target (30 uu off at 60 Hz asks 540 uu/s at gain 0.3; the floor closes it in about 6 frames). The linear floor stays above
// ServoParams::impactCapUu so the impact yield still softens a body at rest.
inline constexpr double kOwnerSpeedShare = 1.2;
inline constexpr double kOwnerSpeedFloorLinUu = 300.0;
inline constexpr double kOwnerSpeedFloorAngDeg = 360.0;

// The feed-forward velocity of the body's centre of mass, as ServoBody computes it: tg.vel + tg.angVelDeg x tc.
inline V3 FeedForwardCom(const BodyNow& cur, const BodyTarget& tg) {
    using namespace detail;
    const V3 c = QRot(tg.rot, cur.comLocal);
    const double wr = kPi / 180.0;
    const double ax = tg.angVelDeg.x * wr, ay = tg.angVelDeg.y * wr, az = tg.angVelDeg.z * wr;
    return V3{ tg.vel.x + ay * c.z - az * c.y, tg.vel.y + az * c.x - ax * c.z, tg.vel.z + ax * c.y - ay * c.x };
}

inline ServoCommand ClampToOwnerSpeed(const ServoCommand& c, const BodyNow& cur, const BodyTarget& tg) {
    using namespace detail;
    ServoCommand out = c;
    const V3 f = FeedForwardCom(cur, tg);
    const double limLin = std::max(kOwnerSpeedShare * Len3(f.x, f.y, f.z), kOwnerSpeedFloorLinUu);
    const double lin = Len3(c.vel.x, c.vel.y, c.vel.z);
    if (lin > limLin) {
        const double k = limLin / lin;
        out.vel = V3{ c.vel.x * k, c.vel.y * k, c.vel.z * k };
    }
    const double limAng = std::max(kOwnerSpeedShare * Len3(tg.angVelDeg.x, tg.angVelDeg.y, tg.angVelDeg.z), kOwnerSpeedFloorAngDeg);
    const double ang = Len3(c.angVelDeg.x, c.angVelDeg.y, c.angVelDeg.z);
    if (ang > limAng) {
        const double k = limAng / ang;
        out.angVelDeg = V3{ c.angVelDeg.x * k, c.angVelDeg.y * k, c.angVelDeg.z * k };
    }
    return out;
}

// Advances a target by aheadS seconds of constant velocity / constant angular velocity (no
// acceleration: the acceleration estimate is a driver-level concern, out of scope for this pure
// law). avatars_pure.lua adv_pos / adv_rot (PURE.advance without the optional `acc`).
inline BodyTarget ExtrapolateTarget(const BodyTarget& tg, double aheadS) {
    using namespace detail;
    BodyTarget out = tg;
    out.pos = V3{ tg.pos.x + tg.vel.x * aheadS, tg.pos.y + tg.vel.y * aheadS, tg.pos.z + tg.vel.z * aheadS };
    const double wr = kPi / 180.0;
    const double wx = tg.angVelDeg.x * wr * aheadS, wy = tg.angVelDeg.y * wr * aheadS, wz = tg.angVelDeg.z * wr * aheadS;
    const double a = Len3(wx, wy, wz);
    if (a > 1e-9) {
        const double h = std::sin(a * 0.5) / a;
        const Q4 d{ wx * h, wy * h, wz * h, std::cos(a * 0.5) };
        out.rot = QMul(d, tg.rot);
    }
    return out;
}

// dt as handed to ServoBody: clamp to [minDtS, maxDtS]. main.lua:4871 (see ServoParams::minDtS).
inline double ClampDt(double dtS, const ServoParams& params) { return std::min(std::max(dtS, params.minDtS), params.maxDtS); }

struct EffectiveParams {
    double gain;
    double legGain;
    double capLinUu;
    double capAngDeg;
};

// Per-stand-in state for the three policies layered on top of the pure servo law: the soft-start
// ramp after a clean (re)pose, the impact-yield window after a detected contact, and the
// sustained-pelvis-error snap timer. All of it is driven by a caller-supplied clock (nowMs);
// which clock (os.clock()*1000 in HSMP) doesn't matter as long as it's monotonic and shared.
class ServoState {
public:
    // Call once right after a clean (re)pose (physics reset, mesh snapped onto the target):
    // caps ramp from rampStartFrac to 1.0 over the next params.rampMs. main.lua:3362-3371 (PX.start_repose),
    // :4160-4163 (the ramp math).
    void StartRamp(double nowMs) {
        rampActive_ = true;
        rampStartMs_ = nowMs;
    }

    // Call once a contact on this body is detected (StandInDriver's dv-step detection, hsm_standin.h):
    // narrows gain/caps for the next params.impactMs.
    // main.lua:4178-4196 (detection), :4157-4159 (the clamp).
    void NotifyImpact(double nowMs, const ServoParams& params) { impactUntilMs_ = nowMs + params.impactMs; }

    // true from the moment the pelvis error has exceeded params.snapErrUu for params.snapMs,
    // and stays true every call after that until the caller confirms the snap with ConfirmSnap().
    // This mirrors main.lua's body.err_since: the duration gate is pure, but main.lua also gates
    // the actual teleport on snap_clear() (don't snap the stand-in into the local player), which
    // needs engine data this header doesn't have -- so the caller ANDs this with its own check and
    // only calls ConfirmSnap() once it actually performs the teleport. main.lua:138-139
    // (SNAP_BODY_ERR/MS), :4742-4753 and :4788-4804 (the two call sites).
    bool WantsSnap(double nowMs, double pelvisErrUu, const ServoParams& params) {
        if (pelvisErrUu <= params.snapErrUu) {
            errSinceMs_ = kNoError;
            return false;
        }
        if (errSinceMs_ == kNoError) { errSinceMs_ = nowMs; }
        return nowMs - errSinceMs_ >= params.snapMs;
    }

    // Call once the caller has actually performed the snap WantsSnap asked for.
    void ConfirmSnap() { errSinceMs_ = kNoError; }

    // The start ramp is still narrowing the caps at nowMs (impact detection is off meanwhile: main.lua:4188
    // `not body.ramp_at`).
    bool Ramping(double nowMs, const ServoParams& params) const {
        if (!rampActive_ || !(params.rampMs > 0.0)) return false;   // rampMs 0: no ramp (ReplicaServoParams), never 0/0
        const double k = (nowMs - rampStartMs_) / params.rampMs;
        return k >= 0.0 && k < 1.0;
    }

    // The gain/caps to hand ServoBody this frame: params narrowed by an active impact-yield
    // window, then caps scaled by an active start ramp (same order as main.lua:4154-4163).
    // legGain is never narrowed by either: main.lua's native path (a.leg_gain, :1013) only
    // overrides it for the unrelated interaction-yield system, out of scope here, so legs keep a
    // steady leg gain through both impact yield and the start ramp.
    EffectiveParams Effective(double nowMs, const ServoParams& params) {
        double gain = params.gain, capLin = params.capLinUu, capAng = params.capAngDeg;
        if (nowMs < impactUntilMs_) {
            gain = std::min(gain, params.impactGain);
            capLin = std::min(capLin, params.impactCapUu);
            capAng = std::min(capAng, params.impactCapUu);
        }
        if (rampActive_ && !(params.rampMs > 0.0)) rampActive_ = false;
        if (rampActive_) {
            const double k = (nowMs - rampStartMs_) / params.rampMs;
            if (k >= 1.0 || k < 0.0) {
                rampActive_ = false;
            } else {
                const double f = params.rampStartFrac + (1.0 - params.rampStartFrac) * k;
                capLin *= f;
                capAng *= f;
            }
        }
        return EffectiveParams{ gain, params.legGain, capLin, capAng };
    }

private:
    static constexpr double kNoError = std::numeric_limits<double>::lowest();
    bool rampActive_ = false;
    double rampStartMs_ = 0.0;
    double impactUntilMs_ = kNoError;
    double errSinceMs_ = kNoError;
};

}  // namespace hsm::sync

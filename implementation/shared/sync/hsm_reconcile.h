// Rev 42, error-offset reconciliation (competitive netcode Phase 5, docs/PLAN_COMPETITIVE_NETCODE.md 6.4). Pure logic, unit-tested.
//
// Rollback would rewind to the owner's authoritative pose and re-simulate to now. Chaos cannot (no determinism, no
// whole-world save/restore, no budget for re-stepping ragdolls), so this does what that re-simulation approximates:
// the stand-in, driven by the owner's inputs (sync/hsm_inputline.h), keeps a history of where its own bodies were at
// each owner-clock time. When the owner's Pose for time t arrives, the error is the pose minus the stand-in's own state
// at t; assuming the gap held since, it is carried into the present and worked off over kReconcileTauMs through the
// velocity servo -- a correction the eye reads as the stand-in drifting onto the truth, never a pop. An error past the
// snap distances is not slewed: the caller places the stand-in on the pose (the existing cut / snap rules).
//
// Step() gives each frame's share of the correction as velocity added on top of what the muscles do.
#pragma once
#include "sync/hsm_pose.h"
#include "sync/hsm_standin.h"   // FighterNow
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hsm::sync {

constexpr double kReconcileTauMs = 80.0;          // the error's time constant: a correction is ~70% worked off in 100 ms
constexpr double kReconcileSnapUu = 120.0;        // a pelvis error past this is placed, not slewed (poseplay kPlayBlendMaxUu)
constexpr size_t kReconcileHistory = 48;          // stand-in states kept (0.8 s at 60 Hz: more than a round trip)
constexpr size_t kReconcileSlots = size_t(kPoseBones) + size_t(kPoseWeapons);
constexpr double kReconcileMaxSpeedUu = 500.0;    // the most correction velocity a body carries (a brisk walk)
constexpr double kReconcileMaxSpinDeg = 360.0;    // and the most correction spin

class Reconciler {
public:
    struct Snapshot {
        double ownerTs = 0;
        std::array<V3, kReconcileSlots> pos{};
        std::array<Q4, kReconcileSlots> rot{};
        std::array<bool, kReconcileSlots> valid{};
    };
    struct Result {
        bool applied = false;   // the pose found a history entry to compare with
        bool snap = false;      // the pelvis error was past kReconcileSnapUu: place the stand-in, do not slew
        double pelvisErrUu = 0; // the error at the pose's time
        double maxErrUu = 0;    // the largest body error
    };

    // The stand-in's bodies this frame, at owner-clock time ownerTs (the time its driving inputs belonged to).
    void Record(double ownerTs, const FighterNow& now) {
        lastRecordTs_ = ownerTs;
        if (n_ > 0 && ownerTs <= At(n_ - 1).ownerTs) return;
        Snapshot& s = ring_[(head_ + n_) % kReconcileHistory];
        if (n_ == kReconcileHistory) head_ = (head_ + 1) % kReconcileHistory; else ++n_;
        s.ownerTs = ownerTs;
        for (size_t i = 0; i < size_t(kPoseBones); ++i) { s.pos[i] = now.bones[i].pos; s.rot[i] = now.bones[i].rot; s.valid[i] = now.valid[i]; }
        for (size_t w = 0; w < size_t(kPoseWeapons); ++w) {
            const size_t i = size_t(kPoseBones) + w;
            s.pos[i] = now.weapons[w].pos; s.rot[i] = now.weapons[w].rot; s.valid[i] = now.weaponValid[w];
        }
    }

    // The owner's authoritative pose. Its error against the stand-in's own state at the same owner time, less what the
    // correction already moved the stand-in since then (it is in the history after that time, not in the error), replaces
    // the running correction: the newest truth wins; an older pose than the last one used is ignored.
    Result OnAuth(const PoseFrame& f) {
        Result r;
        const double ts = double(f.tsMs);
        if (haveAuth_ && ts <= lastAuthTs_) return r;
        Snapshot past;
        if (!StateAt(ts, past)) return r;
        haveAuth_ = true; lastAuthTs_ = ts;
        for (size_t i = 0; i < kReconcileSlots; ++i) {
            const bool bone = i < size_t(kPoseBones);
            const size_t w = i - size_t(kPoseBones);
            const bool present = bone ? kPoseHasBody[i] : f.weapons[w].present;
            if (!past.valid[i] || !present) { corrPos_[i] = V3{}; corrRot_[i] = Q4{}; continue; }
            const V3 tp = bone ? f.bones[i].pos : f.weapons[w].pos;
            const Q4 tr = bone ? f.bones[i].rot : f.weapons[w].rot;
            const AppliedSince since = Since(ts, i);
            corrPos_[i] = V3{ tp.x - past.pos[i].x - since.pos.x, tp.y - past.pos[i].y - since.pos.y, tp.z - past.pos[i].z - since.pos.z };
            corrRot_[i] = Normalize(Mul(Mul(tr, Conj(past.rot[i])), Conj(since.rot)));   // world delta rotation: truth = delta * past
            const double e = Len(corrPos_[i]);
            r.maxErrUu = std::max(r.maxErrUu, e);
            if (i == size_t(kBonePelvis)) r.pelvisErrUu = e;
        }
        r.applied = true;
        r.snap = r.pelvisErrUu > kReconcileSnapUu;
        if (r.snap) Drop();
        return r;
    }

    // This frame's share of the correction as velocity ADDED to each body (UE SetPhysicsLinearVelocity /
    // SetPhysicsAngularVelocityInDegrees with bAddToCurrent): the muscles' own motion is kept and only the error is
    // steered out. A body keeps velocity it was given, so each frame adds only the CHANGE of the correction velocity
    // (this frame's share over dt, capped at kReconcileMaxSpeedUu / kReconcileMaxSpinDeg, minus last frame's): the
    // bodies carry exactly the correction velocity, never a sum of them. The share given is taken off the correction,
    // so the error decays as exp(-t / tau) at any frame rate.
    struct Nudge {
        std::array<V3, kReconcileSlots> linUu{};    // uu/s to add
        std::array<V3, kReconcileSlots> angDeg{};   // deg/s to add, world axes
        std::array<bool, kReconcileSlots> any{};
    };
    Nudge Step(double dtMs) {
        Nudge n;
        if (!(dtMs > 0) || !std::isfinite(dtMs)) return n;
        const double k = 1.0 - std::exp(-dtMs / kReconcileTauMs), dtS = dtMs / 1000.0;
        Applied& a = applied_[(appliedHead_ + appliedN_) % kReconcileHistory];
        if (appliedN_ == kReconcileHistory) appliedHead_ = (appliedHead_ + 1) % kReconcileHistory; else ++appliedN_;
        a.ownerTs = lastRecordTs_;
        for (size_t i = 0; i < kReconcileSlots; ++i) {
            V3 v{ corrPos_[i].x * k / dtS, corrPos_[i].y * k / dtS, corrPos_[i].z * k / dtS };
            if (const double sp = Len(v); sp > kReconcileMaxSpeedUu) v = V3{ v.x * kReconcileMaxSpeedUu / sp, v.y * kReconcileMaxSpeedUu / sp, v.z * kReconcileMaxSpeedUu / sp };
            const V3 dp{ v.x * dtS, v.y * dtS, v.z * dtS };
            Q4 dq = Slerp(Q4{}, corrRot_[i], k);
            V3 w = AngularVelDeg(dq, dtS);
            if (const double sp = Len(w); sp > kReconcileMaxSpinDeg) {
                w = V3{ w.x * kReconcileMaxSpinDeg / sp, w.y * kReconcileMaxSpinDeg / sp, w.z * kReconcileMaxSpinDeg / sp };
                dq = FromAngularVelDeg(w, dtS);
            }
            corrPos_[i] = V3{ corrPos_[i].x - dp.x, corrPos_[i].y - dp.y, corrPos_[i].z - dp.z };
            corrRot_[i] = Normalize(Mul(corrRot_[i], Conj(dq)));
            a.pos[i] = dp; a.rot[i] = dq;
            n.linUu[i] = V3{ v.x - lastLin_[i].x, v.y - lastLin_[i].y, v.z - lastLin_[i].z };
            n.angDeg[i] = V3{ w.x - lastAng_[i].x, w.y - lastAng_[i].y, w.z - lastAng_[i].z };
            lastLin_[i] = v; lastAng_[i] = w;
            n.any[i] = Len(n.linUu[i]) > 1e-6 || Len(n.angDeg[i]) > 1e-6;
        }
        return n;
    }

    // What is left to correct, uu (the largest body), for the health line.
    double Pending() const { double m = 0; for (const V3& c : corrPos_) m = std::max(m, Len(c)); return m; }
    // Drop: nothing more to correct (the next Step takes back the correction velocity the bodies still carry).
    void Drop() { corrPos_.fill(V3{}); corrRot_.fill(Q4{}); }
    // Reset: the stand-in was placed (its velocities zeroed) or replaced: everything starts over.
    void Reset() {
        Drop(); n_ = 0; head_ = 0; haveAuth_ = false; lastAuthTs_ = 0; lastRecordTs_ = 0;
        appliedN_ = 0; appliedHead_ = 0; lastLin_.fill(V3{}); lastAng_.fill(V3{});
    }
    size_t HistorySize() const { return n_; }
    // The newest owner time recorded: a pose newer than this cannot be compared yet (keep it until the history gets there).
    double NewestTs() const { return n_ ? At(n_ - 1).ownerTs : -1.0; }
    // The correction velocity slot i carries now (uu/s): what moves the fighter's root along with its pelvis.
    V3 CarriedLin(size_t i) const { return i < kReconcileSlots ? lastLin_[i] : V3{}; }

private:
    struct Applied { double ownerTs = 0; std::array<V3, kReconcileSlots> pos{}; std::array<Q4, kReconcileSlots> rot{}; };
    struct AppliedSince { V3 pos; Q4 rot; };
    const Snapshot& At(size_t i) const { return ring_[(head_ + i) % kReconcileHistory]; }

    // What the correction moved slot i in the frames recorded after owner time ts (already in the history after ts).
    AppliedSince Since(double ts, size_t i) const {
        AppliedSince r;
        for (size_t k = 0; k < appliedN_; ++k) {
            const Applied& a = applied_[(appliedHead_ + k) % kReconcileHistory];
            if (a.ownerTs <= ts) continue;
            r.pos = V3{ r.pos.x + a.pos[i].x, r.pos.y + a.pos[i].y, r.pos.z + a.pos[i].z };
            r.rot = Normalize(Mul(a.rot[i], r.rot));
        }
        return r;
    }

    // The stand-in at owner time ts: interpolated between the two snapshots around it; false outside the history.
    bool StateAt(double ts, Snapshot& out) const {
        if (n_ == 0 || ts < At(0).ownerTs || ts > At(n_ - 1).ownerTs) return false;
        for (size_t i = 1; i < n_; ++i) {
            const Snapshot& a = At(i - 1); const Snapshot& b = At(i);
            if (ts > b.ownerTs) continue;
            const double span = b.ownerTs - a.ownerTs, s = span > 0 ? (ts - a.ownerTs) / span : 1.0;
            out.ownerTs = ts;
            for (size_t k = 0; k < kReconcileSlots; ++k) {
                out.valid[k] = a.valid[k] && b.valid[k];
                out.pos[k] = V3{ a.pos[k].x + (b.pos[k].x - a.pos[k].x) * s, a.pos[k].y + (b.pos[k].y - a.pos[k].y) * s, a.pos[k].z + (b.pos[k].z - a.pos[k].z) * s };
                out.rot[k] = Slerp(a.rot[k], b.rot[k], s);
            }
            return true;
        }
        out = At(n_ - 1);   // ts equals the only snapshot
        return true;
    }

    static double Len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
    static Q4 Conj(const Q4& q) { return Q4{ -q.x, -q.y, -q.z, q.w }; }
    static Q4 Mul(const Q4& a, const Q4& b) {
        return Q4{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                   a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
    }
    static Q4 Normalize(const Q4& q) {
        const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return l > 1e-12 && std::isfinite(l) ? Q4{ q.x / l, q.y / l, q.z / l, q.w / l } : Q4{};
    }
    // The angular velocity (deg/s, world) that turns by unit quaternion q in dtS.
    static V3 AngularVelDeg(const Q4& q0, double dtS) {
        Q4 q = q0.w < 0 ? Q4{ -q0.x, -q0.y, -q0.z, -q0.w } : q0;
        const double sinHalf = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
        if (sinHalf < 1e-12 || dtS <= 0) return V3{};
        const double angle = 2.0 * std::atan2(sinHalf, q.w);   // radians
        const double k = angle / sinHalf / dtS * (180.0 / 3.14159265358979323846);
        return V3{ q.x * k, q.y * k, q.z * k };
    }
    static Q4 FromAngularVelDeg(const V3& w, double dtS) {   // the inverse of AngularVelDeg
        const double rad = 3.14159265358979323846 / 180.0;
        const V3 r{ w.x * rad * dtS, w.y * rad * dtS, w.z * rad * dtS };
        const double angle = Len(r);
        if (angle < 1e-12) return Q4{};
        const double s = std::sin(angle * 0.5) / angle;
        return Q4{ r.x * s, r.y * s, r.z * s, std::cos(angle * 0.5) };
    }
    static Q4 Slerp(const Q4& a, Q4 b, double s) {   // nlerp the short way: the angles here are small
        if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) b = Q4{ -b.x, -b.y, -b.z, -b.w };
        return Normalize(Q4{ a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, a.z + (b.z - a.z) * s, a.w + (b.w - a.w) * s });
    }

    std::array<Snapshot, kReconcileHistory> ring_{};
    size_t head_ = 0, n_ = 0;
    bool haveAuth_ = false;
    double lastAuthTs_ = 0, lastRecordTs_ = 0;
    std::array<Applied, kReconcileHistory> applied_{};   // the correction given each frame, by the owner time recorded then
    size_t appliedHead_ = 0, appliedN_ = 0;
    std::array<V3, kReconcileSlots> lastLin_{}, lastAng_{};   // the correction velocity the bodies carry now
    std::array<V3, kReconcileSlots> corrPos_{};
    std::array<Q4, kReconcileSlots> corrRot_{};
};

}  // namespace hsm::sync

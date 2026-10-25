// Whole-fighter stand-in drive: from the pose sampled out of the jitter buffer (hsm_poseplay.h) to the velocity
// command of every driven body for the coming physics step (hsm_servo.h), plus the pose pipe used by the mirror
// test and the offline pipeline tests (encode -> simulated link -> decode -> jitter buffer). Pure: no engine types.
//
// The aim (where each body must stand at the end of the coming step) follows Cyrex0/HalfSword-MP
// mods/HSMPAvatars/Scripts/avatars_pure.lua PURE.advance / PURE.aim / PURE.clamp_acc3 and the per-slot
// acceleration estimate of mods/HSMPAvatars/Scripts/main.lua:4013-4040, Copyright (c) 2026 Cyrex0 and the
// HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md): the sampled pose advanced by its velocity and a
// bounded acceleration estimated from consecutive samples, approached with the chord velocity of the step, so a
// curved swing is followed along the curve rather than its tangent.
//
// Not ported: HSMP's FK retarget (PURE.fk_retarget) -- our stand-ins spawn from the owner's own passport, so they
// have the owner's skeleton and scale and the streamed bone positions already fit them; the rate-slewed playback
// clock and display_times (PosePlayer owns playback timing here).
#pragma once
#include "sync/hsm_linksim.h"
#include "sync/hsm_pose.h"
#include "sync/hsm_pose_codec.h"
#include "sync/hsm_poseplay.h"
#include "sync/hsm_servo.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hsm::sync {

inline constexpr double kAimAccMaxUu = 40000.0;   // uu/s^2, avatars_pure.lua PURE.clamp_acc3
inline constexpr double kAimAccMinGapMs = 2.0;    // consecutive samples closer than this keep the previous estimate, main.lua:4017
inline constexpr double kAimAccMaxGapMs = 80.0;   // ... farther apart than this clear it, main.lua:4017-4029
inline constexpr int kAimSlots = kPoseBones + kPoseWeapons;
// A flail head is servoed softer than the bodies (gain and caps scaled by these): the chain between it and the weapon
// stays simulated, so the servo only steers the head along the owner's swing, and a blow or the floor can still move it.
inline constexpr double kHeadGainShare = 0.5;
inline constexpr double kHeadCapShare = 0.5;
// Bodies watched for blows: pelvis, spine_03, head, hand_l, hand_r (main.lua:3885 PX.CONTACT_BODIES).
inline constexpr std::array<int, 5> kContactBones = { kBonePelvis, 3, kBoneHead, kBoneHandL, kBoneHandR };

namespace sidetail {
inline V3 Add(const V3& a, const V3& b) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(const V3& a, const V3& b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Scale(const V3& a, double s) { return V3{ a.x * s, a.y * s, a.z * s }; }
inline double Len(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline V3 ClampLen(const V3& a, double lim) {
    const double l = Len(a);
    return l > lim ? Scale(a, lim / l) : a;
}
}  // namespace sidetail

// One body's servo aim for a step of hS seconds: avatars_pure.lua PURE.aim(tg, 0, h, acc). Position and rotation
// advanced by the target's own motion (position with the acceleration), feed-forward = the chord velocity of the
// step (v + a*h/2), angular velocity unchanged.
inline BodyTarget AimBody(const BodyTarget& tg, const V3& acc, double hS) {
    using namespace sidetail;
    BodyTarget out = ExtrapolateTarget(tg, hS);
    out.pos = Add(out.pos, Scale(acc, 0.5 * hS * hS));
    out.vel = Add(tg.vel, Scale(acc, 0.5 * hS));
    return out;
}

inline BodyTarget TargetOf(const BoneSample& b) { return BodyTarget{ b.pos, b.rot, b.vel, b.angVelDeg }; }
inline BodyTarget TargetOf(const WeaponSample& w) { return BodyTarget{ w.pos, w.rot, w.vel, w.angVelDeg }; }

// Per-stand-in acceleration estimate from consecutive sampled targets (slots: 23 bones, then 2 weapons).
class AimBuilder {
public:
    void Reset() { *this = AimBuilder{}; }

    // Feed every sampled target once per frame. playTsMs: the sender time it was evaluated at
    // (SampleInfo::playTsMs); cut: the buffer was reset (teleport, gap); frozen: stale data is being held (no
    // estimate is updated and Acc() reads zero until fresh data returns). A weapon slot missing from either
    // sample gets no estimate (HSMP keeps that slot's old one; zero is the safer reading of "unknown").
    void Observe(const PoseFrame& target, double playTsMs, bool cut, bool frozen) {
        frozen_ = frozen;
        if (cut) { havePrev_ = false; acc_.fill(V3{}); }
        if (frozen) return;
        const double pt = playTsMs;
        const double gap = pt - prevPt_;
        std::array<V3, kAimSlots> v{};
        std::array<bool, kAimSlots> have{};
        for (int i = 0; i < kPoseBones; ++i) { v[i] = target.bones[i].vel; have[i] = true; }
        for (int w = 0; w < kPoseWeapons; ++w) { v[kPoseBones + w] = target.weapons[w].vel; have[kPoseBones + w] = target.weapons[w].present; }
        if (havePrev_ && gap >= kAimAccMinGapMs && gap <= kAimAccMaxGapMs) {
            const double k = 1000.0 / gap;
            for (int s = 0; s < kAimSlots; ++s)
                acc_[s] = have[s] && prevHave_[s] ? sidetail::ClampLen(sidetail::Scale(sidetail::Sub(v[s], prevV_[s]), k), kAimAccMaxUu) : V3{};
        } else if (!havePrev_ || gap > kAimAccMaxGapMs) {
            acc_.fill(V3{});
        }
        prevV_ = v;
        prevHave_ = have;
        prevPt_ = pt;
        havePrev_ = true;
    }

    // The estimate for slot s (bone index, or kPoseBones + weapon index); zero while frozen.
    V3 Acc(int s) const { return frozen_ || s < 0 || s >= kAimSlots ? V3{} : acc_[s]; }

private:
    std::array<V3, kAimSlots> acc_{};
    std::array<V3, kAimSlots> prevV_{};
    std::array<bool, kAimSlots> prevHave_{};
    double prevPt_ = 0.0;
    bool havePrev_ = false;
    bool frozen_ = false;
};

// The stand-in's bodies as read from the engine this frame. weapons[w] must be the stand-in's weapon in the hand
// of the target's weapons[w] (the caller matches them by WeaponSample::hand).
struct FighterNow {
    std::array<BodyNow, kPoseBones> bones{};
    std::array<bool, kPoseBones> valid{};
    // The body's linear velocity (centre of mass, what SetPhysicsLinearVelocity sets), read for kContactBones only.
    std::array<V3, kPoseBones> vel{};
    std::array<bool, kPoseBones> velValid{};
    std::array<BodyNow, kPoseWeapons> weapons{};
    std::array<bool, kPoseWeapons> weaponValid{};
    // The flail head of weapons[w] (its bone origin, comLocal zero), when that weapon is a flail whose chain simulates.
    std::array<BodyNow, kPoseWeapons> heads{};
    std::array<bool, kPoseWeapons> headValid{};
};

struct FighterCommand {
    std::array<ServoCommand, kPoseBones> bones{};
    std::array<bool, kPoseBones> drive{};
    std::array<ServoCommand, kPoseWeapons> weapons{};
    std::array<bool, kPoseWeapons> weaponDrive{};
    std::array<ServoCommand, kPoseWeapons> heads{};
    std::array<bool, kPoseWeapons> headDrive{};
};

// The aim for one body: advanced over the step, or -- while frozen on stale data -- the held pose standing
// still (zero velocities), main.lua:4350/4512 (`holding` drops the target's velocities). A stale sample carries
// the newest frame's velocities verbatim, which would otherwise keep pushing the stand-in off the held pose.
inline BodyTarget AimOrHold(const BodyTarget& tg, const V3& acc, double hS, bool frozen) {
    return frozen ? BodyTarget{ tg.pos, tg.rot, V3{}, V3{} } : AimBody(tg, acc, hS);
}

// Commands for every body that has a physics body (spine_01 has none), was read this frame, and (weapons) is
// present in the target. Legs use legGain, everything else gain; caps as given. dtS: the coming step, already
// clamped with ClampDt.
inline FighterCommand ServoFighter(const FighterNow& now, const PoseFrame& target, const AimBuilder& aim, bool frozen, double dtS,
                                   const EffectiveParams& p) {
    FighterCommand out;
    for (int i = 0; i < kPoseBones; ++i) {
        if (!kPoseHasBody[i] || !now.valid[i]) continue;
        const BodyTarget a = AimOrHold(TargetOf(target.bones[i]), aim.Acc(i), dtS, frozen);
        out.bones[i] = ClampToOwnerSpeed(ServoBody(now.bones[i], a, dtS, IsLegBone(i) ? p.legGain : p.gain, p.capLinUu, p.capAngDeg), now.bones[i], a);
        out.drive[i] = true;
    }
    for (int w = 0; w < kPoseWeapons; ++w) {
        if (!target.weapons[w].present || !now.weaponValid[w]) continue;
        const BodyTarget a = AimOrHold(TargetOf(target.weapons[w]), aim.Acc(kPoseBones + w), dtS, frozen);
        out.weapons[w] = ClampToOwnerSpeed(ServoBody(now.weapons[w], a, dtS, p.gain, p.capLinUu, p.capAngDeg), now.weapons[w], a);
        out.weaponDrive[w] = true;
        // The flail head of a commanded weapon, when the target has one and the stand-in's chain simulates. No
        // acceleration estimate: the head swings on a chain, and its own velocity already leads the swing.
        if (!target.weapons[w].hasHead || !now.headValid[w]) continue;
        const BodyTarget h = AimOrHold(TargetOf(target.weapons[w].head), V3{}, dtS, frozen);
        out.heads[w] = ClampToOwnerSpeed(ServoBody(now.heads[w], h, dtS, p.gain * kHeadGainShare, p.capLinUu * kHeadCapShare, p.capAngDeg * kHeadCapShare),
                                         now.heads[w], h);
        out.headDrive[w] = true;
    }
    return out;
}

// Moves a whole pose by `d` (positions, weapon roots and blades; rotations and velocities unchanged). The mirror
// test drives its stand-in beside the player this way.
inline void OffsetPose(PoseFrame& f, const V3& d) {
    using sidetail::Add;
    for (BoneSample& b : f.bones) b.pos = Add(b.pos, d);
    for (WeaponSample& w : f.weapons) {
        w.pos = Add(w.pos, d);
        w.base = Add(w.base, d);
        w.tip = Add(w.tip, d);
        w.head.pos = Add(w.head.pos, d);
    }
}

// Distance between the stand-in's bone origin and the target's, uu (-1 when the bone was not read).
inline double BoneError(const FighterNow& now, const PoseFrame& target, int bone) {
    if (bone < 0 || bone >= kPoseBones || !now.valid[bone]) return -1.0;
    return sidetail::Len(sidetail::Sub(now.bones[bone].pos, target.bones[bone].pos));
}

// Moves every read body (bones and weapons) by `d`: what a rigid placement does to the stand-in.
inline void ShiftFighterNow(FighterNow& n, const V3& d) {
    for (int i = 0; i < kPoseBones; ++i) if (n.valid[i]) n.bones[i].pos = sidetail::Add(n.bones[i].pos, d);
    for (int w = 0; w < kPoseWeapons; ++w) if (n.weaponValid[w]) n.weapons[w].pos = sidetail::Add(n.weapons[w].pos, d);
    for (int w = 0; w < kPoseWeapons; ++w) if (n.headValid[w]) n.heads[w].pos = sidetail::Add(n.heads[w].pos, d);
}

// ---- the per-frame stand-in driver ------------------------------------------------------------------------------
// One frame of one stand-in, before the physics step: sample the target, decide a placement, then servo every body.
// A placement moves the whole stand-in rigidly (every mesh body and held weapon) by `shift` = target pelvis - stand-in
// pelvis, velocities zeroed -- HSMP main.lua:2340 snap_mesh -- so the servo then only has to bend limbs into place.
// It happens on the first target, on every cut (teleport, stream gap: main.lua start_repose) and when the pelvis
// error stays above snapErrUu for snapMs (main.lua:4742-4804), unless the caller vetoes that snap (snapBlocked:
// main.lua:2432 snap_clear, the target pelvis is inside the local player). Every placement restarts the cap ramp.
struct DriverStep {
    bool have = false;          // a target was sampled this frame (target and info are valid)
    bool place = false;         // move the whole stand-in by `shift` (velocities zeroed) before applying cmd
    bool snap = false;          // ... because of a sustained pelvis error (not the first target or a cut)
    V3 shift{};
    double pelvisErrUu = -1.0;  // stand-in pelvis vs this frame's target, before any placement; -1 = pelvis not read
    bool struck = false;        // a contact body was knocked off its last command: the servo yields from this frame
    SampleInfo info{};
    FighterCommand cmd{};       // for the stand-in after the placement; empty while the pelvis is not read
};

class StandInDriver {
public:
    explicit StandInDriver(const ServoParams& p = ServoParams{}, bool useAcc = true) : params_(p), useAcc_(useAcc) {}

    // A new stand-in (or the old one respawned): the next target places it again.
    void Reset() {
        placed_ = false;
        aim_.Reset();
        state_ = ServoState{};
        lastCmd_ = FighterCommand{};
        dvPrev_.fill(-1.0);
    }
    // The stand-in was struck by something Step cannot see: yield for impactMs (ServoState::NotifyImpact).
    void NotifyImpact(double nowMs) { state_.NotifyImpact(nowMs, params_); }
    const ServoParams& Params() const { return params_; }

    // now: the stand-in's bodies as read this frame; nextDtS: the coming physics step (clamped here); target
    // receives this frame's sampled pose.
    DriverStep Step(PosePlayer& player, double nowMs, const FighterNow& now, double nextDtS, bool snapBlocked, PoseFrame& target) {
        DriverStep s;
        s.struck = DetectImpact(nowMs, now);
        if (s.struck) state_.NotifyImpact(nowMs, params_);   // yielding from this frame's command on
        lastCmd_ = FighterCommand{};                          // until a command is computed below
        if (!player.Sample(static_cast<int64_t>(std::llround(nowMs)), target, &s.info)) return s;
        s.have = true;
        s.pelvisErrUu = BoneError(now, target, kBonePelvis);
        if (s.pelvisErrUu < 0.0) return s;   // no pelvis read: nothing to place it by, and no frame to servo from
        if (!placed_ || s.info.cut) {
            s.place = true;
        } else if (state_.WantsSnap(nowMs, s.pelvisErrUu, params_) && !snapBlocked) {
            s.place = s.snap = true;
        }
        FighterNow body = now;
        if (s.place) {
            s.shift = sidetail::Sub(target.bones[kBonePelvis].pos, now.bones[kBonePelvis].pos);
            ShiftFighterNow(body, s.shift);
            if (!placed_) aim_.Reset();
            placed_ = true;
            state_.ConfirmSnap();   // the pelvis is on its target now: any running error clock starts over
            state_.StartRamp(nowMs);
        }
        if (useAcc_) aim_.Observe(target, static_cast<double>(s.info.playTsMs), s.info.cut, s.info.stale);
        s.cmd = ServoFighter(body, target, aim_, s.info.stale, ClampDt(nextDtS, params_), state_.Effective(nowMs, params_));
        lastCmd_ = s.cmd;
        return s;
    }

private:
    // main.lua:4178-4196: a blow is a STEP in how far the solver left a contact body off its last command. A limb
    // held against a joint or a grip sits a steady ~900 uu/s off the capped servo every frame (HSMP measured 438
    // false "struck" in a 70-round gate without one blow), so the offset alone never counts. Off while the start ramp
    // runs. Unlike main.lua the yield starts with this frame's command rather than the next one's.
    bool DetectImpact(double nowMs, const FighterNow& now) {
        const double idv = params_.impactDvUu;
        bool struck = false;
        for (size_t k = 0; k < kContactBones.size(); ++k) {
            const int b = kContactBones[k];
            if (!now.velValid[b] || !lastCmd_.drive[b]) { dvPrev_[k] = -1.0; continue; }
            const double dv = sidetail::Len(sidetail::Sub(now.vel[b], lastCmd_.bones[b].vel));
            const double prev = dvPrev_[k];
            dvPrev_[k] = dv;
            if (idv > 0.0 && prev >= 0.0 && dv > idv && dv - prev > idv && !state_.Ramping(nowMs, params_)) struck = true;
        }
        return struck;
    }

    ServoParams params_;
    bool useAcc_ = true;
    bool placed_ = false;
    AimBuilder aim_;
    ServoState state_;
    FighterCommand lastCmd_{};                       // the command handed out last frame (empty: none)
    std::array<double, kContactBones.size()> dvPrev_ = MinusOnes();   // last frame's offset per contact body; -1 = none

    static constexpr std::array<double, kContactBones.size()> MinusOnes() {
        std::array<double, kContactBones.size()> a{};
        for (double& v : a) v = -1.0;
        return a;
    }
};

// ---- pose pipe: owner frame -> EncodePose -> LinkSim -> DecodePose -> PosePlayer -------------------------------
struct PosePacket {
    std::array<uint8_t, kPoseFrameMaxBytes> bytes{};
    uint16_t size = 0;
};

struct PosePipeStats {
    uint64_t encoded = 0;        // frames handed to Send
    uint64_t decodeFailed = 0;   // delivered packets DecodePose rejected
    uint64_t pushed = 0;         // decoded frames the player accepted
    uint64_t rejected = 0;       // decoded frames the player refused (duplicate, late)
};

class PosePipe {
public:
    explicit PosePipe(const LinkConfig& c) : link_(c) { buf_.reserve(kPoseFrameMaxBytes); }

    // Encodes `f` and hands it to the link at local time nowMs. False if the link lost or refused it.
    bool Send(const PoseFrame& f, double nowMs) {
        EncodePoseInto(f, buf_);
        ++stats_.encoded;
        PosePacket p;
        p.size = static_cast<uint16_t>(std::min(buf_.size(), p.bytes.size()));
        std::copy(buf_.begin(), buf_.begin() + p.size, p.bytes.begin());
        return link_.Send(p, nowMs);
    }

    // Everything the link delivers by nowMs goes into the jitter buffer, received at nowMs.
    void Pump(double nowMs) {
        PosePacket p;
        while (link_.Receive(nowMs, p)) {
            PoseFrame f;
            if (!DecodePose(p.bytes.data(), p.size, f)) { ++stats_.decodeFailed; continue; }
            if (player_.Push(f, static_cast<int64_t>(std::llround(nowMs)))) ++stats_.pushed;
            else ++stats_.rejected;
        }
    }

    PosePlayer& Player() { return player_; }
    const LinkStats& Link() const { return link_.Stats(); }
    const PosePipeStats& Stats() const { return stats_; }

private:
    LinkSim<PosePacket, 256> link_;
    PosePlayer player_;
    PosePipeStats stats_{};
    std::vector<uint8_t> buf_;
};

}  // namespace hsm::sync

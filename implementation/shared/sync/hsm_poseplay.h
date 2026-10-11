// Receive-side playback of a remote fighter's pose stream: buffers PoseFrame packets as they
// arrive out of order or duplicated, tracks the sender-to-local clock offset and its jitter, sizes
// a playback delay from that jitter, and samples a continuous target pose a little behind "now" --
// Hermite interpolation of bone/weapon positions (tangents are the frames' own velocities) and
// slerp of rotations between the two frames bracketing the sample time, sender-velocity
// extrapolation (decaying to zero, capped) past the newest frame, a short blended correction when
// a late frame lands on a time already shown, and a hard cut (buffer reset) on a teleport-sized
// jump or a stream gap.
//
// Ported from Cyrex0/HalfSword-MP crates/hsmp-pose/src/poseplay.rs, Copyright (c) 2026 Cyrex0 and
// the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md).
//
// Skipped (HSMP-only, or made moot by always having exactly one PoseFrame stream with a velocity
// on every bone): root/capsule records; the v2::Context match/round/life tracking and its
// context-authenticated reset; the relay interval hint (set_interval_hint); the rate-slewed
// "stretch" playback clock (we clamp the playback point to never read backwards for a small error
// and hard-resync above kPlayResyncMs instead of slewing); the v1/mask dual-format bookkeeping and
// its finite-difference tangent fallback; acceleration-aware look-ahead refinement; and the
// Stats/DeliveredSample/explain_delivered/replay_tests diagnostics.
#pragma once
#include "sync/hsm_pose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace hsm::sync {

// ---- tunables, ported 1:1 from poseplay.rs (line numbers as of the ported revision) ----
inline constexpr double kPlayDelayStartMs = 60.0;         // Playback::new(), poseplay.rs:335
inline constexpr double kPlayDelayMinMs = 16.0;           // DELAY_MIN_V2_MS, poseplay.rs:70
inline constexpr double kPlayDelayMaxMs = 250.0;          // DELAY_MAX_MS, poseplay.rs:65
inline constexpr double kPlayDelayMarginMs = 6.0;         // DELAY_MARGIN_V2_MS, poseplay.rs:71
inline constexpr double kPlayExtrapFullMs = 34.0;         // EXTRAP_FULL_MS, poseplay.rs:73
inline constexpr double kPlayV2ExtrapOkMs = 17.0;         // V2_EXTRAP_OK_MS, poseplay.rs:79
inline constexpr double kPlayIntervalDelayCapMs = 66.0;   // V2_INTERVAL_DELAY_CAP_MS, poseplay.rs:81
inline constexpr double kPlaySenderFpsMaxIvMs = 55.0;     // V2_SENDER_FPS_MAX_IV_MS, poseplay.rs:83
inline constexpr double kPlayHermiteMaxGapMs = 270.0;     // HERMITE_MAX_GAP_V2_MS, poseplay.rs:86
inline constexpr double kPlayClockWindowMs = 2000.0;      // CLOCK_WINDOW_MS, poseplay.rs:88
inline constexpr double kPlayJitterWindowMs = 8000.0;     // JITTER_WINDOW_MS, poseplay.rs:90
inline constexpr double kPlayJitterQuantile = 0.95;       // JITTER_QUANTILE, poseplay.rs:93
inline constexpr double kPlayExtrapMs = 100.0;            // EXTRAP_MS, poseplay.rs:95
inline constexpr double kPlayExtrapMaxUu = 30.0;          // EXTRAP_MAX_UU, poseplay.rs:97
inline constexpr double kPlayStaleMs = 1000.0;            // STALE_MS, poseplay.rs:99
inline constexpr double kPlayCutDistUu = 300.0;           // CUT_DIST_UU, poseplay.rs:103
inline constexpr double kPlayCutSpeedUups = 3000.0;       // CUT_SPEED_UUPS, poseplay.rs:104
inline constexpr double kPlayGapCutMs = 600.0;            // GAP_CUT_MS, poseplay.rs:106
inline constexpr double kPlayResyncMs = 250.0;            // RESYNC_MS, poseplay.rs:110
inline constexpr double kPlayBlendTauMs = 100.0;          // BLEND_TAU_MS, poseplay.rs:125
inline constexpr double kPlayBlendMaxUu = 120.0;          // BLEND_MAX_UU, poseplay.rs:126
inline constexpr double kPlayLeadMaxMs = 120.0;           // LEAD_MAX_MS, poseplay.rs:128
inline constexpr double kPlayRestartBackMs = 2000.0;      // RESTART_BACK_MS, poseplay.rs:135
inline constexpr size_t kPlayKeepFrames = 48;             // KEEP_FRAMES, poseplay.rs:136

// What Sample() found, beyond the pose itself.
struct SampleInfo {
    bool extrapolated = false;    // past the newest buffered frame (decaying-velocity window, or the hold right after it)
    bool stale = false;           // newest frame older than kPlayStaleMs: holding, not driving
    bool cut = true;              // a teleport jump or a stream gap reset the buffer since the previous Sample() (also true for the first ever sample)
    double extrapolatedMs = 0.0;  // how far past the newest frame the sample point is (0 while interpolating)
    double delayMs = 0.0;         // current playback delay
    double j95Ms = 0.0;           // current jitter estimate (95th percentile of arrival lateness)
    uint32_t playTsMs = 0;        // sender-clock time the pose was evaluated at
};

namespace ppdetail {

// ---- small math helpers (world-space V3/Q4, no engine types) ----

inline V3 Add(const V3& a, const V3& b) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(const V3& a, const V3& b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Scale(const V3& a, double s) { return V3{ a.x * s, a.y * s, a.z * s }; }
inline double Len3(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline double Dist3(const V3& a, const V3& b) { return Len3(Sub(a, b)); }
inline V3 Lerp3(const V3& a, const V3& b, double s) { return Add(a, Scale(Sub(b, a), s)); }

// Hamilton product a*b (x, y, z, w). Same convention as hsm_servo.h detail::QMul.
inline Q4 QMul(const Q4& a, const Q4& b) {
    return Q4{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
               a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
               a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
               a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
inline Q4 QConj(const Q4& q) { return Q4{ -q.x, -q.y, -q.z, q.w }; }
inline Q4 QNorm(const Q4& q) {
    const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 1e-8)) return Q4{ 0.0, 0.0, 0.0, 1.0 };
    return Q4{ q.x / l, q.y / l, q.z / l, q.w / l };
}

// poseplay.rs nlerp(), poseplay.rs:625-634.
inline Q4 Nlerp(const Q4& a, const Q4& b, double t) {
    const double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const double s = dot < 0.0 ? -1.0 : 1.0;
    return QNorm(Q4{ a.x + (b.x * s - a.x) * t, a.y + (b.y * s - a.y) * t, a.z + (b.z * s - a.z) * t, a.w + (b.w * s - a.w) * t });
}

// poseplay.rs slerp(), poseplay.rs:639-652. Shortest arc; falls back to nlerp near-parallel.
inline Q4 Slerp(const Q4& a, const Q4& b, double t) {
    double dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    double sgn = 1.0;
    if (dot < 0.0) { dot = -dot; sgn = -1.0; }
    if (dot > 0.9995) return Nlerp(a, b, t);
    const double th = std::acos(std::min(dot, 1.0));
    const double st = std::sin(th);
    const double wa = std::sin((1.0 - t) * th) / st;
    const double wb = std::sin(t * th) / st * sgn;
    return QNorm(Q4{ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb });
}

// poseplay.rs hermite(), poseplay.rs:618-623. s in [0,1]; m0/m1 in units per ms; h = segment ms.
inline double Hermite(double p0, double m0, double p1, double m1, double s, double h) {
    const double s2 = s * s, s3 = s2 * s;
    return (2.0 * s3 - 3.0 * s2 + 1.0) * p0 + (s3 - 2.0 * s2 + s) * h * m0 + (-2.0 * s3 + 3.0 * s2) * p1 + (s3 - s2) * h * m1;
}
inline V3 HermiteV3(const V3& p0, const V3& m0, const V3& p1, const V3& m1, double s, double h) {
    return V3{ Hermite(p0.x, m0.x, p1.x, m1.x, s, h), Hermite(p0.y, m0.y, p1.y, m1.y, s, h), Hermite(p0.z, m0.z, p1.z, m1.z, s, h) };
}

// Integrate a world angular velocity (deg/s) over `seconds`, starting from q0. poseplay.rs:739-751.
inline Q4 IntegrateAngVel(const Q4& q0, const V3& angVelDeg, double seconds) {
    constexpr double kDeg2Rad = 3.14159265358979323846 / 180.0;
    const V3 w{ angVelDeg.x * kDeg2Rad * seconds, angVelDeg.y * kDeg2Rad * seconds, angVelDeg.z * kDeg2Rad * seconds };
    const double ang = Len3(w);
    if (!(ang > 1e-9)) return q0;
    const double h = std::sin(ang * 0.5) / ang;
    const Q4 dq{ w.x * h, w.y * h, w.z * h, std::cos(ang * 0.5) };
    return QNorm(QMul(dq, q0));
}

// ---- fixed-capacity, time-ordered ring: no heap allocation ----
template <typename T, size_t Cap>
class Ring {
public:
    void Clear() { size_ = 0; head_ = 0; }
    size_t Size() const { return size_; }
    bool Empty() const { return size_ == 0; }
    T& At(size_t i) { return buf_[(head_ + i) % Cap]; }
    const T& At(size_t i) const { return buf_[(head_ + i) % Cap]; }
    T& Front() { return At(0); }
    const T& Front() const { return At(0); }
    T& Back() { return At(size_ - 1); }
    const T& Back() const { return At(size_ - 1); }

    void PopFront() {
        if (size_ == 0) return;
        head_ = (head_ + 1) % Cap;
        --size_;
    }
    void PushBack(const T& v) {
        if (size_ < Cap) {
            buf_[(head_ + size_) % Cap] = v;
            ++size_;
        } else {
            buf_[head_] = v;
            head_ = (head_ + 1) % Cap;
        }
    }
    // Insert before logical index `pos` (0..=Size()).
    void InsertAt(size_t pos, const T& v) {
        if (size_ < Cap) {
            for (size_t i = size_; i > pos; --i) At(i) = At(i - 1);
            At(pos) = v;
            ++size_;
            return;
        }
        if (pos == 0) return;  // would be evicted immediately; matches the Rust source's insert-then-trim-front
        PopFront();
        --pos;
        for (size_t i = size_; i > pos; --i) At(i) = At(i - 1);
        At(pos) = v;
        ++size_;
    }

private:
    std::array<T, Cap> buf_{};
    size_t head_ = 0, size_ = 0;
};

// Sender->local clock mapping from arrival times. poseplay.rs Clock, poseplay.rs:254-285.
class ClockState {
public:
    // Fixed windows sized well beyond kPlayClockWindowMs / kPlayJitterWindowMs at any realistic
    // push rate; a pathological push rate (far above network/game rate) would evict samples
    // before their time-based window expiry, slightly front-loading the min/percentile.
    static constexpr size_t kOffsetCap = 512;
    static constexpr size_t kJitterCap = 1024;

    void Reset() { win_.Clear(); late_.Clear(); offset_.reset(); j95_ = 0.0; }
    bool HasOffset() const { return offset_.has_value(); }
    double Offset() const { return offset_.value_or(0.0); }
    double J95() const { return j95_; }

    // poseplay.rs Clock::observe(), poseplay.rs:264-283.
    void Observe(double rx, double ts) {
        win_.PushBack({ rx, rx - ts });
        while (win_.Size() > 8 && rx - win_.Front().first > kPlayClockWindowMs) win_.PopFront();
        double mn = std::numeric_limits<double>::infinity();
        for (size_t i = 0; i < win_.Size(); ++i) mn = std::min(mn, win_.At(i).second);
        offset_ = mn;
        late_.PushBack({ rx, rx - ts - mn });
        while (late_.Size() > 8 && rx - late_.Front().first > kPlayJitterWindowMs) late_.PopFront();
        const size_t n = late_.Size();
        std::array<double, kJitterCap> tmp{};
        for (size_t i = 0; i < n; ++i) tmp[i] = std::max(0.0, late_.At(i).second);
        size_t k = static_cast<size_t>(std::ceil(static_cast<double>(n) * kPlayJitterQuantile));
        k = std::clamp<size_t>(k, 1, n) - 1;
        std::nth_element(tmp.begin(), tmp.begin() + static_cast<long>(k), tmp.begin() + static_cast<long>(n));
        j95_ = tmp[k];
    }

private:
    Ring<std::pair<double, double>, kOffsetCap> win_;   // (rx, rx - ts)
    Ring<std::pair<double, double>, kJitterCap> late_;  // (rx, lateness above the offset at that moment)
    std::optional<double> offset_;
    double j95_ = 0.0;
};

enum class Mode { Interp, Extrap, Hold, Stale };

inline constexpr size_t kBoneSlots = static_cast<size_t>(kPoseBones);      // 23
inline constexpr size_t kWeaponSlots = static_cast<size_t>(kPoseWeapons);  // 2
inline constexpr size_t kTotalSlots = kBoneSlots + kWeaponSlots;           // 25 playback slots (blend_step operates on these)

// Flattened bone+weapon pose/velocity, used only by the blend step (poseplay.rs SLOTS, poseplay.rs:35).
struct SlotState {
    std::array<V3, kTotalSlots> pos{};
    std::array<Q4, kTotalSlots> rot{};
    std::array<V3, kTotalSlots> vel{};
    std::array<V3, kTotalSlots> angVel{};
    std::array<bool, kTotalSlots> present{};  // bones: always true; weapons: WeaponSample::present
};

inline void ExtractSlots(const PoseFrame& f, SlotState& s) {
    for (size_t i = 0; i < kBoneSlots; ++i) {
        s.pos[i] = f.bones[i].pos;
        s.rot[i] = f.bones[i].rot;
        s.vel[i] = f.bones[i].vel;
        s.angVel[i] = f.bones[i].angVelDeg;
        s.present[i] = true;
    }
    for (size_t w = 0; w < kWeaponSlots; ++w) {
        const size_t i = kBoneSlots + w;
        s.pos[i] = f.weapons[w].pos;
        s.rot[i] = f.weapons[w].rot;
        s.vel[i] = f.weapons[w].vel;
        s.angVel[i] = f.weapons[w].angVelDeg;
        s.present[i] = f.weapons[w].present;
    }
}
inline void WriteBackSlots(const SlotState& s, PoseFrame& f) {
    for (size_t i = 0; i < kBoneSlots; ++i) {
        f.bones[i].pos = s.pos[i];
        f.bones[i].rot = s.rot[i];
        f.bones[i].vel = s.vel[i];
        f.bones[i].angVelDeg = s.angVel[i];
    }
    for (size_t w = 0; w < kWeaponSlots; ++w) {
        const size_t i = kBoneSlots + w;
        f.weapons[w].pos = s.pos[i];
        f.weapons[w].rot = s.rot[i];
        f.weapons[w].vel = s.vel[i];
        f.weapons[w].angVelDeg = s.angVel[i];
    }
}

// Blend correction state. poseplay.rs Blend, poseplay.rs:289-301.
struct BlendState {
    bool valid = false;
    double at = 0.0, lead = 0.0, newestTs = 0.0;
    uint32_t cut = 0;
    bool prevInterp = false;
    std::array<bool, kTotalSlots> mask{};
    std::array<V3, kTotalSlots> shownPos{}, rawVelLin{}, rawVelAng{}, offPos{};
    std::array<Q4, kTotalSlots> shownRot{}, offRot{};
};

// One bone/weapon's sender-velocity extrapolation. poseplay.rs:716-760 (per-slot body), minus the
// acceleration-aware look-ahead refinement (see the top-of-file skip list).
inline void ExtrapolateSlot(const V3& p0, const Q4& q0, const V3& v0, const V3& w0, double dispTMs, double decay, double capUu,
                             V3& outPos, Q4& outRot, V3& outVel, V3& outAngVel) {
    const V3 d = Scale(v0, dispTMs / 1000.0);
    const double len = Len3(d);
    const double k = len > capUu ? capUu / len : 1.0;
    outPos = Add(p0, Scale(d, k));
    outRot = IntegrateAngVel(q0, w0, (dispTMs * k) / 1000.0);
    outVel = Scale(v0, decay * k);
    outAngVel = Scale(w0, decay * k);
}

// Past the newest buffered frame: full sender velocity for kPlayExtrapFullMs(+lead), then a linear
// ease-out to zero by kPlayExtrapMs(+lead), displacement capped. poseplay.rs sample_frames_lead(),
// poseplay.rs:694-763. `prev` only gates whether the newest-two-frame gap is trustworthy (and,
// per weapon, whether it was present in both) -- its pose data is otherwise unused (no accel term).
inline void ExtrapolateFrame(const PoseFrame& newest, const PoseFrame& prev, double dt, double leadMs, PoseFrame& out, Mode& mode) {
    const double ext = kPlayExtrapMs + leadMs;
    const double e = std::min(dt, ext);
    const double full = std::min(e, kPlayExtrapFullMs + leadMs);
    const double rest = std::max(e - full, 0.0);
    const double spanOut = std::max(ext - full, 1.0);
    const double capUu = kPlayExtrapMaxUu + 0.6 * leadMs;
    const double dispT = full + rest - (rest * rest) / (2.0 * spanOut);
    const double decay = rest > 0.0 ? std::max(1.0 - rest / spanOut, 0.0) : 1.0;
    mode = (dt <= ext) ? Mode::Extrap : Mode::Hold;

    out = newest;  // default: every field (incl. weapon present/hand/classHash/base/tip) verbatim from newest
    for (size_t i = 0; i < kBoneSlots; ++i) {
        ExtrapolateSlot(newest.bones[i].pos, newest.bones[i].rot, newest.bones[i].vel, newest.bones[i].angVelDeg, dispT, decay, capUu,
                         out.bones[i].pos, out.bones[i].rot, out.bones[i].vel, out.bones[i].angVelDeg);
    }
    for (size_t w = 0; w < kWeaponSlots; ++w) {
        if (newest.weapons[w].present && prev.weapons[w].present) {
            ExtrapolateSlot(newest.weapons[w].pos, newest.weapons[w].rot, newest.weapons[w].vel, newest.weapons[w].angVelDeg, dispT, decay,
                             capUu, out.weapons[w].pos, out.weapons[w].rot, out.weapons[w].vel, out.weapons[w].angVelDeg);
        }  // else left as newest's values verbatim (already copied via `out = newest`)
    }
}

// One bone/weapon's Hermite(pos)+slerp(rot)+lerp(vel) between two bracketing frames. poseplay.rs:778-807.
inline void InterpolateSlot(const V3& pA, const Q4& qA, const V3& vA, const V3& avA, const V3& pB, const Q4& qB, const V3& vB, const V3& avB,
                             double s, double h, V3& outPos, Q4& outRot, V3& outVel, V3& outAngVel) {
    outPos = (h <= kPlayHermiteMaxGapMs) ? HermiteV3(pA, Scale(vA, 1.0 / 1000.0), pB, Scale(vB, 1.0 / 1000.0), s, h) : Lerp3(pA, pB, s);
    outRot = Slerp(qA, qB, s);
    outVel = Lerp3(vA, vB, s);
    outAngVel = Lerp3(avA, avB, s);
}

// Segment a..b containing `at`. poseplay.rs:768-808. A weapon present in only one of the two
// frames is taken whole from the nearer frame (s < 0.5), matching how poseplay.rs's Extra (the
// weapon's own metadata) is chosen at the same spot (poseplay.rs:777).
inline void InterpolateFrame(const PoseFrame& a, const PoseFrame& b, double at, PoseFrame& out) {
    const double h = static_cast<double>(b.tsMs) - static_cast<double>(a.tsMs);
    const double s = h > 0.0 ? (at - static_cast<double>(a.tsMs)) / h : 0.0;
    out.stepUs = (s < 0.5) ? a.stepUs : b.stepUs;
    for (size_t i = 0; i < kBoneSlots; ++i) {
        InterpolateSlot(a.bones[i].pos, a.bones[i].rot, a.bones[i].vel, a.bones[i].angVelDeg, b.bones[i].pos, b.bones[i].rot, b.bones[i].vel,
                         b.bones[i].angVelDeg, s, h, out.bones[i].pos, out.bones[i].rot, out.bones[i].vel, out.bones[i].angVelDeg);
    }
    for (size_t w = 0; w < kWeaponSlots; ++w) {
        const WeaponSample& wa = a.weapons[w];
        const WeaponSample& wb = b.weapons[w];
        if (wa.present && wb.present) {
            InterpolateSlot(wa.pos, wa.rot, wa.vel, wa.angVelDeg, wb.pos, wb.rot, wb.vel, wb.angVelDeg, s, h, out.weapons[w].pos,
                             out.weapons[w].rot, out.weapons[w].vel, out.weapons[w].angVelDeg);
            out.weapons[w].present = true;
            out.weapons[w].hand = (s < 0.5) ? wa.hand : wb.hand;
            out.weapons[w].classHash = (s < 0.5) ? wa.classHash : wb.classHash;
            out.weapons[w].base = (s < 0.5) ? wa.base : wb.base;
            out.weapons[w].tip = (s < 0.5) ? wa.tip : wb.tip;
        } else {
            out.weapons[w] = (s < 0.5) ? wa : wb;  // taken from the nearer frame (neither present is fine too)
        }
    }
}

}  // namespace ppdetail

// Bounded-memory jitter buffer + sampler for one peer's PoseFrame stream. Pure: no engine types,
// no exceptions, no heap allocation (the frame ring is a fixed-capacity array member).
class PosePlayer {
public:
    PosePlayer() { Reset(); }

    // Buffers `f` (sender clock f.tsMs) as received at local time rxMs. Returns false for an exact
    // duplicate (a buffered frame with the same tsMs) or a frame at/before the already-sampled
    // sender time -- poseplay.rs Push::Duplicate / Push::Late (poseplay.rs:224). Everything else,
    // including an out-of-order insert and a teleport/gap that resets the buffer (cut), is
    // accepted. A sender clock that steps far backwards restarts the whole stream first.
    bool Push(const PoseFrame& f, int64_t rxMs) {
        const double rx = static_cast<double>(rxMs);
        const double ts = static_cast<double>(f.tsMs);

        if (!frames_.Empty() && ts < static_cast<double>(frames_.Back().tsMs) - kPlayRestartBackMs) {
            // poseplay.rs Playback::reset(), poseplay.rs:342-349 (sender restart; delay/cut untouched).
            frames_.Clear();
            clock_.Reset();
            havePt_ = false;
            blend_.valid = false;
        }
        for (size_t i = 0; i < frames_.Size(); ++i) {
            if (frames_.At(i).tsMs == f.tsMs) return false;  // Duplicate, poseplay.rs:423-426
        }
        clock_.Observe(rx, ts);  // poseplay.rs:429-430 -- observed even if this push turns out Late
        if (!frames_.Empty() && ts <= (havePt_ ? pt_ : -std::numeric_limits<double>::infinity())) {
            return false;  // Late, poseplay.rs:431-434
        }

        const double newest = frames_.Empty() ? -std::numeric_limits<double>::infinity() : static_cast<double>(frames_.Back().tsMs);
        if (ts > newest) {
            if (!frames_.Empty()) {
                const PoseFrame& p = frames_.Back();
                const double d = ppdetail::Dist3(p.bones[kBonePelvis].pos, f.bones[kBonePelvis].pos);
                const double dt = std::max((ts - static_cast<double>(p.tsMs)) / 1000.0, 1e-3);
                const bool cut = (d > kPlayCutDistUu && d / dt > kPlayCutSpeedUups) || (ts - static_cast<double>(p.tsMs) > kPlayGapCutMs);
                if (cut) {
                    // poseplay.rs:447-453: drop history, bump the cut counter, show the new place now.
                    frames_.Clear();
                    ++cutCounter_;
                    cutPending_ = true;
                    havePt_ = false;
                }
            }
            frames_.PushBack(f);
        } else {
            size_t pos = frames_.Size();
            for (size_t i = 0; i < frames_.Size(); ++i) {
                if (static_cast<double>(frames_.At(i).tsMs) > ts) { pos = i; break; }
            }
            frames_.InsertAt(pos, f);
        }
        UpdateDelay();
        return true;
    }

    // Advances the playback point to local time `nowMs` and samples the buffer `leadMs` (clamped
    // to [0, kPlayLeadMaxMs]) past it -- poseplay.rs sample_lead(), poseplay.rs:559-611. Returns
    // false if nothing has ever been pushed. `target` is written only on success.
    bool Sample(int64_t nowMs, PoseFrame& target, SampleInfo* info = nullptr, double leadMs = 0.0) {
        if (!clock_.HasOffset() || frames_.Empty()) return false;
        const double now = static_cast<double>(nowMs);
        const double offset = clock_.Offset();
        const double wantPt = now - offset - delay_;
        const double pt = (!havePt_ || std::fabs(wantPt - pt_) > kPlayResyncMs) ? wantPt : std::max(pt_, wantPt);
        pt_ = pt;
        havePt_ = true;

        const double lead = std::isfinite(leadMs) ? std::clamp(leadMs, 0.0, kPlayLeadMaxMs) : 0.0;
        const double at = pt + lead;
        const double senderNow = now - offset;

        PoseFrame raw{};
        ppdetail::Mode mode;
        double dtPastNewest = 0.0;
        SampleFramesInto(at, senderNow, lead, raw, mode, dtPastNewest);

        ppdetail::SlotState slots;
        ppdetail::ExtractSlots(raw, slots);
        BlendStep(slots, mode == ppdetail::Mode::Stale, mode == ppdetail::Mode::Interp, at, lead);
        ppdetail::WriteBackSlots(slots, raw);

        raw.tsMs = static_cast<uint32_t>(std::llround(std::clamp(at, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max()))));
        target = raw;
        if (info) {
            info->extrapolated = (mode == ppdetail::Mode::Extrap);
            info->stale = (mode == ppdetail::Mode::Stale);
            info->cut = cutPending_;
            info->extrapolatedMs = dtPastNewest;
            info->delayMs = delay_;
            info->j95Ms = clock_.J95();
            info->playTsMs = raw.tsMs;
        }
        cutPending_ = false;
        return true;
    }

    void Reset() {
        frames_.Clear();
        clock_.Reset();
        havePt_ = false;
        pt_ = 0.0;
        delay_ = kPlayDelayStartMs;
        cutCounter_ = 0;
        cutPending_ = true;  // nothing to blend from yet
        blend_ = ppdetail::BlendState{};
    }

    double delayMs() const { return delay_; }
    double j95Ms() const { return clock_.J95(); }
    bool hasOffset() const { return clock_.HasOffset(); }
    double offsetMs() const { return clock_.Offset(); }
    size_t size() const { return frames_.Size(); }

private:
    using FrameRing = ppdetail::Ring<PoseFrame, kPlayKeepFrames>;

    // poseplay.rs Playback::update_delay(), poseplay.rs:373-403, v2 branch only (every bone here
    // always has a velocity). No relay interval hint (skipped): Interval() always measures.
    void UpdateDelay() {
        const double iv = Interval();
        const double base = clock_.J95() + kPlayDelayMarginMs;
        double extra = std::max(0.0, iv - kPlayV2ExtrapOkMs);
        if (iv <= kPlaySenderFpsMaxIvMs) extra = std::min(extra, std::max(0.0, kPlayIntervalDelayCapMs - base));
        const double target = std::clamp(base + extra, kPlayDelayMinMs, kPlayDelayMaxMs);
        const double a = target > delay_ ? 0.3 : 0.02;  // grow fast, shrink slowly
        delay_ += (target - delay_) * a;
    }

    // Median spacing of the last up to 9 frames. poseplay.rs measured_interval(), poseplay.rs:354-362.
    double Interval() const {
        const size_t n = frames_.Size();
        if (n < 2) return 1000.0 / 60.0;
        const size_t from = n > 9 ? n - 9 : 0;
        std::array<double, 9> diffs{};
        size_t cnt = 0;
        for (size_t i = from; i + 1 < n && cnt < diffs.size(); ++i) {
            const double d = static_cast<double>(frames_.At(i + 1).tsMs) - static_cast<double>(frames_.At(i).tsMs);
            if (d > 0.0) diffs[cnt++] = d;
        }
        if (cnt == 0) return 1000.0 / 60.0;
        std::sort(diffs.begin(), diffs.begin() + static_cast<long>(cnt));
        return std::clamp(diffs[cnt / 2], 4.0, 200.0);
    }

    // poseplay.rs sample_frames_lead(), poseplay.rs:689-809 (dispatch to the four cases).
    void SampleFramesInto(double at, double senderNow, double leadMs, PoseFrame& out, ppdetail::Mode& mode, double& dtPastNewest) const {
        const size_t n = frames_.Size();
        const PoseFrame& newest = frames_.Back();
        const double newestTs = static_cast<double>(newest.tsMs);
        if (senderNow - newestTs > kPlayStaleMs) {
            out = newest;
            mode = ppdetail::Mode::Stale;
            return;
        }
        const PoseFrame& oldest = frames_.Front();
        if (at <= static_cast<double>(oldest.tsMs)) {
            out = oldest;
            mode = ppdetail::Mode::Hold;
            return;
        }
        if (at >= newestTs) {
            const double dt = at - newestTs;
            dtPastNewest = dt;
            if (n >= 2) {
                const PoseFrame& prev = frames_.At(n - 2);
                const double span = newestTs - static_cast<double>(prev.tsMs);
                if (span > 0.0 && span <= kPlayHermiteMaxGapMs) {
                    ppdetail::ExtrapolateFrame(newest, prev, dt, leadMs, out, mode);
                    return;
                }
            }
            // Single frame buffered, or the last two are too far apart to trust: hold, no motion.
            out = newest;
            for (auto& b : out.bones) { b.vel = V3{}; b.angVelDeg = V3{}; }
            for (auto& w : out.weapons) { w.vel = V3{}; w.angVelDeg = V3{}; }
            mode = ppdetail::Mode::Hold;
            return;
        }
        size_t k = 0;
        for (size_t jj = n - 1; jj >= 1; --jj) {
            const size_t j = jj - 1;
            if (static_cast<double>(frames_.At(j).tsMs) <= at) { k = j; break; }
        }
        ppdetail::InterpolateFrame(frames_.At(k), frames_.At(k + 1), at, out);
        mode = ppdetail::Mode::Interp;
    }

    // Correction blending: a late frame that changed the trajectory shown while extrapolating or
    // holding becomes a decaying offset instead of a pop. poseplay.rs blend_step(), poseplay.rs:508-557.
    void BlendStep(ppdetail::SlotState& s, bool isStale, bool isInterp, double at, double leadMs) {
        const double newestTs = static_cast<double>(frames_.Back().tsMs);
        const uint32_t cutNow = cutCounter_;
        const ppdetail::BlendState prev = blend_;
        blend_.valid = false;
        if (isStale) return;  // blend is dropped across a stale gap, poseplay.rs:512

        std::array<V3, ppdetail::kTotalSlots> off{};
        std::array<Q4, ppdetail::kTotalSlots> qoff{};
        qoff.fill(Q4{ 0.0, 0.0, 0.0, 1.0 });
        if (prev.valid && prev.cut == cutNow) {
            const double dt = std::max(at - prev.at, 0.0);
            const double k = std::exp(-dt / kPlayBlendTauMs);
            const bool jump = !prev.prevInterp && newestTs > prev.newestTs && (prev.at - prev.lead - prev.newestTs) > kPlayV2ExtrapOkMs;
            for (size_t i = 0; i < ppdetail::kTotalSlots; ++i) {
                if (!(s.present[i] && prev.mask[i])) continue;
                if (jump) {
                    const V3 d = ppdetail::Sub(ppdetail::Add(prev.shownPos[i], ppdetail::Scale(prev.rawVelLin[i], dt / 1000.0)), s.pos[i]);
                    if (ppdetail::Len3(d) > kPlayBlendMaxUu) continue;
                    off[i] = d;
                    qoff[i] = ppdetail::QNorm(ppdetail::QMul(prev.shownRot[i], ppdetail::QConj(s.rot[i])));
                } else {
                    off[i] = ppdetail::Scale(prev.offPos[i], k);
                    qoff[i] = ppdetail::Nlerp(Q4{ 0.0, 0.0, 0.0, 1.0 }, prev.offRot[i], k);
                }
            }
        }

        const std::array<V3, ppdetail::kTotalSlots> rawVelLin = s.vel;
        const std::array<V3, ppdetail::kTotalSlots> rawVelAng = s.angVel;
        for (size_t i = 0; i < ppdetail::kTotalSlots; ++i) {
            if (!s.present[i]) continue;
            s.pos[i] = ppdetail::Add(s.pos[i], off[i]);
            s.vel[i] = ppdetail::Sub(s.vel[i], ppdetail::Scale(off[i], 1000.0 / kPlayBlendTauMs));
            if (qoff[i].w < 0.999999) s.rot[i] = ppdetail::QNorm(ppdetail::QMul(qoff[i], s.rot[i]));
        }

        blend_.valid = true;
        blend_.at = at;
        blend_.lead = leadMs;
        blend_.newestTs = newestTs;
        blend_.cut = cutNow;
        blend_.prevInterp = isInterp;
        blend_.mask = s.present;
        blend_.shownPos = s.pos;
        blend_.shownRot = s.rot;
        blend_.rawVelLin = rawVelLin;
        blend_.rawVelAng = rawVelAng;
        blend_.offPos = off;
        blend_.offRot = qoff;
    }

    FrameRing frames_;
    ppdetail::ClockState clock_;
    double delay_ = kPlayDelayStartMs;
    uint32_t cutCounter_ = 0;
    bool cutPending_ = true;
    bool havePt_ = false;
    double pt_ = 0.0;
    ppdetail::BlendState blend_;
};

}  // namespace hsm::sync

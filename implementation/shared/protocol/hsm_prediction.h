// Client-side prediction bookkeeping for the local Willie: a fixed ring of predicted body poses keyed
// by input seq, and a reconciler that turns the host's acknowledged pose into a bounded velocity
// correction (or a hard snap). Pure logic; no engine or generated SDK types.
//
// Client session, per frame:
//   1. after the local step for input seq S:   hist.Push(S, bodies, n)     (bodies AFTER any correction)
//   2. per host snapshot (ackInputSeq A, host bodies H, n bodies), dt = seconds the velocity is applied over:
//        Reconciler::Correction c;
//        switch (rec.Evaluate(hist, A, H, n, dt, c)) {
//          case Mode::Correct: add c.velocity[i] to body i for dt (displacement = velocity * dt); break;
//          case Mode::Snap:    teleport bodies to H, then hist.Clear() and rec.Reset(); break;
//          case Mode::None:
//          case Mode::Stale:   break;
//        }
// Each ack is evaluated at most once: an ack not newer than the last evaluated one (or with no matching
// history entry, or a different body count) returns Stale without touching the reconciler's state.
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hsm {

constexpr float kPoseDeadzoneCm = 2.f;          // per-body error below this: no correction
constexpr float kPoseSnapCm = 150.f;            // per-body error at or above this: hard snap
constexpr float kReconcileGain = 0.15f;         // fraction of the error removed per correction
constexpr float kReconcileMaxSpeedCmS = 300.f;  // cap on the correction velocity (cm/s)

struct Vec3 { float x = 0, y = 0, z = 0; };

// One predicted pose (all bodies of the local Willie) and the input seq that produced it.
struct PoseEntry {
    uint32_t seq = 0;
    uint8_t count = 0;
    bool valid = false;
    std::array<BodyState, kMaxBodies> bodies{};
};

// Fixed ring indexed by seq & (Capacity - 1). Push never allocates. Lookup compares the stored seq
// exactly, so evicted, never-pushed and wrapped-around slots all return nullptr correctly.
// At 60 Hz input, the default 128 entries cover about 2 s of round-trip time.
template <size_t Capacity = 128>
class PoseHistory {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "PoseHistory capacity must be a power of two");
public:
    // Records the pose predicted for seq. Seqs must be strictly newer (wrap-aware) than the last push.
    bool Push(uint32_t seq, const BodyState* bodies, size_t count) {
        if (count > kMaxBodies || (count > 0 && bodies == nullptr)) return false;
        if (have_ && !SeqNewer(seq, newest_)) return false;
        PoseEntry& e = ring_[seq & (Capacity - 1)];
        e.seq = seq; e.count = uint8_t(count); e.valid = true;
        for (size_t i = 0; i < count; ++i) e.bodies[i] = bodies[i];
        newest_ = seq; have_ = true;
        return true;
    }
    // Returns the pose recorded for exactly seq, or nullptr if it was never pushed or has been evicted.
    const PoseEntry* Find(uint32_t seq) const {
        const PoseEntry& e = ring_[seq & (Capacity - 1)];
        return (e.valid && e.seq == seq) ? &e : nullptr;
    }
    bool empty() const { return !have_; }
    uint32_t newest() const { return newest_; }
    void Clear() { for (PoseEntry& e : ring_) e.valid = false; have_ = false; }
private:
    std::array<PoseEntry, Capacity> ring_{};
    uint32_t newest_ = 0;
    bool have_ = false;
};

struct ReconcileConfig {
    float deadzoneCm = kPoseDeadzoneCm;
    float snapCm = kPoseSnapCm;
    float gain = kReconcileGain;
    float maxSpeedCmS = kReconcileMaxSpeedCmS;
};

class Reconciler {
public:
    enum class Mode : uint8_t {
        Stale,   // no matching history for the ack, body count mismatch, or ack not newer than the last one
        None,    // every body within the deadzone: do nothing
        Correct, // apply Correction::velocity for dt
        Snap,    // some body at or beyond the snap threshold (or non-finite): teleport to the host pose
    };
    struct Correction {
        float maxErrorCm = 0;                       // largest per-body position error seen
        std::array<Vec3, kMaxBodies> velocity{};   // cm/s per body; zero unless mode == Correct
    };

    explicit Reconciler(ReconcileConfig cfg = {}) : cfg_(cfg) {}

    // predicted: history entry for the ack (nullptr when absent). host: the host's bodies for this Willie.
    // dt: seconds the returned velocity will be applied over (must be > 0).
    Mode Evaluate(uint32_t ackSeq, const PoseEntry* predicted, const BodyState* host, size_t hostCount,
                  float dt, Correction& out) {
        out = Correction{};
        if (!predicted || !predicted->valid || predicted->count != hostCount || hostCount > kMaxBodies) return Mode::Stale;
        if (hostCount > 0 && host == nullptr) return Mode::Stale;
        if (!(dt > 0.f) || !std::isfinite(dt)) return Mode::Stale;
        if (haveAck_ && !SeqNewer(ackSeq, lastAck_)) return Mode::Stale;
        lastAck_ = ackSeq; haveAck_ = true;

        // Pass 1: find the worst error and whether any body needs a snap.
        bool snap = false;
        for (size_t i = 0; i < hostCount; ++i) {
            float ex, ey, ez;
            Error(predicted->bodies[i], host[i], ex, ey, ez);
            if (!std::isfinite(ex) || !std::isfinite(ey) || !std::isfinite(ez)) { snap = true; continue; }
            float d = std::sqrt(ex * ex + ey * ey + ez * ez);
            out.maxErrorCm = std::max(out.maxErrorCm, d);
            if (d >= cfg_.snapCm) snap = true;
        }
        if (snap) { out.velocity = {}; return Mode::Snap; }

        // Pass 2: velocity = error * gain / dt, capped at maxSpeedCmS. Bodies inside the deadzone get zero.
        Mode mode = Mode::None;
        float k = cfg_.gain / dt;
        for (size_t i = 0; i < hostCount; ++i) {
            float ex, ey, ez;
            Error(predicted->bodies[i], host[i], ex, ey, ez);
            float d = std::sqrt(ex * ex + ey * ey + ez * ez);
            if (d < cfg_.deadzoneCm) continue;
            Vec3 v{ex * k, ey * k, ez * k};
            float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (speed > cfg_.maxSpeedCmS) { float s = cfg_.maxSpeedCmS / speed; v = {v.x * s, v.y * s, v.z * s}; }
            out.velocity[i] = v;
            mode = Mode::Correct;
        }
        return mode;
    }

    // Convenience overload: looks the ack up in a history ring.
    template <size_t C>
    Mode Evaluate(const PoseHistory<C>& hist, uint32_t ackSeq, const BodyState* host, size_t hostCount,
                  float dt, Correction& out) {
        return Evaluate(ackSeq, hist.Find(ackSeq), host, hostCount, dt, out);
    }

    // Forget the last evaluated ack (call after a snap or a level/entity change).
    void Reset() { haveAck_ = false; }

private:
    // Error is host minus predicted, so a positive correction moves the predicted body toward the host.
    static void Error(const BodyState& predicted, const BodyState& host, float& ex, float& ey, float& ez) {
        ex = host.px - predicted.px; ey = host.py - predicted.py; ez = host.pz - predicted.pz;
    }
    ReconcileConfig cfg_;
    uint32_t lastAck_ = 0;
    bool haveAck_ = false;
};

}  // namespace hsm

// Remote-body smoothing: a jitter buffer for host snapshots. Pure logic; no engine types.
//
// Client session, per remote Willie:
//   buf.Push(snap.tick, nowMs, willie)        // on each Snapshot; only the bodies are buffered
//   SampledPose p; buf.Sample(nowMs, p)       // once per rendered frame; apply p.bodies[0..count)
// Discrete Willie fields (health, flags, ...) are the caller's to take from its latest push.
//
// Timing: the host tick maps to host time as tick * snapshotPeriodMs. The local clock offset is
// tracked as the minimum of (recvMs - hostMs), relaxing slowly upward so a stalled host can recover.
// Playback renders at now - delay - offset, where delay adapts to smoothed interarrival jitter.
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hsm {

constexpr double kJitterInitialDelayMs = 100.0;   // delay until the first jitter estimate exists
constexpr double kJitterMinDelayMs = 50.0;
constexpr double kJitterMaxDelayMs = 250.0;
constexpr double kJitterMaxExtrapolateMs = 150.0; // extrapolate from the last velocity this long, then hold
constexpr double kJitterSmoothing = 1.0 / 16.0;   // RFC 3550 interarrival jitter gain
constexpr double kJitterMultiplier = 4.0;         // target delay = 2 snapshot periods + 4 * jitter
constexpr double kJitterOffsetRelax = 1.0 / 64.0; // upward drift of the clock offset per accepted snapshot

// A rendered body: float position and float unit quaternion (x, y, z, w).
struct SmoothBody { float px = 0, py = 0, pz = 0; float qx = 0, qy = 0, qz = 0, qw = 1; };

struct SampledPose {
    std::array<SmoothBody, kMaxBodies> bodies{};
    uint8_t count = 0;
    bool extrapolated = false;   // rendering past the newest snapshot
    bool capped = false;         // extrapolation reached kJitterMaxExtrapolateMs; pose is frozen from here
};

// Bounded ring of accepted snapshots, oldest first. Capacity 32 at 30 Hz is about one second.
template <size_t Capacity = 32>
class JitterBuffer {
public:
    explicit JitterBuffer(double snapshotPeriodMs = 1000.0 / 30.0)
        : periodMs_(snapshotPeriodMs > 0.0 && std::isfinite(snapshotPeriodMs) ? snapshotPeriodMs : 1000.0 / 30.0) {}

    // Accepts a snapshot whose tick is strictly newer (wrap-aware) than every buffered one. Duplicates and
    // older ticks return false. The bodies are copied; the Willie's other fields are not buffered.
    bool Push(uint32_t tick, int64_t recvMs, const WillieState& state) {
        if (state.bodies.size() > kMaxBodies) return false;
        int64_t u = int64_t(tick);
        double hostMs = double(u) * periodMs_;
        if (have_) {
            if (!SeqNewer(tick, newestTick_)) return false;
            u = newestTickU_ + int64_t(int32_t(tick - newestTick_));
            hostMs = double(u) * periodMs_;
            double prevHostMs = double(newestTickU_) * periodMs_;
            // RFC 3550-style interarrival jitter: arrival spacing minus host-time spacing.
            double d = (double(recvMs) - double(lastRecvMs_)) - (hostMs - prevHostMs);
            jitterMs_ += (std::fabs(d) - jitterMs_) * kJitterSmoothing;
            double cand = double(recvMs) - hostMs;
            if (cand < offsetMs_) offsetMs_ = cand;
            else offsetMs_ += (cand - offsetMs_) * kJitterOffsetRelax;
            delayMs_ = std::clamp(2.0 * periodMs_ + kJitterMultiplier * jitterMs_, kJitterMinDelayMs, kJitterMaxDelayMs);
        } else {
            offsetMs_ = double(recvMs) - hostMs;
            jitterMs_ = 0.0;
        }
        size_t idx;
        if (size_ < Capacity) { idx = (head_ + size_) % Capacity; ++size_; }
        else { idx = head_; head_ = (head_ + 1) % Capacity; }   // full: overwrite the oldest
        Frame& f = ring_[idx];
        f.tick = u; f.count = uint8_t(state.bodies.size());
        for (size_t i = 0; i < f.count; ++i) f.bodies[i] = state.bodies[i];
        newestTick_ = tick; newestTickU_ = u; lastRecvMs_ = recvMs; have_ = true;
        return true;
    }

    // Renders the pose at nowMs. Returns false only when the buffer is empty.
    bool Sample(int64_t nowMs, SampledPose& out) const {
        out = SampledPose{};
        if (size_ == 0) return false;
        double renderMs = double(nowMs) - delayMs_ - offsetMs_;
        const Frame& oldest = At(0);
        const Frame& newest = At(size_ - 1);
        double newestMs = double(newest.tick) * periodMs_;
        if (renderMs <= double(oldest.tick) * periodMs_) { Hold(oldest, out); return true; }
        if (renderMs >= newestMs) {
            Hold(newest, out);
            if (size_ >= 2) {
                const Frame& prev = At(size_ - 2);
                double spanMs = newestMs - double(prev.tick) * periodMs_;   // > 0: ticks are unique
                double aheadMs = renderMs - newestMs;
                out.extrapolated = true;
                out.capped = aheadMs > kJitterMaxExtrapolateMs;
                double dtMs = std::min(aheadMs, kJitterMaxExtrapolateMs);
                uint8_t n = std::min(newest.count, prev.count);
                for (size_t i = 0; i < n; ++i) {
                    const BodyState& a = prev.bodies[i];
                    const BodyState& b = newest.bodies[i];
                    out.bodies[i].px = float(b.px + (b.px - a.px) * dtMs / spanMs);
                    out.bodies[i].py = float(b.py + (b.py - a.py) * dtMs / spanMs);
                    out.bodies[i].pz = float(b.pz + (b.pz - a.pz) * dtMs / spanMs);
                }
            }
            return true;
        }
        // Bracketing pair: the newest frame at or before renderMs, and its successor.
        size_t i = size_ - 2;
        while (double(At(i).tick) * periodMs_ > renderMs) --i;
        const Frame& a = At(i);
        const Frame& b = At(i + 1);
        double aMs = double(a.tick) * periodMs_, bMs = double(b.tick) * periodMs_;
        float t = float((renderMs - aMs) / (bMs - aMs));
        if (a.count != b.count) { Hold(b, out); return true; }   // body list changed: no blend, take the newer
        out.count = b.count;
        for (size_t k = 0; k < b.count; ++k) out.bodies[k] = Blend(a.bodies[k], b.bodies[k], t);
        return true;
    }

    void Reset() { head_ = 0; size_ = 0; have_ = false; delayMs_ = kJitterInitialDelayMs; jitterMs_ = 0.0; }
    size_t size() const { return size_; }
    double delayMs() const { return delayMs_; }
    double jitterMs() const { return jitterMs_; }

private:
    struct Frame {
        int64_t tick = 0;                                  // unwrapped host tick
        uint8_t count = 0;
        std::array<BodyState, kMaxBodies> bodies{};
    };
    const Frame& At(size_t i) const { return ring_[(head_ + i) % Capacity]; }

    static SmoothBody Unpack(const BodyState& b) {
        SmoothBody s; s.px = b.px; s.py = b.py; s.pz = b.pz;
        UnpackQuat(b.rot, s.qx, s.qy, s.qz, s.qw);
        return s;
    }
    static void Hold(const Frame& f, SampledPose& out) {
        out.count = f.count;
        for (size_t i = 0; i < f.count; ++i) out.bodies[i] = Unpack(f.bodies[i]);
    }
    // Position lerp; quaternion nlerp along the shorter arc, renormalised.
    static SmoothBody Blend(const BodyState& a, const BodyState& b, float t) {
        SmoothBody sa = Unpack(a), sb = Unpack(b);
        SmoothBody r;
        r.px = sa.px + (sb.px - sa.px) * t; r.py = sa.py + (sb.py - sa.py) * t; r.pz = sa.pz + (sb.pz - sa.pz) * t;
        float dot = sa.qx * sb.qx + sa.qy * sb.qy + sa.qz * sb.qz + sa.qw * sb.qw;
        float s = dot < 0.f ? -1.f : 1.f;
        r.qx = sa.qx + (s * sb.qx - sa.qx) * t; r.qy = sa.qy + (s * sb.qy - sa.qy) * t;
        r.qz = sa.qz + (s * sb.qz - sa.qz) * t; r.qw = sa.qw + (s * sb.qw - sa.qw) * t;
        float len = std::sqrt(r.qx * r.qx + r.qy * r.qy + r.qz * r.qz + r.qw * r.qw);
        if (len > 1e-6f) { r.qx /= len; r.qy /= len; r.qz /= len; r.qw /= len; }
        else { r.qx = r.qy = r.qz = 0; r.qw = 1; }
        return r;
    }

    double periodMs_;
    std::array<Frame, Capacity> ring_{};
    size_t head_ = 0, size_ = 0;
    bool have_ = false;
    uint32_t newestTick_ = 0;
    int64_t newestTickU_ = 0;
    int64_t lastRecvMs_ = 0;
    double offsetMs_ = 0.0;
    double jitterMs_ = 0.0;
    double delayMs_ = kJitterInitialDelayMs;
};

}  // namespace hsm

// The owner's capsule (actor) root and yaw, kept beside the stand-in's pose buffer so the stand-in's capsule can be
// placed where the owner's capsule was at the same owner time the pose is played at (SampleInfo::playTsMs) --
// HSMP drives the root from the played frame's root and yaw (mods/HSMPAvatars/Scripts/main.lua drive_root, called
// with cur.root at main.lua:4634/4773; Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT, see
// THIRD_PARTY_NOTICES.md). Here the root travels in the Pose message rather than in the pose codec, so it is
// buffered separately: linear position, shortest-arc yaw between the two samples around the play time, the
// nearest end outside them (never extrapolated: the capsule is collision-free and only answers gameplay queries).
// Samples only ever append in owner time; a late or duplicate one is dropped, and one far enough behind the newest
// means the owner's clock restarted, so the track starts over (PosePlayer's kPlayRestartBackMs rule).
#pragma once
#include "sync/hsm_pose.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hsm::sync {

inline constexpr size_t kRootTrackCap = 64;            // ~1 s at 60 Hz: longer than the longest playback delay
inline constexpr double kRootRestartBackMs = 2000.0;   // = kPlayRestartBackMs (hsm_poseplay.h)

// Degrees wrapped to (-180, 180].
inline double WrapYawDeg(double d) {
    double w = std::fmod(d, 360.0);
    if (w > 180.0) w -= 360.0;
    else if (w <= -180.0) w += 360.0;
    return w;
}

class RootTrack {
public:
    // False (and nothing stored) for a non-finite sample, a duplicate time or a late one.
    bool Push(uint32_t tsMs, const V3& pos, double yawDeg) {
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) || !std::isfinite(yawDeg)) return false;
        if (size_ > 0) {
            const uint32_t newest = At_(size_ - 1).tsMs;
            if (double(tsMs) < double(newest) - kRootRestartBackMs) Reset();
            else if (tsMs <= newest) return false;
        }
        if (size_ == kRootTrackCap) { head_ = (head_ + 1) % kRootTrackCap; --size_; }
        Sample& s = buf_[(head_ + size_) % kRootTrackCap];
        s = Sample{ tsMs, pos, WrapYawDeg(yawDeg) };
        ++size_;
        return true;
    }

    // The root and yaw at owner time playTsMs; false (outputs untouched) while empty.
    bool At(uint32_t playTsMs, V3& pos, double& yawDeg) const {
        if (size_ == 0) return false;
        const Sample& first = At_(0);
        const Sample& last = At_(size_ - 1);
        if (playTsMs <= first.tsMs) { pos = first.pos; yawDeg = first.yawDeg; return true; }
        if (playTsMs >= last.tsMs) { pos = last.pos; yawDeg = last.yawDeg; return true; }
        size_t hi = 1;
        while (At_(hi).tsMs < playTsMs) ++hi;   // terminates: last.tsMs > playTsMs
        const Sample& a = At_(hi - 1);
        const Sample& b = At_(hi);
        const double t = double(playTsMs - a.tsMs) / double(b.tsMs - a.tsMs);
        pos = V3{ a.pos.x + (b.pos.x - a.pos.x) * t, a.pos.y + (b.pos.y - a.pos.y) * t, a.pos.z + (b.pos.z - a.pos.z) * t };
        yawDeg = WrapYawDeg(a.yawDeg + WrapYawDeg(b.yawDeg - a.yawDeg) * t);
        return true;
    }

    size_t Size() const { return size_; }
    void Reset() { head_ = 0; size_ = 0; }

private:
    struct Sample {
        uint32_t tsMs = 0;
        V3 pos{};
        double yawDeg = 0.0;
    };
    const Sample& At_(size_t i) const { return buf_[(head_ + i) % kRootTrackCap]; }

    std::array<Sample, kRootTrackCap> buf_{};
    size_t head_ = 0, size_ = 0;
};

}  // namespace hsm::sync

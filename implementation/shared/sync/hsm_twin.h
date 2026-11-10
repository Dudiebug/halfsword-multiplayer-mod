// Rev 42, the drift twin (competitive netcode Phase 0's key experiment and the Phase 4-5 prototype; docs/PLAN_COMPETITIVE_NETCODE.md
// 7). Pure logic, unit-tested.
//
// Input-driven stand-ins only pay off if two copies of one fighter, given the same inputs at the same time, stay close:
// Chaos is not deterministic, so they drift, and the pose stream has to pull the copy back. The twin bench measures
// exactly that on one machine: a second, live fighter beside the player is driven by the player's own input frames
// carried over a simulated link (sync/hsm_inputline.h), optionally corrected by the player's pose stream over the same
// link (sync/hsm_reconcile.h), and every frame its bodies are compared with the player's own, shifted by the spawn
// offset. Small errors with correction on: worth building online. Metres within a second: not without more delay.
#pragma once
#include "sync/hsm_pose.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace hsm::sync {

// One frame's comparison of the twin with the player (both read this frame), uu. -1 where a side lacks the body.
struct TwinDiff {
    double pelvis = -1, handL = -1, handR = -1, tip = -1;
};

inline double TwinDist(const V3& a, const V3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// player shifted by offset (where the twin was spawned relative to it) against the twin.
inline TwinDiff CompareTwin(const PoseFrame& player, const PoseFrame& twin, const V3& offset) {
    auto at = [&](const V3& p) { return V3{p.x + offset.x, p.y + offset.y, p.z + offset.z}; };
    TwinDiff d;
    d.pelvis = TwinDist(at(player.bones[size_t(kBonePelvis)].pos), twin.bones[size_t(kBonePelvis)].pos);
    d.handL = TwinDist(at(player.bones[size_t(kBoneHandL)].pos), twin.bones[size_t(kBoneHandL)].pos);
    d.handR = TwinDist(at(player.bones[size_t(kBoneHandR)].pos), twin.bones[size_t(kBoneHandR)].pos);
    for (int w = 0; w < kPoseWeapons; ++w) {   // the blade tip of the first weapon both hold in the same hand
        const WeaponSample& a = player.weapons[size_t(w)];
        const WeaponSample& b = twin.weapons[size_t(w)];
        if (a.present && b.present && a.hand == b.hand) { d.tip = TwinDist(at(a.tip), b.tip); break; }
    }
    return d;
}

// Running mean / max / p99 over one window (fixed bins of 1 uu up to 4 m, the rest in the last bin).
class TwinStat {
public:
    void Add(double v) {
        if (!(v >= 0) || !std::isfinite(v)) return;
        ++n_; sum_ += v; max_ = std::max(max_, v);
        ++bins_[std::min<size_t>(size_t(v), kBins - 1)];
    }
    size_t Count() const { return n_; }
    double Mean() const { return n_ ? sum_ / double(n_) : 0.0; }
    double Max() const { return max_; }
    double Quantile(double q) const {
        if (!n_) return 0.0;
        const size_t want = size_t(std::ceil(std::clamp(q, 0.0, 1.0) * double(n_)));
        size_t seen = 0;
        for (size_t i = 0; i < kBins; ++i) { seen += bins_[i]; if (seen >= want && want > 0) return double(i + 1); }
        return double(kBins);
    }
private:
    static constexpr size_t kBins = 400;
    size_t n_ = 0;
    double sum_ = 0, max_ = 0;
    std::array<uint32_t, kBins> bins_{};
};

struct TwinWindow {
    TwinStat pelvis, hands, tip;
    uint64_t frames = 0, predicted = 0, corrections = 0, snaps = 0;
    void Add(const TwinDiff& d) {
        ++frames;
        pelvis.Add(d.pelvis);
        hands.Add(d.handL);
        hands.Add(d.handR);
        tip.Add(d.tip);
    }
};

inline std::string TwinLine(const TwinWindow& w, const char* label) {
    char b[320];
    const double nf = w.frames ? double(w.frames) : 1.0;
    std::snprintf(b, sizeof b,
                  "twin %s: pelvis avg %.1f p99 %.0f max %.1f | hands avg %.1f p99 %.0f | tip avg %.1f p99 %.0f uu | predicted %.1f%% "
                  "| corrections %llu snaps %llu | frames %llu",
                  label, w.pelvis.Mean(), w.pelvis.Quantile(0.99), w.pelvis.Max(), w.hands.Mean(), w.hands.Quantile(0.99),
                  w.tip.Mean(), w.tip.Quantile(0.99), 100.0 * double(w.predicted) / nf,
                  static_cast<unsigned long long>(w.corrections), static_cast<unsigned long long>(w.snaps),
                  static_cast<unsigned long long>(w.frames));
    return b;
}

}  // namespace hsm::sync

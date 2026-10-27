// Name tags behind walls: when to trace from the camera to each fighter's head, and what the answers mean. A fighter is
// traced at most 10 times a second and a frame does at most a couple of traces, so a crowd spreads over the frames. A tag
// hides after two blocked traces in a row and shows again after one clear one (no flicker at a fence post), and its
// opacity eases rather than pops. Game-independent.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace hsm {

inline constexpr uint32_t kOcclusionIntervalMs = 100;   // per fighter: 10 Hz
inline constexpr int kOcclusionPerFrame = 2;            // traces a frame may do at most
inline constexpr int kOcclusionHideAfter = 2;           // consecutive blocked traces before the tag hides
inline constexpr float kOcclusionFadeSeconds = 0.2f;    // zero to full opacity (and back)
inline constexpr uint32_t kOcclusionForgetMs = 1000;    // a fighter nobody asked about for this long is dropped

class OcclusionTracker {
public:
    // Once at the start of every frame's name tag pass: resets the frame's trace budget, drops fighters gone for a while.
    void Begin(uint32_t nowMs) {
        now_ = nowMs; traces_ = 0;
        slots_.erase(std::remove_if(slots_.begin(), slots_.end(), [&](const Slot& s) { return nowMs - s.touchedMs > kOcclusionForgetMs; }),
                     slots_.end());
    }
    // True when this fighter should be traced now (and the trace is then counted against the frame's budget and the
    // fighter's next time). Call Report with the answer.
    bool Due(uint8_t entity) {
        Slot& s = Get(entity);
        s.touchedMs = now_;
        if (traces_ >= kOcclusionPerFrame || int32_t(now_ - s.nextMs) < 0) return false;
        ++traces_;
        s.nextMs = now_ + kOcclusionIntervalMs;
        return true;
    }
    // The answer of a trace: clear (nothing between the camera and the head) or blocked.
    void Report(uint8_t entity, bool clear) {
        Slot& s = Get(entity);
        if (clear) { s.blocked = 0; s.hidden = false; }
        else if (++s.blocked >= kOcclusionHideAfter) { s.blocked = kOcclusionHideAfter; s.hidden = true; }
    }
    // Opacity factor 0..1 for the tag this frame, easing towards shown or hidden; call once a frame per tag, after Due.
    // A fighter not yet traced is hidden, so a tag never flashes through a wall before its first trace.
    float Seen(uint8_t entity) {
        Slot& s = Get(entity);
        s.touchedMs = now_;
        const uint32_t dt = s.fadedMs == now_ ? 0 : std::min<uint32_t>(now_ - s.fadedMs, 250);   // one step a frame, a stall is capped
        s.fadedMs = now_;
        const float step = float(dt) / 1000.f / kOcclusionFadeSeconds;
        s.fade = s.hidden ? std::max(0.f, s.fade - step) : std::min(1.f, s.fade + step);
        return s.fade;
    }
    size_t Tracked() const { return slots_.size(); }

private:
    struct Slot {
        uint8_t entity = 0;
        bool hidden = true;       // until a trace says clear
        int blocked = 0;
        float fade = 0.f;
        uint32_t nextMs = 0, touchedMs = 0, fadedMs = 0;
    };
    Slot& Get(uint8_t entity) {
        for (Slot& s : slots_) if (s.entity == entity) return s;
        Slot s; s.entity = entity; s.nextMs = s.touchedMs = s.fadedMs = now_;
        slots_.push_back(s);
        return slots_.back();
    }
    std::vector<Slot> slots_;
    uint32_t now_ = 0;
    int traces_ = 0;
};

}  // namespace hsm

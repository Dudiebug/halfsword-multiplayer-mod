// Input bookkeeping shared by client (capture) and host (apply). Pure logic, unit-tested.
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <vector>

namespace hsm {

// Mouse axes are per-frame deltas and must be summed between sends; the rest are levels.
inline bool IsDeltaAxis(Axis a) { return a == Axis::TurnMouse || a == Axis::LookMouse; }
constexpr float kMaxDeltaPerTick = 4 * kAxisLimit;   // a few client frames per host frame; bounds a flood

class InputCapture {
public:
    void OnAxis(Axis a, float v) {
        if (!std::isfinite(v)) return;
        float& slot = axes_[size_t(a)];
        slot = IsDeltaAxis(a) ? Clamp(slot + v) : Clamp(v);
    }
    void OnAction(Action a, bool pressed) {
        uint32_t bit = 1u << size_t(a);
        buttons_ = pressed ? (buttons_ | bit) : (buttons_ & ~bit);
    }
    // Builds the next frame and resets accumulated deltas.
    InputFrame Take(uint32_t timeMs) {
        InputFrame f; f.seq = ++seq_; f.clientTimeMs = timeMs; f.axes = axes_; f.buttons = buttons_;
        for (size_t i = 0; i < kAxisCount; ++i) if (IsDeltaAxis(Axis(i))) axes_[i] = 0;
        return f;
    }
    uint32_t buttons() const { return buttons_; }
private:
    static float Clamp(float v) { return v > kAxisLimit ? kAxisLimit : v < -kAxisLimit ? -kAxisLimit : v; }
    std::array<float, kAxisCount> axes_{};
    uint32_t buttons_ = 0;
    uint32_t seq_ = 0;
};

class InputApplier {
public:
    struct Step {
        std::array<float, kAxisCount> axes{};   // values to feed this game frame
        std::vector<Action> pressed, released;  // edges to fire this game frame
    };
    // Accepts a received frame; returns false for stale/duplicate sequence numbers.
    bool Receive(const InputFrame& f) {
        if (have_ && !SeqNewer(f.seq, latest_.seq)) return false;
        if (have_) { uint32_t iv = f.clientTimeMs - latest_.clientTimeMs; if (iv >= 1 && iv <= 200) intervalMs_ = float(iv); }
        for (size_t i = 0; i < kAxisCount; ++i)
            if (IsDeltaAxis(Axis(i))) pendingDelta_[i] = std::clamp(pendingDelta_[i] + f.axes[i], -kMaxDeltaPerTick, kMaxDeltaPerTick);
        latest_ = f; have_ = true; return true;
    }
    // Called once per game frame for this player. With dtMs, mouse deltas are spread over the frames until the
    // sender's next frame (the game reads them as per-frame rates: one spike then zeros is a wrist flick, not a
    // swing); without it they are applied at once.
    Step Tick(float dtMs = 0) {
        Step s;
        if (!have_) return s;
        float portion = dtMs > 0 ? std::min(1.f, dtMs / intervalMs_) : 1.f;
        for (size_t i = 0; i < kAxisCount; ++i) {
            if (!IsDeltaAxis(Axis(i))) { s.axes[i] = latest_.axes[i]; continue; }
            s.axes[i] = portion >= 1.f ? pendingDelta_[i] : pendingDelta_[i] * portion;
            pendingDelta_[i] -= s.axes[i];
        }
        uint32_t changed = latest_.buttons ^ applied_;
        for (size_t i = 0; i < kActionCount; ++i) {
            uint32_t bit = 1u << i;
            if (!(changed & bit)) continue;
            (latest_.buttons & bit ? s.pressed : s.released).push_back(Action(i));
        }
        applied_ = latest_.buttons;
        return s;
    }
    // Releases everything that is held (disconnect / despawn), so the Willie does not keep gripping.
    Step ReleaseAll() {
        Step s;
        for (size_t i = 0; i < kActionCount; ++i) if (applied_ & (1u << i)) s.released.push_back(Action(i));
        applied_ = 0; latest_ = InputFrame{}; pendingDelta_.fill(0); have_ = false;
        return s;
    }
    uint32_t lastSeq() const { return latest_.seq; }
    const InputFrame& latest() const { return latest_; }   // control rotation of the newest frame
private:
    InputFrame latest_{};
    bool have_ = false;
    uint32_t applied_ = 0;
    std::array<float, kAxisCount> pendingDelta_{};
    float intervalMs_ = 1000.f / 60.f;   // sender's frame spacing, from its clientTimeMs
};

}  // namespace hsm

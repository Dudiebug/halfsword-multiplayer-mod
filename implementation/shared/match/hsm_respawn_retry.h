// Retry policy for the local player's own respawn (F1): a null body is tried again every kRetryMs, at most
// kMaxAttempts times in all, then given up on (the next round reset fixes it). Wrap-safe on the 32-bit ms clock.
#pragma once
#include <cstdint>

namespace hsm {

class RespawnRetry {
public:
    static constexpr uint32_t kRetryMs = 1000;
    static constexpr int kMaxAttempts = 5;
    // A respawn is wanted (idempotent while one is already being retried).
    void Begin() { if (!active_) { active_ = true; attempts_ = 0; nextAt_ = 0; due_ = true; } }
    void Reset() { active_ = false; attempts_ = 0; due_ = false; }
    bool Active() const { return active_; }
    // True when an attempt should be made now.
    bool Due(uint32_t nowMs) const { return active_ && (due_ || int32_t(nowMs - nextAt_) >= 0); }
    // An attempt was made and failed; returns true when more attempts remain.
    bool Failed(uint32_t nowMs) {
        if (!active_) return false;
        ++attempts_; due_ = false; nextAt_ = nowMs + kRetryMs;
        if (attempts_ >= kMaxAttempts) { active_ = false; return false; }
        return true;
    }
    int attempts() const { return attempts_; }
private:
    bool active_ = false, due_ = false;
    int attempts_ = 0;
    uint32_t nextAt_ = 0;
};

}  // namespace hsm

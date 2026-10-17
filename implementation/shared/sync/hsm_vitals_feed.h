// Owner condition/death ordering for one remote entity. No Unreal dependencies.
#pragma once
#include "protocol/hsm_owner.h"

namespace hsm::sync {
class VitalsFeed {
public:
    bool Push(const VitalsMsg& v) {
        if (haveClock_ && !SeqNewer(v.tsMs, clock_)) return false;
        clock_ = v.tsMs; haveClock_ = true;
        // After replacing a body, ignore death samples until an alive sample or reliable death confirmation.
        // A reliable death confirmation can still kill a new body before its first alive sample.
        if (awaitAlive_ && (v.flags & 1)) return false;
        awaitAlive_ = false;
        latest_ = v; have_ = true;
        dead_ = dead_ || (v.flags & 1);
        if (dead_) latest_.flags |= 1;
        return true;
    }
    void ConfirmDeath() { dead_ = true; awaitAlive_ = false; if (have_) latest_.flags |= 1; }
    void NewBody() { have_ = dead_ = false; awaitAlive_ = true; }
    bool Dead() const { return dead_; }
    const VitalsMsg* Latest() const { return have_ ? &latest_ : nullptr; }
private:
    VitalsMsg latest_{};
    uint32_t clock_ = 0;
    bool haveClock_ = false, have_ = false, dead_ = false, awaitAlive_ = false;
};
}  // namespace hsm::sync

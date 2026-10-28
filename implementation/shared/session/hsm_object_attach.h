// Attach, both ways (0.8.0-pre2, M0 fix). An object that was put on another tracked object (ObjectEvent Attached) is not
// streamed by its attacher any more, but BodyStates it sent just before can still be on the way: the unreliable channel and
// the reliable event are not ordered against each other, and a receiver that took such a state for "streamed again" would
// take the object off its parent (the pre1 rule: a fresh state un-attaches). The latch holds an attached object against
// that: from the Attached event, a BodyState from the streamer that was streaming then is ignored, until
//   - a Detached event for the whole object (body 0) or a Destroyed ends it (OnDetached), or
//   - the host grants the object to another holder, or at another level (OnGrant), or
//   - kAttachStaleMs have passed since the event on this machine. A state that arrives later than an in-flight state could
//     is a let-go streamed again by a sender that sends no Detached (pre1): the old rule applies again, so nothing sticks.
// The Attached message carries no time (rev 26 is fixed), so "older than the attach" is judged by arrival on this machine.
// Game-independent, unit-tested.
#pragma once
#include "session/hsm_lease.h"
#include <cstdint>
#include <map>

namespace hsm {

constexpr double kAttachStaleMs = 2000.0;       // states from the attacher this soon after the event are in flight from before it
constexpr size_t kMaxAttachLatches = 1024;

class AttachLatch {
public:
    // The Attached event for objId was replayed here. streamer/level: who streams it and at which lease now (the view's).
    void OnAttached(uint64_t objId, uint8_t streamer, LeaseLevel level, double nowMs) {
        if (latches_.size() >= kMaxAttachLatches && !latches_.count(objId)) return;   // full: the object is simply not held
        latches_[objId] = Latch{streamer, level, nowMs};
    }
    // The whole object let go of its parent (a Detached event on body 0), or is gone.
    void OnDetached(uint64_t objId) { latches_.erase(objId); }
    // A Grant for objId reached this machine: another holder or lease level than the attacher's ends the latch.
    void OnGrant(uint64_t objId, uint8_t streamer, LeaseLevel level) {
        auto it = latches_.find(objId);
        if (it != latches_.end() && (it->second.streamer != streamer || it->second.level != level)) latches_.erase(it);
    }
    // True when a BodyState of objId from lessee is to be ignored now (and not relayed, on the host).
    bool Blocks(uint64_t objId, uint8_t lessee, double nowMs) {
        auto it = latches_.find(objId);
        if (it == latches_.end()) return false;
        if (nowMs - it->second.atMs >= kAttachStaleMs) { latches_.erase(it); return false; }
        return lessee == it->second.streamer;
    }
    bool Latched(uint64_t objId) const { return latches_.count(objId) != 0; }
    size_t Size() const { return latches_.size(); }
    void Clear() { latches_.clear(); }

private:
    struct Latch { uint8_t streamer = kHostSlot; LeaseLevel level = LeaseLevel::Free; double atMs = 0.0; };
    std::map<uint64_t, Latch> latches_;
};

}  // namespace hsm

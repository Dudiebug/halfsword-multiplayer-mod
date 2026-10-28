// When a stand-in may be given paint or an armour change (the armour vertex-paint crash). The game's vertex-paint tasks read a
// fighter's skinned mesh on worker threads; an armour piece removed or put on under them frees what they read.
//   * An armour replay waits until the fighter's paint component has been idle (no tasks queued) for kPaintQuietMs without a
//     break, not just at one instant: every observation of "busy" restarts the clock.
//   * Blood and wound replays are dropped for kPaintBloodHoldMs after an armour change on that fighter (they start new tasks).
// Time is passed in (milliseconds, any monotonic clock). Game thread only.
#pragma once
#include <cstdint>
#include <map>

namespace hsm::combat {

constexpr double kPaintQuietMs = 250.0;
constexpr double kPaintBloodHoldMs = 1000.0;

class PaintGate {
public:
    // One look at the fighter's paint component: tasks queued (busy) or not.
    void Observe(uint8_t entity, bool busy, double nowMs) {
        Entry& e = e_[entity];
        if (busy) e.idleSince = kNone;
        else if (e.idleSince == kNone) e.idleSince = nowMs;
    }
    // Idle without a break for quietMs, as far as Observe has seen.
    bool Quiet(uint8_t entity, double nowMs, double quietMs = kPaintQuietMs) const {
        auto it = e_.find(entity);
        return it != e_.end() && it->second.idleSince != kNone && nowMs - it->second.idleSince >= quietMs;
    }
    void NoteArmour(uint8_t entity, double nowMs) { e_[entity].armourAt = nowMs; }
    // A blood or wound replay on this fighter should be dropped: an armour change within holdMs.
    bool BloodHeld(uint8_t entity, double nowMs, double holdMs = kPaintBloodHoldMs) const {
        auto it = e_.find(entity);
        return it != e_.end() && it->second.armourAt != kNone && nowMs - it->second.armourAt < holdMs;
    }
    void Forget(uint8_t entity) { e_.erase(entity); }

private:
    static constexpr double kNone = -1e300;
    struct Entry { double idleSince = kNone, armourAt = kNone; };
    std::map<uint8_t, Entry> e_;
};

}  // namespace hsm::combat

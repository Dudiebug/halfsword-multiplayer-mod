// Deferred destroy of an armour piece the fighter's vertex-paint tasks may still read (the armour vertex-paint crash, T5).
// The game's Remove Armor / Set Up Armor starts a GetAllVertexColorsOnly task per worn piece and its completion callback
// destroys the piece; blood and wound paint tasks queued on the same skinned mesh then read freed vertex data on a worker
// thread. A piece the mod could not destroy yet is parked here: hidden, out of the fighter's armour list, and released
//   * once no task has referenced it for kPaintDeferQuietMs without a break (every "still referenced" look restarts the
//     clock), or
//   * after kPaintDeferMaxMs whatever the tasks do (a stuck task must not keep the piece forever).
// At most kPaintDeferMax pieces wait at once; Add refuses past that and the caller lets the game destroy as before.
// Time is passed in (milliseconds, any monotonic clock). Game thread only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hsm::combat {

constexpr double kPaintDeferQuietMs = 100.0;
constexpr double kPaintDeferMaxMs = 3000.0;
constexpr size_t kPaintDeferMax = 16;

struct PaintRelease {
    uint64_t key = 0;
    double waitedMs = 0;
    bool timedOut = false;   // released by the time limit, not because the tasks were done
};

class PaintDefer {
public:
    // Parks key. False when it is already parked or the table is full.
    bool Add(uint64_t key, double nowMs) {
        if (Has(key) || e_.size() >= kPaintDeferMax) return false;
        e_.push_back({key, nowMs, kNone});
        return true;
    }
    bool Has(uint64_t key) const {
        for (const Entry& e : e_) if (e.key == key) return true;
        return false;
    }
    size_t Size() const { return e_.size(); }
    // One look at every parked piece: busy(key) says whether a task still references it. Appends those due to out,
    // in the order they were parked, and forgets them.
    template <class Busy>
    void Poll(double nowMs, Busy&& busy, std::vector<PaintRelease>& out) {
        std::vector<Entry> keep;
        for (Entry& e : e_) {
            if (busy(e.key)) e.freeSince = kNone;
            else if (e.freeSince == kNone) e.freeSince = nowMs;
            const bool free = e.freeSince != kNone && nowMs - e.freeSince >= kPaintDeferQuietMs;
            const bool late = nowMs - e.since >= kPaintDeferMaxMs;
            if (free || late) out.push_back({e.key, nowMs - e.since, !free});
            else keep.push_back(e);
        }
        e_.swap(keep);
    }
    void Drop(uint64_t key) {
        for (size_t i = 0; i < e_.size(); ++i) if (e_[i].key == key) { e_.erase(e_.begin() + long(i)); return; }
    }
    void Clear() { e_.clear(); }

private:
    static constexpr double kNone = -1e300;
    struct Entry { uint64_t key; double since; double freeSince; };
    std::vector<Entry> e_;
};

}  // namespace hsm::combat

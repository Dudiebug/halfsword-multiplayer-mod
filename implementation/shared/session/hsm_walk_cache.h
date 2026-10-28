// Cost of the level walk (0.8.0-pre2, M0 fix). The walk (ObjectSync::WalkLevel) asks every movable primitive component of
// every candidate actor "do you simulate physics?" on each census, a reflected call apiece. A component's answer changes
// rarely (a plank freed by the game, a prop switched off), so it is kept per component and re-asked only after a while, and the
// walk makes at most kWalkQueriesPerPass fresh asks. How long an answer is kept depends on how much it matters:
//   a component of an object already tracked and asleep (at rest)    kSimKnownAsleepMs: it has simulated, it is not moving
//   any other component whose last answer was "no"                    kSimNoMs: a static movable (most of a level)
//   any other component whose last answer was "yes"                   kSimYesMs: short, new objects must be seen promptly
// An entry is for one (pointer, object-array index) pair: a pointer the engine reuses for another object has another index.
// Game-independent, unit-tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace hsm {

constexpr double kSimKnownAsleepMs = 30000.0;
constexpr double kSimNoMs = 8000.0;
constexpr double kSimYesMs = 2000.0;
constexpr double kSimKeepMs = 60000.0;           // an entry not refreshed for this long is dropped by Prune
constexpr size_t kWalkQueriesPerPass = 600;      // fresh IsSimulatingPhysics asks one walk makes at most
constexpr size_t kMaxSimCacheEntries = 16384;

inline double SimTtlMs(bool trackedAsleep, bool lastSim) {
    if (trackedAsleep) return kSimKnownAsleepMs;
    return lastSim ? kSimYesMs : kSimNoMs;
}

class SimCache {
public:
    // True with the cached answer when there is one for this component that is still good for the actor it belongs to.
    bool Lookup(uint64_t key, int32_t index, bool trackedAsleep, double nowMs, bool& sim) const {
        auto it = map_.find(key);
        if (it == map_.end() || it->second.index != index) return false;
        if (nowMs - it->second.atMs >= SimTtlMs(trackedAsleep, it->second.sim)) return false;
        sim = it->second.sim;
        return true;
    }
    // The last answer whatever its age (a walk out of fresh asks uses it rather than assume "no").
    bool Stale(uint64_t key, int32_t index, bool& sim) const {
        auto it = map_.find(key);
        if (it == map_.end() || it->second.index != index) return false;
        sim = it->second.sim;
        return true;
    }
    void Store(uint64_t key, int32_t index, bool sim, double nowMs) {
        if (map_.size() >= kMaxSimCacheEntries && !map_.count(key)) return;
        map_[key] = Entry{index, sim, nowMs};
    }
    // Drop entries older than kSimKeepMs; returns how many went.
    size_t Prune(double nowMs) {
        size_t n = 0;
        for (auto it = map_.begin(); it != map_.end();) {
            if (nowMs - it->second.atMs >= kSimKeepMs) { it = map_.erase(it); ++n; } else ++it;
        }
        return n;
    }
    size_t Size() const { return map_.size(); }
    void Clear() { map_.clear(); }

private:
    struct Entry { int32_t index = -1; bool sim = false; double atMs = 0.0; };
    std::unordered_map<uint64_t, Entry> map_;
};

}  // namespace hsm

// A sheathed weapon that sits far from its socket (the replica's slot watchdog, #248), and the log line about it.
//
// Under host sync a client's own fighter is a stand-in whose Mesh the rig moves rigidly, leaving the game's own sheath
// joints behind. StandInSlots points the joints at the Mesh, but a weapon can still end up metres from its socket (a
// joint that was not retargeted, a body the game re-spawned, a teleport it did not follow), and the old one-shot snap
// only fired when a joint was retargeted and the weapon was more than 15 cm out. The watchdog looks at every anchored
// slot weapon every kSlotWatchMs and asks this class, per weapon, what to do with the distance it read:
//   snap    the weapon has been farther than kSlotDriftCm from its socket on at least two reads, kSlotDriftMs apart or
//           more: put it back. Not again within kSlotSnapGapMs, and after kSlotMaxSnaps snaps in a row without the
//           weapon ever coming near (the game keeps dragging it away) it is left alone for kSlotGiveUpMs, so the
//           watchdog cannot fight the physics every few frames.
//   report  the weapon is farther than kSlotFarReportCm: one log line per weapon per kSlotReportEveryMs, so a tester's
//           log says which weapon was where without being flooded.
// A weapon that was not read for kSlotStaleMs starts over (a re-spawned weapon never inherits the old one's streak).
// Pure, no engine headers.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hsm::combat {

constexpr double kSlotDriftCm = 40.0;          // farther than this from its socket is adrift (the game's slack is 5 cm)
constexpr double kSlotDriftMs = 250.0;         // ... for at least this long
constexpr double kSlotWatchMs = 125.0;         // the watchdog reads this often
constexpr double kSlotSnapGapMs = 1000.0;      // a weapon is snapped again no sooner
constexpr int kSlotMaxSnaps = 6;               // snaps in a row (the weapon never came near) before giving up for a while
constexpr double kSlotGiveUpMs = 10000.0;
constexpr double kSlotFarReportCm = 100.0;     // the log line starts here
constexpr double kSlotReportEveryMs = 10000.0;
constexpr double kSlotStaleMs = 2000.0;        // no read for this long: the streak starts over
constexpr size_t kSlotDriftMemory = 32;        // weapons tracked

struct SlotDriftVerdict {
    bool snap = false;     // put the weapon back at its socket now
    bool report = false;   // write the "far from its socket" line now
    int snaps = 0;         // snaps in a row so far, this one included (to log only the first few)
};

class SlotDrift {
public:
    // One read: key names the weapon, cm is its distance from its socket (negative: could not be read, ignored).
    SlotDriftVerdict Observe(uint64_t key, double cm, double nowMs) {
        SlotDriftVerdict v;
        if (!(cm >= 0.0)) return v;   // also a NaN
        Entry& e = Find(key);
        if (nowMs - e.lastReadMs > kSlotStaleMs) { e.farSince = -1.0; e.farReads = 0; }
        e.lastReadMs = nowMs;
        if (cm > kSlotFarReportCm && nowMs - e.lastReportMs >= kSlotReportEveryMs) {
            e.lastReportMs = nowMs;
            v.report = true;
        }
        if (cm <= kSlotDriftCm) {   // home: the streak and the give-up are over
            e.farSince = -1.0;
            e.farReads = 0;
            e.snaps = 0;
            e.giveUpUntilMs = -1e18;
            return v;
        }
        if (e.farSince < 0.0) { e.farSince = nowMs; e.farReads = 0; }
        ++e.farReads;
        if (nowMs < e.giveUpUntilMs) return v;
        if (e.farReads >= 2 && nowMs - e.farSince >= kSlotDriftMs && nowMs - e.lastSnapMs >= kSlotSnapGapMs) {
            e.lastSnapMs = nowMs;
            e.farSince = -1.0;   // the next streak is measured from after the snap
            e.farReads = 0;
            v.snap = true;
            v.snaps = ++e.snaps;
            if (e.snaps >= kSlotMaxSnaps) { e.giveUpUntilMs = nowMs + kSlotGiveUpMs; e.snaps = 0; }
        }
        return v;
    }

    void Forget(uint64_t key) {
        for (size_t i = 0; i < e_.size(); ++i)
            if (e_[i].key == key) { e_.erase(e_.begin() + static_cast<std::ptrdiff_t>(i)); return; }
    }
    void Clear() { e_.clear(); }
    size_t Size() const { return e_.size(); }

private:
    struct Entry {
        uint64_t key = 0;
        double farSince = -1.0;        // the streak's first far read
        int farReads = 0;
        double lastReadMs = -1e18;
        double lastSnapMs = -1e18;
        double lastReportMs = -1e18;
        double giveUpUntilMs = -1e18;
        int snaps = 0;
    };
    Entry& Find(uint64_t key) {
        for (Entry& e : e_)
            if (e.key == key) return e;
        if (e_.size() >= kSlotDriftMemory) e_.erase(e_.begin());
        e_.push_back(Entry{});
        e_.back().key = key;
        return e_.back();
    }
    std::vector<Entry> e_;
};

}  // namespace hsm::combat

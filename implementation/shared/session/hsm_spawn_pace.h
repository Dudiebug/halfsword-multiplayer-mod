// Fighter spawns kept apart on one machine (crash family J, exe+0x21E5CA4), pure part.
// J is an engine race in the streaming of a fighter's groom bulk data (the IO cache reads one block past the end of
// the default hair's .ubulk). 49 of 65 events had a fighter spawn in the 2 s before; it came a median 38 ms after the
// newest of two or three spawns a median 24 ms apart: the mod's match-start burst (the own respawn, then a stand-in a
// frame), each spawn binding the same groom, in 37 of 65 within 20 ms of an old fighter's groom being destroyed. SpawnPace holds the next stand-in spawn until
// gap ms after the last fighter spawn, so one spawn's streaming is not under way when the next starts.
#pragma once
#include <cstdint>

namespace hsm {

constexpr uint32_t kSpawnGapMsDefault = 120;   // spawn_gap_ms: the J faults came a median 38 ms, p90 about 100 ms, after a spawn
constexpr uint32_t kSpawnGapMsMax = 500;

class SpawnPace {
public:
    // 0: no gap (one spawn a frame, as in 0.8.2-beta1); more than kSpawnGapMsMax counts as kSpawnGapMsMax.
    void SetGap(uint32_t ms) { gap_ = ms > kSpawnGapMsMax ? kSpawnGapMsMax : ms; }
    uint32_t gap() const { return gap_; }
    // May a held spawn go now? Always with no gap or before the first spawn. The difference is signed: a now taken
    // before a later Note (the host's RespawnAll batch noted in the same tick) is not ready; the clock's wrap still is.
    bool Ready(uint32_t nowMs) const { return gap_ == 0 || !any_ || int32_t(nowMs - last_) >= int32_t(gap_); }
    // A fighter was spawned on this machine (held or not: the own respawn is never held, but the next waits for it).
    void Note(uint32_t nowMs) { any_ = true; last_ = nowMs; }

private:
    uint32_t gap_ = kSpawnGapMsDefault, last_ = 0;
    bool any_ = false;
};

}  // namespace hsm

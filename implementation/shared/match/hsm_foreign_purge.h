// When a client removes the fighters that exist only in its own copy of the level (#2754).
//
// Every machine runs the level's start-up, so every machine's own copy gets the level's stock foes. The host's world is the
// only one that counts: a client keeps its own fighter and the stand-ins of the host's fighters, and nothing else. The old
// rule looked once a second, and only while a match was running, so a stock foe showed for up to a second after the level
// opened, and for as long as the lobby stayed between matches. Now, with `early` on:
//   - every frame for the first seconds after the level's start-up is over (the foes appear in that window), and
//   - once a second after that, match or no match.
// With `early` off, the old rule: once a second, only while a match runs.
// Times are milliseconds since the start-up was over / since the last purge (the caller takes the differences, so the
// 32-bit clock's wrap is the caller's to handle).
#pragma once
#include <cstdint>

namespace hsm {

struct ForeignPurgeTiming {
    uint32_t burstMs = 6000;     // every frame for this long after the level's start-up
    uint32_t steadyMs = 1000;    // then no more often than this
};

inline bool ForeignPurgeDue(bool early, bool matchRunning, uint32_t sinceStartUpMs, uint32_t sinceLastPurgeMs,
                            const ForeignPurgeTiming& t = ForeignPurgeTiming{}) {
    if (!early && !matchRunning) return false;
    if (early && sinceStartUpMs < t.burstMs) return true;
    return sinceLastPurgeMs >= t.steadyMs;
}

}  // namespace hsm

// The host's watchdog over the bots of a wave. A wave ends only when every bot is dead, so one bot that spawned before its
// floor tile existed (it fell for ever), or that is wedged somewhere it cannot leave, stopped the wave for good (#2413).
//
// Two rules, both conservative (a fight lasts seconds, a wave minutes):
//   BotWatch   one bot: it is below the floor when it is far under the height it spawned at, and stuck when it has not
//              moved from one spot for a long time with no living player close by (a bot beside a player is fighting, or
//              waiting to; the wave rule covers it if it never comes to anything).
//   WaveStalled the whole wave: nothing has spawned or died for a long time while bots are alive.
// The DLL kills what the verdict names (the normal death path, so the match scores it as the bots' own death, no killer).
// Times are the 32-bit millisecond clock, wrap-safe.
#pragma once
#include <cmath>
#include <cstdint>
#include <map>

namespace hsm {

struct BotWatchLimits {
    double belowCm = 2000;            // this far under its spawn height: it fell through the world
    double stillCm = 40;              // closer than this to its mark counts as not having moved
    uint32_t stillMs = 90000;         // not moved for this long
    double engagedCm = 600;           // a living player this close: the bot is in a fight, never stuck
    uint32_t waveStallMs = 300000;    // a wave with bots alive where nothing spawned or died for this long
};

enum class BotVerdict : uint8_t { Ok, BelowFloor, Still };

class BotWatch {
public:
    // The bot spawned at height `spawnZ` (the DLL calls this when it announces the bot). Optional: the first Observe of
    // an unknown bot takes its own height instead.
    void Arm(uint8_t entity, double spawnZ, uint32_t nowMs) {
        Mark& m = marks_[entity];
        m = Mark{};
        m.spawnZ = spawnZ; m.armed = true; m.at = nowMs;
    }
    // One look at a bot at (x, y, z). `nearestPlayerCm` is the distance to the closest living player, negative when none
    // is known.
    BotVerdict Observe(uint8_t entity, double x, double y, double z, double nearestPlayerCm, uint32_t nowMs,
                       const BotWatchLimits& lim = BotWatchLimits{}) {
        Mark& m = marks_[entity];
        if (!m.armed) { m.armed = true; m.spawnZ = z; }
        if (z < m.spawnZ - lim.belowCm) return BotVerdict::BelowFloor;
        if (!m.placed) { m.placed = true; m.x = x; m.y = y; m.z = z; m.at = nowMs; return BotVerdict::Ok; }
        const double dx = x - m.x, dy = y - m.y, dz = z - m.z;
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > lim.stillCm) { m.x = x; m.y = y; m.z = z; m.at = nowMs; return BotVerdict::Ok; }
        if (nearestPlayerCm >= 0 && nearestPlayerCm <= lim.engagedCm) { m.at = nowMs; return BotVerdict::Ok; }
        return uint32_t(nowMs - m.at) >= lim.stillMs ? BotVerdict::Still : BotVerdict::Ok;
    }
    void Forget(uint8_t entity) { marks_.erase(entity); }
    void Clear() { marks_.clear(); }
    size_t Size() const { return marks_.size(); }

private:
    struct Mark {
        bool armed = false, placed = false;
        double spawnZ = 0, x = 0, y = 0, z = 0;
        uint32_t at = 0;              // when the bot arrived at its mark
    };
    std::map<uint8_t, Mark> marks_;
};

// `lastProgressMs`: the wave's start, its latest spawn or its latest bot death. A wave with no bot alive is not stalled
// (it is between waves, or about to end).
inline bool WaveStalled(uint32_t lastProgressMs, uint32_t nowMs, int aliveBots, const BotWatchLimits& lim = BotWatchLimits{}) {
    return aliveBots > 0 && uint32_t(nowMs - lastProgressMs) >= lim.waveStallMs;
}

}  // namespace hsm

// When a fighter the mod destroys is really destroyed (Adapter::DestroyFighter, TickDeferredDestroys), pure part.
// Two things can still be reading a fighter the mod is about to destroy:
// - the vertex-paint plugin's queued task on its mesh (0.8.0-pre7, #72): wait until the plugin is idle;
// - Niagara's hair-strands data interface on a worker thread (crash family 2, exe+0x4DF1614): the game turns the hair
//   simulation off and destroys the groom later, but the mod turned it off and destroyed the groom and the fighter in the
//   same call, so a tick already under way read the freed groom. With hair_settle the simulation goes off at once and
//   the fighter (hidden, no collision) is destroyed kHairSettleTicks frames later.
// Either way the fighter is gone by kParkMaxMs.
#pragma once
#include <cstdint>

namespace hsm {

constexpr int kHairSettleTicks = 4;     // frames between the hair simulation going off and the destroy
constexpr double kParkMaxMs = 3000;     // a parked fighter is destroyed by then whatever the paint plugin says

// At DestroyFighter: park the fighter (true) or destroy it now (false). hasHair: it has a live groom component.
inline bool ParkOnDestroy(bool hairSettle, bool hasHair, bool paintQueued) {
    return paintQueued || (hairSettle && hasHair);
}

enum class ParkStep : uint8_t {
    Wait,        // keep it parked this frame
    Done,        // destroy it now: the paint plugin is idle and the hair has settled
    TimedOut,    // destroy it now: kParkMaxMs has passed
};

// Once a frame per parked fighter. ticks: frames since it was parked (1 on the first call); waitedMs: time since then.
// paintQueued is only asked for once the hair has settled (it is a call into the game).
template <class PaintQueued>
ParkStep ParkTick(bool hairSettle, bool hasHair, int ticks, double waitedMs, PaintQueued&& paintQueued) {
    if (waitedMs >= kParkMaxMs) return ParkStep::TimedOut;
    if (hairSettle && hasHair && ticks < kHairSettleTicks) return ParkStep::Wait;
    return paintQueued() ? ParkStep::Wait : ParkStep::Done;
}

}  // namespace hsm

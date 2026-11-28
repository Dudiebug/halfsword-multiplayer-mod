// When a fighter the mod destroys is really destroyed (session/hsm_fighter_park.h).
#include "session/hsm_fighter_park.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using P = ParkStep;

int main() {
    int asked = 0;
    auto idle = [&] { ++asked; return false; };
    auto busy = [&] { ++asked; return true; };

    // Crash family 2: the hair simulation went off and the groom was destroyed in the same call. With hair_settle a
    // fighter with hair is parked, and destroyed only after kHairSettleTicks frames, the paint plugin idle.
    {
        CHECK(ParkOnDestroy(true, true, false));
        asked = 0;
        for (int t = 1; t < kHairSettleTicks; ++t) CHECK(ParkTick(true, true, t, t * 16.0, idle) == P::Wait);
        CHECK(asked == 0);                                                           // the paint plugin is not asked yet
        CHECK(ParkTick(true, true, kHairSettleTicks, kHairSettleTicks * 16.0, idle) == P::Done);
        CHECK(asked == 1);
        CHECK(ParkTick(true, true, kHairSettleTicks + 1, 100, idle) == P::Done);
    }
    // Settled hair, paint still queued: it waits for the plugin as before.
    {
        CHECK(ParkTick(true, true, kHairSettleTicks, 70, busy) == P::Wait);
        CHECK(ParkTick(true, true, 100, kParkMaxMs - 1, busy) == P::Wait);
        CHECK(ParkTick(true, true, 101, kParkMaxMs, busy) == P::TimedOut);
    }
    // hair_settle=0: as in 0.8.2-beta1. Destroyed at once unless the paint plugin has a task queued.
    {
        CHECK(!ParkOnDestroy(false, true, false));
        CHECK(ParkOnDestroy(false, true, true));
        CHECK(ParkTick(false, true, 1, 16, idle) == P::Done);
        CHECK(ParkTick(false, true, 1, 16, busy) == P::Wait);
    }
    // No hair (a bald fighter, or the stand-in gore already destroyed it): no wait for it.
    {
        CHECK(!ParkOnDestroy(true, false, false));
        CHECK(ParkOnDestroy(true, false, true));
        CHECK(ParkTick(true, false, 1, 16, idle) == P::Done);
        CHECK(ParkTick(true, false, 1, 16, busy) == P::Wait);
    }
    // The time cap holds even before the hair has settled (a frame that took seconds).
    {
        asked = 0;
        CHECK(ParkTick(true, true, 1, kParkMaxMs, busy) == P::TimedOut);
        CHECK(ParkTick(true, true, 1, kParkMaxMs + 500, idle) == P::TimedOut);
        CHECK(asked == 0);
        CHECK(ParkTick(true, true, 1, kParkMaxMs - 1, idle) == P::Wait);
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_fighter_park: ok\n");
    return 0;
}

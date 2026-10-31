// Unit tests for hsm_slotdrift.h. Run via implementation/shared/run_tests.sh test_hsm_slotdrift.
//
// The failing case is the report's (#248): a replica's scabbard weapon read about 320 cm from its socket on every read,
// for the whole session, and the old snap (once, at 15 cm, when a joint was retargeted) never ran again.
#include "combat/hsm_slotdrift.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

int main() {
    // The report: 320 cm away on every read. No snap on the first far read, one on the second (a few frames later).
    {
        SlotDrift d;
        double t = 1000.0;
        CHECK(!d.Observe(1, 320.0, t).snap);
        t += kSlotWatchMs;
        CHECK(!d.Observe(1, 320.0, t).snap);          // only 125 ms of it: not yet
        t += kSlotWatchMs;
        const SlotDriftVerdict v = d.Observe(1, 320.0, t);   // 250 ms
        CHECK(v.snap);
        CHECK(v.snaps == 1);
        // A snap that did not hold is not repeated within the gap ...
        for (int i = 0; i < 6; ++i) { t += kSlotWatchMs; CHECK(!d.Observe(1, 320.0, t).snap); }
        // ... and is after it.
        t += kSlotWatchMs * 2;
        CHECK(d.Observe(1, 320.0, t).snap || d.Observe(1, 320.0, t + kSlotWatchMs).snap);
    }

    // A snap that works: the weapon is home on the next read and the count starts over.
    {
        SlotDrift d;
        d.Observe(1, 500.0, 0.0);
        CHECK(d.Observe(1, 500.0, 300.0).snap);
        CHECK(!d.Observe(1, 2.0, 425.0).snap);
        d.Observe(1, 500.0, 5000.0);
        const SlotDriftVerdict v = d.Observe(1, 500.0, 5300.0);
        CHECK(v.snap && v.snaps == 1);
    }

    // The threshold is 40 cm: the game's own slack (5 cm) and a swing of the hung weapon do not trigger it.
    {
        SlotDrift d;
        for (int i = 0; i < 40; ++i) CHECK(!d.Observe(1, kSlotDriftCm, i * kSlotWatchMs).snap);
        SlotDrift e;
        bool any = false;
        for (int i = 0; i < 4; ++i) any = e.Observe(1, kSlotDriftCm + 0.5, i * kSlotWatchMs).snap || any;
        CHECK(any);
    }

    // A single far read, or far reads that keep coming back near, never snap.
    {
        SlotDrift d;
        double t = 0.0;
        for (int i = 0; i < 20; ++i, t += kSlotWatchMs) CHECK(!d.Observe(1, (i % 2) ? 200.0 : 10.0, t).snap);
        CHECK(!d.Observe(2, 900.0, 0.0).snap);
        CHECK(!d.Observe(2, 5.0, 125.0).snap);
    }

    // The watchdog gives up after kSlotMaxSnaps snaps that did not bring the weapon near, and tries again later.
    {
        SlotDrift d;
        double t = 0.0;
        int snaps = 0, lastCount = 0;
        for (; t < 12000.0 && snaps < kSlotMaxSnaps; t += kSlotWatchMs) {
            const SlotDriftVerdict v = d.Observe(1, 320.0, t);
            if (v.snap) { ++snaps; lastCount = v.snaps; }
        }
        CHECK(snaps == kSlotMaxSnaps);
        CHECK(lastCount == kSlotMaxSnaps);
        const double gaveUp = t;
        int during = 0, after = 0;
        for (; t < gaveUp + kSlotGiveUpMs - 500.0; t += kSlotWatchMs) during += d.Observe(1, 320.0, t).snap;
        CHECK(during == 0);
        for (; t < gaveUp + kSlotGiveUpMs + 3000.0; t += kSlotWatchMs) after += d.Observe(1, 320.0, t).snap;
        CHECK(after >= 1);
    }

    // Coming home ends the give-up at once.
    {
        SlotDrift d;
        double t = 0.0;
        int snaps = 0;
        for (; snaps < kSlotMaxSnaps; t += kSlotWatchMs) snaps += d.Observe(1, 320.0, t).snap;
        d.Observe(1, 1.0, t);
        t += kSlotWatchMs;
        bool again = false;
        for (int i = 0; i < 10; ++i, t += kSlotWatchMs) again = d.Observe(1, 320.0, t).snap || again;
        CHECK(again);
    }

    // The report: past 100 cm, once per weapon per 10 s; not below.
    {
        SlotDrift d;
        CHECK(!d.Observe(1, 100.0, 0.0).report);
        CHECK(!d.Observe(1, 60.0, 125.0).report);
        CHECK(d.Observe(1, 150.0, 250.0).report);
        CHECK(!d.Observe(1, 150.0, 375.0).report);
        CHECK(!d.Observe(1, 150.0, 250.0 + kSlotReportEveryMs - 1.0).report);
        CHECK(d.Observe(1, 150.0, 250.0 + kSlotReportEveryMs).report);
        CHECK(d.Observe(2, 150.0, 400.0).report);   // another weapon has its own clock
    }

    // Weapons are independent.
    {
        SlotDrift d;
        d.Observe(1, 300.0, 0.0);
        d.Observe(2, 5.0, 0.0);
        CHECK(d.Observe(1, 300.0, 300.0).snap);
        CHECK(!d.Observe(2, 5.0, 300.0).snap);
    }

    // A weapon not read for a while starts over: a re-spawned weapon never inherits the old one's streak.
    {
        SlotDrift d;
        d.Observe(1, 300.0, 0.0);
        CHECK(!d.Observe(1, 300.0, kSlotStaleMs + 1000.0).snap);   // one far read after the gap: a new streak
        CHECK(d.Observe(1, 300.0, kSlotStaleMs + 1300.0).snap);
    }

    // Unreadable distances are ignored.
    {
        SlotDrift d;
        d.Observe(1, 300.0, 0.0);
        CHECK(!d.Observe(1, -1.0, 150.0).snap);
        CHECK(!d.Observe(1, std::nan(""), 200.0).snap);
        CHECK(d.Observe(1, 300.0, 300.0).snap);   // the streak survived them
    }

    // Memory is bounded and Forget works.
    {
        SlotDrift d;
        for (uint64_t k = 0; k < 100; ++k) d.Observe(k, 5.0, double(k));
        CHECK(d.Size() <= kSlotDriftMemory);
        d.Forget(99);
        CHECK(d.Size() <= kSlotDriftMemory - 1);
        d.Clear();
        CHECK(d.Size() == 0);
    }

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_slotdrift: ok\n");
    return 0;
}

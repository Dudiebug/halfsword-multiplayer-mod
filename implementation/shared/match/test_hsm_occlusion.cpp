#include "match/hsm_occlusion.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    // A new fighter is hidden until a trace says clear; one clear trace shows it; it fades in rather than popping.
    {
        OcclusionTracker t;
        t.Begin(1000);
        CHECK(t.Due(1));
        CHECK(t.Seen(1) == 0.f);
        t.Report(1, true);
        t.Begin(1016);
        CHECK(!t.Due(1));                                    // 10 Hz: not again so soon
        const float a = t.Seen(1);
        CHECK(a > 0.f && a < 1.f);
        t.Begin(1100);
        CHECK(t.Due(1));                                     // 100 ms later
        t.Begin(1400);
        CHECK(t.Seen(1) == 1.f);                             // fully in after 0.2 s
    }
    // No trace yet (the frame's two traces went to others): the tag stays hidden, it does not fade in on its own.
    {
        OcclusionTracker t;
        t.Begin(2000);
        CHECK(t.Due(1) && t.Due(2) && !t.Due(3));            // two traces a frame
        CHECK(t.Seen(3) == 0.f);
        t.Begin(2050);
        CHECK(t.Seen(3) == 0.f);
    }
    // Hide after two blocked traces in a row, show after one clear; a single blocked trace changes nothing.
    {
        OcclusionTracker t;
        uint32_t now = 0;
        auto trace = [&](bool clear) { now += kOcclusionIntervalMs; t.Begin(now); CHECK(t.Due(7)); t.Report(7, clear); return t.Seen(7); };
        trace(true); trace(true); trace(true);               // settle at full
        CHECK(trace(true) == 1.f);
        CHECK(trace(false) == 1.f);                          // one blocked: still shown (a fence post)
        CHECK(trace(true) == 1.f);                           // and the count starts over
        CHECK(trace(false) == 1.f);
        const float fading = trace(false);                   // the second in a row: starts fading out
        CHECK(fading < 1.f && fading > 0.f);
        CHECK(trace(false) == 0.f);                          // gone after 0.2 s
        CHECK(trace(false) == 0.f);
        const float back = trace(true);                      // one clear trace brings it back
        CHECK(back > 0.f);
        CHECK(trace(true) > back);
    }
    // At most two traces a frame, and the rest wait for the next frame; each fighter still gets its 10 Hz.
    {
        OcclusionTracker t;
        int done[6] = {};
        uint32_t now = 5000;
        int perFrameMax = 0;
        for (int frame = 0; frame < 100; ++frame, now += 10) {   // 100 fps for one second
            t.Begin(now);
            int n = 0;
            for (uint8_t e = 0; e < 6; ++e) if (t.Due(e)) { ++done[e]; ++n; t.Report(e, true); }
            perFrameMax = n > perFrameMax ? n : perFrameMax;
        }
        CHECK(perFrameMax == kOcclusionPerFrame);
        for (int e = 0; e < 6; ++e) CHECK(done[e] >= 9 && done[e] <= 10);
    }
    // A slow frame rate traces less often per fighter but never more than the budget.
    {
        OcclusionTracker t;
        int n = 0;
        uint32_t now = 0;
        for (int frame = 0; frame < 30; ++frame, now += 33) {    // 30 fps
            t.Begin(now);
            for (uint8_t e = 0; e < 16; ++e) if (t.Due(e)) { ++n; t.Report(e, true); }
        }
        CHECK(n <= 30 * kOcclusionPerFrame);
    }
    // Fighters nobody asks about are forgotten; one that returns starts hidden again.
    {
        OcclusionTracker t;
        t.Begin(0); CHECK(t.Due(3)); t.Report(3, true);
        t.Begin(500); t.Seen(3); t.Begin(900); t.Seen(3);
        CHECK(t.Tracked() == 1);
        t.Begin(900 + kOcclusionForgetMs + 1);
        CHECK(t.Tracked() == 0);
        CHECK(t.Due(3));
        CHECK(t.Seen(3) == 0.f);
    }
    // The clock wrapping past 2^32 ms keeps the schedule.
    {
        OcclusionTracker t;
        t.Begin(0xFFFFFFF0u); CHECK(t.Due(1));
        t.Begin(0xFFFFFFF8u); CHECK(!t.Due(1));
        t.Begin(0x00000060u); CHECK(t.Due(1));
    }
    std::printf(g_fail ? "%d FAILED\n" : "all occlusion tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

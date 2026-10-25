// Unit tests for hsm_slowsend.h. Run via implementation/shared/run_tests.sh test_hsm_slowsend.
#include "sync/hsm_slowsend.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static void TestClassify() {
    CHECK(SlowGapFor(1000.0 / 60.0) == kSlowGapMs);
    CHECK(SlowGapFor(1000.0 / 30.0) == 50.0);   // a bot's 30 Hz stream is not slow at 33 ms
    CHECK(ClassifyPoseGap(17, 17, 25) == SlowCause::None);
    CHECK(ClassifyPoseGap(25, 25, 25) == SlowCause::None);
    CHECK(ClassifyPoseGap(26, 40, 25) == SlowCause::Frame);     // the owner's clock jumped: its frame was long
    CHECK(ClassifyPoseGap(17, 26, 25) == SlowCause::Network);   // the clock ran on, the arrival did not
    CHECK(ClassifyPoseGap(17, 3, 25) == SlowCause::None);       // a burst after a held pose is not itself slow
}

// Feeds n poses spaced tsStep in the owner's clock and arrivalStep locally. Returns how many were due.
static int Feed(SlowSenderMeter& m, uint32_t& ts, double& at, int n, uint32_t tsStep, double arrivalStep) {
    int due = 0;
    for (int i = 0; i < n; ++i) {
        ts += tsStep; at += arrivalStep;
        if (m.Push(ts, at)) ++due;
    }
    return due;
}

static void TestSteadyStreamSaysNothing() {
    SlowSenderMeter m;
    uint32_t ts = 1000; double at = 0;
    CHECK(Feed(m, ts, at, 60 * 20, 17, 17.0) == 0);
}

static void TestSlowOwnerFrames() {
    SlowSenderMeter m;
    uint32_t ts = 1000; double at = 0;
    CHECK(Feed(m, ts, at, 200, 40, 40.0) == 1);   // 8 s of 25 fps: one window closed
    const SlowReport& r = m.Last();
    CHECK(r.cause == SlowCause::Frame && r.frame == r.gaps && r.network == 0);
    CHECK(r.worstTsGapMs == 40.0 && r.worstArrivalGapMs == 40.0);
    CHECK(Feed(m, ts, at, 500, 40, 40.0) == 0);   // the same cause again within kSlowRelogMs: quiet
    CHECK(Feed(m, ts, at, 500, 40, 40.0) == 1);   // ... and once more after it
}

static void TestNetworkHoldsPoses() {
    SlowSenderMeter m;
    uint32_t ts = 1000; double at = 0;
    int due = 0;
    for (int i = 0; i < 400; ++i) {               // every 4th pose arrives 60 ms late, then a burst
        ts += 17;
        at += (i % 4 == 0) ? 60.0 : 1.0;
        if (m.Push(ts, at)) ++due;
    }
    CHECK(due == 1);
    CHECK(m.Last().cause == SlowCause::Network && m.Last().frame == 0 && m.Last().network > 0);
}

static void TestBelowShareSaysNothing() {
    SlowSenderMeter m;
    uint32_t ts = 1000; double at = 0;
    int due = 0;
    for (int i = 0; i < 1000; ++i) {              // one long frame in 20: 5% < kSlowShare
        const uint32_t step = (i % 20 == 0) ? 40 : 17;
        ts += step; at += step;
        if (m.Push(ts, at)) ++due;
    }
    CHECK(due == 0);
}

static void TestCauseChangeLogsAtOnce() {
    SlowSenderMeter m;
    uint32_t ts = 1000; double at = 0;
    CHECK(Feed(m, ts, at, 200, 40, 40.0) == 1 && m.Last().cause == SlowCause::Frame);
    int due = 0;
    for (int i = 0; i < 800; ++i) {               // the first window after it is mixed (frame wins, already logged)
        ts += 17;
        at += (i % 4 == 0) ? 60.0 : 1.0;
        if (m.Push(ts, at)) ++due;
    }
    CHECK(due == 1 && m.Last().cause == SlowCause::Network);
}

static void TestBotCadence() {
    SlowSenderMeter m(1000.0 / 30.0);
    uint32_t ts = 1000; double at = 0;
    CHECK(Feed(m, ts, at, 400, 33, 33.0) == 0);
    CHECK(Feed(m, ts, at, 400, 60, 60.0) == 1 && m.Last().cause == SlowCause::Frame);
}

static void TestClockWrap() {
    SlowSenderMeter m;
    uint32_t ts = 0xFFFFFFFFu - 17u * 100u; double at = 0;
    CHECK(Feed(m, ts, at, 600, 17, 17.0) == 0);   // the stream clock wraps without a giant gap
}

static void TestFrameMeter() {
    SlowFrameMeter f;
    double now = 0;
    int due = 0;
    for (int i = 0; i < 600; ++i) { now += 16.7; if (f.Push(16.7, now)) ++due; }
    CHECK(due == 0);
    for (int i = 0; i < 300; ++i) { now += 30.0; if (f.Push(30.0, now)) ++due; }
    CHECK(due == 1 && f.Slow() > 0 && f.Slow() * 10 >= f.Frames() && f.WorstMs() == 30.0);
    for (int i = 0; i < 300; ++i) { now += 30.0; if (f.Push(30.0, now)) ++due; }
    CHECK(due == 1);                              // rate-limited
    for (int i = 0; i < 1000; ++i) { now += 30.0; if (f.Push(30.0, now)) ++due; }
    CHECK(due == 2);
    SlowFrameMeter g;
    now = 0; due = 0;
    for (int i = 0; i < 1000; ++i) { const double d = (i % 20 == 0) ? 30.0 : 16.7; now += d; if (g.Push(d, now)) ++due; }
    CHECK(due == 0);                              // 5% slow frames
    for (int i = 0; i < 1000; ++i) { now += 25.0; if (g.Push(25.0, now)) ++due; }
    CHECK(due == 0);                              // exactly 25 ms is not past it
    SlowFrameMeter h;                             // each window reports its own worst frame
    now = 0; due = 0;
    for (int i = 0; i < 300; ++i) { now += 40.0; if (h.Push(40.0, now)) ++due; }
    CHECK(due == 1 && h.WorstMs() == 40.0);
    for (int i = 0; i < 2000; ++i) { now += 26.0; if (h.Push(26.0, now)) ++due; }
    CHECK(due == 2 && h.WorstMs() == 26.0);
}

int main() {
    TestClassify();
    TestSteadyStreamSaysNothing();
    TestSlowOwnerFrames();
    TestNetworkHoldsPoses();
    TestBelowShareSaysNothing();
    TestCauseChangeLogsAtOnce();
    TestBotCadence();
    TestClockWrap();
    TestFrameMeter();
    if (g_fail) { std::printf("test_hsm_slowsend: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_slowsend: ok\n");
    return 0;
}

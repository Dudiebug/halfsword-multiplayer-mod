// Rev 42 input timeline: the InputBatch wire, the owner's sender, the receiver's due times, prediction with decay, the
// turn payback and the match input delay. Run via implementation/shared/run_tests.sh test_hsm_inputline.
#include "sync/hsm_inputline.h"
#include "hsm_inputdelay.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::sync;

static const size_t kTurn = size_t(Axis::TurnMouse), kFwd = size_t(Axis::MoveForward), kArm = size_t(Axis::RightArm);

static InputFrame Frame(uint32_t seq, float turn, float fwd, uint32_t buttons = 0) {
    InputFrame f; f.seq = seq; f.axes[kTurn] = turn; f.axes[kFwd] = fwd; f.buttons = buttons; return f;
}

static void TestWire() {
    InputBatch b; b.entity = 3; b.delayMs = 50;
    for (uint32_t i = 0; i < 10; ++i) b.frames.push_back({1000 + i * 17, Frame(100 + i, float(i), 1.f)});
    const auto bytes = Encode(b);
    InputBatch o;
    CHECK(Decode(bytes.data(), bytes.size(), o));
    CHECK(o.entity == 3 && o.delayMs == 50 && o.frames.size() == kInputBatchMax);              // the newest 8 only
    CHECK(o.frames.front().second.seq == 102 && o.frames.back().second.seq == 109 && o.frames.back().first == 1000 + 9 * 17);
    { InputBatch x = b; x.entity = 0; auto e = Encode(x); CHECK(!Decode(e.data(), e.size(), o)); }
    { InputBatch x = b; x.delayMs = kInputDelayMaxMs + 1; auto e = Encode(x); CHECK(!Decode(e.data(), e.size(), o)); }
    { InputBatch x = b; x.frames[9].second.seq = 200; auto e = Encode(x); CHECK(!Decode(e.data(), e.size(), o)); }   // not consecutive
    { InputBatch x = b; x.frames[9].first = 900; auto e = Encode(x); CHECK(!Decode(e.data(), e.size(), o)); }        // time went back
    { InputBatch x; x.entity = 1; auto e = Encode(x); CHECK(!Decode(e.data(), e.size(), o)); }                      // empty
    { auto e = bytes; e.push_back(0); CHECK(!Decode(e.data(), e.size(), o)); }                                       // trailing byte
    { auto e = bytes; e.pop_back(); CHECK(!Decode(e.data(), e.size(), o)); }                                         // short
}

static void TestSender() {
    InputBatchSender s;
    InputBatch b = s.Add(2, Frame(1, 0, 0), 1000, 50);
    CHECK(b.frames.size() == 1 && b.frames[0].first == 1050 && b.delayMs == 50);
    for (uint32_t i = 2; i <= 12; ++i) b = s.Add(2, Frame(i, 0, 0), 1000 + i * 17, 50);
    CHECK(b.frames.size() == kInputBatchMax && b.frames.back().second.seq == 12);
    b = s.Add(2, Frame(13, 0, 0), 1000 + 13 * 17, 0);                 // the delay went down: never before the last frame
    CHECK(b.frames.back().first == b.frames[b.frames.size() - 2].first);
    b = s.Add(2, Frame(40, 0, 0), 2000, 0);                           // a gap in seq: starts over
    CHECK(b.frames.size() == 1);
    b = s.Add(2, Frame(41, 0, 0), 2017, kInputDelayMaxMs + 500);     // the delay is capped
    CHECK(b.delayMs == kInputDelayMaxMs);
}

// Frames arrive before they are due (delay covers the trip): played at their own time, never predicted.
static void TestOnTime() {
    InputTimeline t;
    InputBatchSender s;
    std::vector<double> margins;
    // Owner: frames every 17 ms, delay 50 ms; the trip is 30 ms, so each arrives 20 ms early.
    for (uint32_t i = 1; i <= 3; ++i) {
        const uint32_t press = 1000 + i * 17;
        t.Push(s.Add(1, Frame(i, 2.f, 1.f, i), press, 50), press + 30, &margins);
    }
    CHECK(margins.size() == 3);
    CHECK(std::fabs(margins[0] - 20.0) < 1e-6);   // executes 50 ms after the press, arrived 30 ms after it
    TimelineStep st = t.Step(1000 + 17 + 3, 17);    // after the first press: its look is due, its main track is not
    CHECK(st.have && !st.predicted && st.frame.axes[kTurn] == 2.f);
    st = t.Step(1000 + 17 + 50, 17);                 // the first frame executes
    CHECK(st.have && !st.mainPredicted && st.frame.buttons == 1 && st.frame.axes[kFwd] == 1.f);
    st = t.Step(1000 + 51 + 50, 17);                 // the rest at once (a long game frame): newest levels, summed turns
    CHECK(st.have && st.frame.buttons == 3);
    CHECK(t.Buffered() == 0);
}

// A missing frame is predicted: levels held, the turn decays 1, 2/3, 1/3, 0, and the real frame pays the turns back.
static void TestPredictionAndPayback() {
    InputTimeline t;
    InputBatch b; b.entity = 1; b.delayMs = 0;
    b.frames.push_back({1000, Frame(1, 9.f, 1.f, 4)});
    t.Push(b, 990);
    TimelineStep st = t.Step(1000, 17);
    CHECK(st.have && !st.predicted && st.frame.axes[kTurn] == 9.f);
    float given = 0;
    const float expect[] = {9.f, 6.f, 3.f, 0.f, 0.f};
    for (int k = 0; k < 5; ++k) {
        st = t.Step(1000 + 17 * (k + 2), 17);
        CHECK(st.predicted && st.mainPredicted && st.lookPredicted && st.frame.buttons == 4 && st.frame.axes[kFwd] == 1.f);
        CHECK(std::fabs(st.frame.axes[kTurn] - expect[k]) < 1e-4);
        given += st.frame.axes[kTurn];
    }
    CHECK(st.predictedFrames >= 4);
    // The real frames arrive late: frame 2 turned 9 and frame 3 turned 0; what prediction gave (18) is taken back.
    InputBatch late; late.entity = 1; late.delayMs = 0;
    late.frames.push_back({1017, Frame(2, 9.f, 1.f, 4)});
    late.frames.push_back({1034, Frame(3, 0.f, 0.f, 0)});
    std::vector<double> margins;
    t.Push(late, 1110, &margins);
    CHECK(margins.size() == 2 && margins[0] < 0);   // late
    st = t.Step(1120, 17);
    CHECK(!st.predicted && st.frame.buttons == 0 && st.frame.axes[kFwd] == 0.f);
    CHECK(std::fabs(st.frame.axes[kTurn] - (9.f - given)) < 1e-4);   // net turn: real 9 minus predicted 18
    // A frame already played is not played again.
    t.Push(late, 1130);
    CHECK(t.Buffered() == 0);
}

// With nothing for a long time the fighter is idle: no predicted turn.
static void TestIdle() {
    InputTimeline t;
    InputBatch b; b.entity = 1; b.frames.push_back({1000, Frame(1, 5.f, 0.f)});
    t.Push(b, 1000);
    t.Step(1000, 17);
    TimelineStep st = t.Step(1000 + kTimelineStaleMs + 100, 17);
    CHECK(st.predicted && st.frame.axes[kTurn] == 0.f);
}

// The arm axes are levels on the main track: held, never summed.
static void TestLevelsNotSummed() {
    InputTimeline t;
    InputBatch b; b.entity = 1;
    InputFrame a = Frame(1, 0, 0); a.axes[kArm] = 0.5f;
    InputFrame c = Frame(2, 0, 0); c.axes[kArm] = 0.7f;
    b.frames.push_back({1000, a}); b.frames.push_back({1017, c});
    t.Push(b, 900);
    TimelineStep st = t.Step(1020, 17);
    CHECK(std::fabs(st.frame.axes[kArm] - 0.7f) < 1e-6);
}

static void TestOwnerClock() {
    CHECK(OwnerClockAt(5000, 4000, 60) == 1030);   // offset = clock difference + quickest trip: add back half the round trip
    CHECK(OwnerClockAt(5000, 4000, -5) == 1000);
}

static void TestMatchDelay() {
    CHECK(ParseMatchInputDelay("").mode == MatchDelayMode::Off);
    CHECK(ParseMatchInputDelay("off").mode == MatchDelayMode::Off);
    CHECK(ParseMatchInputDelay("AUTO").mode == MatchDelayMode::Auto);
    { MatchDelaySetting s = ParseMatchInputDelay("3"); CHECK(s.mode == MatchDelayMode::Frames && s.frames == 3); }
    CHECK(!ParseMatchInputDelay("5").valid);
    CHECK(!ParseMatchInputDelay("x").valid);
    CHECK(MatchDelayFramesToMs(3) == 50 && MatchDelayFramesToMs(9) == 67);
    CHECK(AutoMatchDelayFrames({}, 5) == 0);
    CHECK(AutoMatchDelayFrames({0, 0}, 5) == 0);
    CHECK(AutoMatchDelayFrames({20}, 2) == 1);      // 10 + 2 + 4 = 16 ms
    CHECK(AutoMatchDelayFrames({60, 20}, 5) == 3);  // 30 + 5 + 4 = 39 ms
    CHECK(AutoMatchDelayFrames({400}, 5) == kMatchDelayMaxFrames);
    MatchDelaySetting f; f.mode = MatchDelayMode::Frames; f.frames = 2;
    CHECK(MatchDelayFrames(f, {400}, 0) == 2);
    CHECK(MatchDelayFrames(MatchDelaySetting{}, {400}, 0) == 0);
}

int main() {
    TestWire();
    TestSender();
    TestOnTime();
    TestPredictionAndPayback();
    TestIdle();
    TestLevelsNotSummed();
    TestOwnerClock();
    TestMatchDelay();
    if (g_fail == 0) std::printf("test_hsm_inputline: all passed\n");
    return g_fail == 0 ? 0 : 1;
}

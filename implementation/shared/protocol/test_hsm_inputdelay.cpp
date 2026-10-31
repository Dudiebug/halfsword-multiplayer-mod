// Unit tests for hsm_inputdelay.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_inputdelay.cpp
#include "hsm_inputdelay.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestParse() {
    auto p = [](const char* s) { return ParseHostInputDelay(s); };
    CHECK(p("auto").mode == HostDelayMode::Auto && p("auto").valid);
    CHECK(p("").mode == HostDelayMode::Auto && p("  AUTO ").mode == HostDelayMode::Auto);
    CHECK(p("0").mode == HostDelayMode::Off && p("off").mode == HostDelayMode::Off);
    CHECK(p("40").mode == HostDelayMode::Fixed && p("40").fixedMs == 40);
    CHECK(p("40ms").fixedMs == 40 && p(" 25 ms").fixedMs == 25);
    CHECK(p("999").fixedMs == kHostDelayFixedMaxMs);                         // clamp
    CHECK(p("-5").mode == HostDelayMode::Off);                               // below 0 is 0
    const HostDelaySetting bad = p("fast");
    CHECK(!bad.valid && bad.mode == HostDelayMode::Auto);
    CHECK(!p("12x").valid && !p("ms").valid);
}

static void TestLag() {
    CHECK(ClientInputLagMs(70, 60) == 43);      // 35 + 8.33
    CHECK(ClientInputLagMs(0, 60) == 8);
    CHECK(ClientInputLagMs(100, 0) > 0);        // a bad rate does not divide by zero
    CHECK(MedianInputLagMs({}, 60) == 0);
    CHECK(MedianInputLagMs({0, 0}, 60) == 0);                                // none measured
    CHECK(MedianInputLagMs({70}, 60) == 43);
    CHECK(MedianInputLagMs({70, 62, 83}, 60) == 43);                         // the middle one of 43, 39, 50
    CHECK(MedianInputLagMs({70, 62, 83, 0}, 60) == 43);                      // unmeasured left out
    CHECK(MedianInputLagMs({70, 90}, 60) == 48);                             // mean of 43 and 53, rounded up
    CHECK(MedianInputLagMs({1000, 2000, 3000}, 60) == kHostDelayAutoMaxMs);  // clamp
}

static void TestGovernor() {
    HostDelayGovernor g(HostDelaySetting{});
    CHECK(g.Due(0) && g.delayMs() == 0);
    const std::vector<uint32_t> rtt{70, 62, 83};
    CHECK(g.Evaluate(1000, rtt, 60) && g.delayMs() == 10 && g.targetMs() == 43 && g.clients() == 3);
    CHECK(!g.Due(5999) && g.Due(6000));
    g.Evaluate(6000, rtt, 60); CHECK(g.delayMs() == 20);
    g.Evaluate(11000, rtt, 60); g.Evaluate(16000, rtt, 60); CHECK(g.delayMs() == 40);
    g.Evaluate(21000, rtt, 60); CHECK(g.delayMs() == 43);                    // arrives, does not overshoot
    CHECK(!g.Evaluate(26000, rtt, 60));
    g.Evaluate(31000, {20, 24}, 60); CHECK(g.targetMs() == 19 && g.delayMs() == 33);   // down by 10
    g.Evaluate(36000, {}, 60); CHECK(g.delayMs() == 0 && g.clients() == 0);  // no clients: at once
    g.Evaluate(41000, rtt, 60); CHECK(g.delayMs() == 10);
    g.Suspend(); CHECK(g.delayMs() == 0 && g.Due(0));                        // owner sync / out of the fight
}

static void TestGovernorFixedOff() {
    HostDelaySetting s; s.mode = HostDelayMode::Fixed; s.fixedMs = 80;
    HostDelayGovernor f(s);
    f.Evaluate(0, {}, 60); CHECK(f.delayMs() == 80);                         // fixed even alone, at once
    f.Evaluate(5000, {500}, 60); CHECK(f.delayMs() == 80);
    HostDelayGovernor off(ParseHostInputDelay("0"));
    CHECK(!off.Due(0) && off.delayMs() == 0);
    off.Evaluate(0, {100}, 60); CHECK(off.delayMs() == 0);
}

static void TestQueueActions() {
    DelayedInput q;
    q.PushAction(1000, 3, true);
    q.PushAction(1010, 3, false);
    q.PushAction(1010, 5, true);
    CHECK(q.Take(1030, 40).actions.empty());                                 // nothing 40 ms old yet
    auto o = q.Take(1040, 40);
    CHECK(o.actions.size() == 1 && o.actions[0].first == 3 && o.actions[0].second);
    o = q.Take(1060, 40);
    CHECK(o.actions.size() == 2 && !o.actions[0].second && o.actions[1].first == 5);   // order kept
    CHECK(q.Empty());
    q.PushAction(2000, 1, true);
    CHECK(q.Take(2000, 0).actions.size() == 1);                              // delay 0 hands over at once
    for (int i = 0; i < 2000; ++i) q.PushAction(3000, 1, i % 2 == 0);
    CHECK(q.Take(3000, 0).actions.size() == DelayedInput::kMaxActions);      // bounded
}

static void TestQueueAxes() {
    constexpr size_t F = size_t(Axis::MoveForward), R = size_t(Axis::RightArm);
    DelayedInput q;
    q.PushAxis(1000, F, 0.f);                                                // the game fires the axes every frame
    q.PushAxis(1016, F, 0.f);
    q.PushAxis(1032, F, 1.f);
    q.PushAxis(1048, F, 1.f);
    q.PushAxis(1064, F, 0.f);
    auto o = q.Take(1050, 50);                                               // as of 1000: 0, quiet window not over
    CHECK(o.feed[F] && o.axes[F] == 0.f);
    o = q.Take(1070, 50);                                                    // as of 1020: still 0 (1 came at 1032)
    CHECK(o.axes[F] == 0.f);
    o = q.Take(1090, 50);                                                    // as of 1040: 1
    CHECK(o.feed[F] && o.axes[F] == 1.f);
    o = q.Take(1100, 50);                                                    // held: fed every frame
    CHECK(o.feed[F] && o.axes[F] == 1.f);
    o = q.Take(1120, 50);                                                    // as of 1070: back to 0
    CHECK(o.axes[F] == 0.f && o.feed[F]);
    o = q.Take(1200, 50);
    CHECK(o.axes[F] == 0.f && o.feed[F]);                                    // 80 ms of 0: still inside the quiet window
    o = q.Take(1700, 50);
    CHECK(o.axes[F] == 0.f && !o.feed[F]);                                   // past it: not fed
    CHECK(!o.feed[R]);                                                       // an axis never seen is never fed
    q.PushAxis(1800, F, 1.f);
    o = q.Take(1800, 0); CHECK(o.axes[F] == 1.f && o.feed[F]);               // flush: the latest value
    q.Clear(); CHECK(q.Empty());
}

int main() {
    TestParse(); TestLag(); TestGovernor(); TestGovernorFixedOff(); TestQueueActions(); TestQueueAxes();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

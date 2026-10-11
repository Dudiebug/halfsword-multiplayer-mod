// Unit tests for hsm_input.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_input.cpp
#include "hsm_input.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Has(const std::vector<Action>& v, Action a) { for (Action x : v) if (x == a) return true; return false; }

static void TestCapture() {
    InputCapture c;
    c.OnAxis(Axis::TurnMouse, 1.5f); c.OnAxis(Axis::TurnMouse, 2.0f);       // deltas accumulate
    c.OnAxis(Axis::MoveForward, 0.3f); c.OnAxis(Axis::MoveForward, 1.0f);   // levels keep latest
    c.OnAxis(Axis::LookMouse, 1e9f);                                         // clamped
    c.OnAction(Action::GrabLeft, true);
    InputFrame f = c.Take(10);
    CHECK(f.seq == 1 && f.axes[size_t(Axis::TurnMouse)] == 3.5f && f.axes[size_t(Axis::MoveForward)] == 1.0f);
    CHECK(f.axes[size_t(Axis::LookMouse)] == kAxisLimit);
    CHECK(f.buttons == 1u << size_t(Action::GrabLeft));
    InputFrame g = c.Take(20);
    CHECK(g.seq == 2 && g.axes[size_t(Axis::TurnMouse)] == 0 && g.axes[size_t(Axis::MoveForward)] == 1.0f);   // deltas reset, levels persist
    c.OnAction(Action::GrabLeft, false); CHECK(c.Take(30).buttons == 0);
    // the produced frame survives the wire format
    auto bytes = Encode(f); InputFrame d; CHECK(Decode(bytes.data(), bytes.size(), d) && d.axes == f.axes);
}

static void TestApplier() {
    InputApplier a;
    CHECK(a.Tick().pressed.empty());                                         // nothing received yet
    InputFrame f; f.seq = 5; f.axes[size_t(Axis::TurnMouse)] = 2; f.axes[size_t(Axis::MoveRight)] = -1; f.buttons = 1u << size_t(Action::Thrust);
    CHECK(a.Receive(f));
    auto s = a.Tick();
    CHECK(s.axes[size_t(Axis::TurnMouse)] == 2 && s.axes[size_t(Axis::MoveRight)] == -1 && Has(s.pressed, Action::Thrust) && s.released.empty());
    auto s2 = a.Tick();                                                      // no new frame: delta consumed, level held, no edges
    CHECK(s2.axes[size_t(Axis::TurnMouse)] == 0 && s2.axes[size_t(Axis::MoveRight)] == -1 && s2.pressed.empty() && s2.released.empty());
    CHECK(!a.Receive(f));                                                    // duplicate
    InputFrame old = f; old.seq = 4; CHECK(!a.Receive(old));                 // stale
    InputFrame g = f; g.seq = 6; g.axes[size_t(Axis::TurnMouse)] = 1; g.buttons = 1u << size_t(Action::Jump);
    InputFrame h = g; h.seq = 7; h.axes[size_t(Axis::TurnMouse)] = 4;
    CHECK(a.Receive(g) && a.Receive(h));                                     // two frames between host ticks: deltas summed
    auto s3 = a.Tick();
    CHECK(s3.axes[size_t(Axis::TurnMouse)] == 5 && Has(s3.released, Action::Thrust) && Has(s3.pressed, Action::Jump));
    auto r = a.ReleaseAll();
    CHECK(Has(r.released, Action::Jump) && r.pressed.empty());
    InputFrame wrap; wrap.seq = 0xFFFFFFFFu; CHECK(a.Receive(wrap)); InputFrame next; next.seq = 0; CHECK(a.Receive(next));   // wraparound
}

static void TestDeltaFloodBounded() {
    InputApplier a;
    for (uint32_t seq = 1; seq <= 500; ++seq) {                               // a client floods frames between two host ticks
        InputFrame f; f.seq = seq; f.axes[size_t(Axis::TurnMouse)] = kAxisLimit; f.axes[size_t(Axis::LookMouse)] = -kAxisLimit;
        CHECK(a.Receive(f));
    }
    InputApplier::Step s = a.Tick();
    CHECK(s.axes[size_t(Axis::TurnMouse)] == kMaxDeltaPerTick && s.axes[size_t(Axis::LookMouse)] == -kMaxDeltaPerTick);
    InputFrame g; g.seq = 501; g.axes[size_t(Axis::TurnMouse)] = 3;
    a.Receive(g); g.seq = 502; a.Receive(g);
    CHECK(a.Tick().axes[size_t(Axis::TurnMouse)] == 6);                        // normal accumulation untouched
}

static void TestDeltaSpreadOverHostFrames() {
    InputApplier a;
    InputFrame f; f.seq = 1; f.clientTimeMs = 1000; a.Receive(f);
    InputFrame g; g.seq = 2; g.clientTimeMs = 1020; g.axes[size_t(Axis::TurnMouse)] = 40; a.Receive(g);   // 20 ms client interval
    float first = a.Tick(5.f).axes[size_t(Axis::TurnMouse)];
    CHECK(first > 9.9f && first < 10.1f);                                     // a quarter of the interval -> a quarter
    float sum = first;
    for (int i = 0; i < 40; ++i) sum += a.Tick(5.f).axes[size_t(Axis::TurnMouse)];
    CHECK(sum > 39.9f && sum < 40.1f);                                        // nothing lost, nothing invented
    InputFrame h; h.seq = 3; h.clientTimeMs = 1040; h.axes[size_t(Axis::TurnMouse)] = 8; a.Receive(h);
    float all = a.Tick(100.f).axes[size_t(Axis::TurnMouse)];
    CHECK(all > 7.99f && all < 8.01f);                                         // a long host frame takes all that is pending
    CHECK(a.Tick().axes[size_t(Axis::TurnMouse)] == 0);
}

int main() {
    TestDeltaSpreadOverHostFrames();
    TestDeltaFloodBounded();
    TestCapture(); TestApplier();
    std::printf(g_fail ? "%d FAILED\n" : "all input tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

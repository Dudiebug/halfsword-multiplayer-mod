// RemoteWrite: the wire, the push gathering and the owner's gate. Run via implementation/shared/run_tests.sh test_hsm_remote.
#include "sync/hsm_remote.h"
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::sync;

static bool RoundTrip(const RemoteWriteMsg& m, RemoteWriteMsg& out) { auto b = Encode(m); return Decode(b.data(), b.size(), out); }

static void TestWire() {
    RemoteWriteMsg m; m.kind = RemoteKind::Push; m.from = 2; m.target = 5; m.bone = 0; m.seq = 77; m.vec = V3{1000, -200, 50};
    RemoteWriteMsg o;
    CHECK(RoundTrip(m, o) && o.kind == RemoteKind::Push && o.from == 2 && o.target == 5 && o.bone == 0 && o.seq == 77);
    CHECK(o.vec.x == 1000 && o.vec.y == -200 && o.vec.z == 50);
    { RemoteWriteMsg b = m; b.vec = V3{kPushMaxImpulse * 2, 0, 0}; CHECK(!RoundTrip(b, o)); }          // too strong
    { RemoteWriteMsg b = m; b.vec.y = std::numeric_limits<double>::quiet_NaN(); CHECK(!RoundTrip(b, o)); }
    { RemoteWriteMsg b = m; b.bone = uint8_t(kPoseBones); CHECK(!RoundTrip(b, o)); }
    { RemoteWriteMsg b = m; b.bone = 1; CHECK(!RoundTrip(b, o)); }                                     // spine_01: no body
    { RemoteWriteMsg b = m; b.target = b.from; CHECK(!RoundTrip(b, o)); }
    { RemoteWriteMsg b = m; b.from = 0; CHECK(!RoundTrip(b, o)); }
    { RemoteWriteMsg b = m; b.target = uint8_t(kMaxWillies + 1); CHECK(!RoundTrip(b, o)); }
    RemoteWriteMsg g; g.kind = RemoteKind::GrabBreak; g.from = 3; g.target = 1; g.bone = 1;
    CHECK(RoundTrip(g, o) && o.kind == RemoteKind::GrabBreak && o.bone == 1);
    g.bone = 2; CHECK(!RoundTrip(g, o));
    g.bone = 0; g.vec.x = 1; CHECK(!RoundTrip(g, o));
    RemoteWriteMsg r; r.kind = RemoteKind::Revive; r.from = 1; r.target = 4;
    CHECK(RoundTrip(r, o) && o.kind == RemoteKind::Revive && o.target == 4);
    r.bone = 3; CHECK(!RoundTrip(r, o));
    RemoteWriteMsg p; p.kind = RemoteKind::Press; p.from = 2; p.target = 5; p.bone = 18; p.vec = V3{10, -20, 30};
    CHECK(RoundTrip(p, o) && o.kind == RemoteKind::Press && o.bone == 18 && o.vec.y == -20);
    { RemoteWriteMsg b = p; b.vec = V3{kPressMaxOffset * 1.5, 0, 0}; CHECK(!RoundTrip(b, o)); }       // not on that bone
    { RemoteWriteMsg b = p; b.bone = 1; CHECK(!RoundTrip(b, o)); }                                     // spine_01: no body
    { RemoteWriteMsg b = p; b.bone = uint8_t(kPoseBones); CHECK(!RoundTrip(b, o)); }
    RemoteWriteMsg t; t.kind = RemoteKind::Talk; t.from = 2; t.target = 11; t.vec = V3{-1, 0.25, 0};
    CHECK(RoundTrip(t, o) && o.kind == RemoteKind::Talk && o.target == 11 && o.vec.x == -1 && o.vec.y == 0.25);
    { RemoteWriteMsg b = t; b.vec.x = 1.5; CHECK(!RoundTrip(b, o)); }
    { RemoteWriteMsg b = t; b.vec.y = -1.5; CHECK(!RoundTrip(b, o)); }
    { RemoteWriteMsg b = t; b.vec.z = 0.5; CHECK(!RoundTrip(b, o)); }
    { RemoteWriteMsg b = t; b.bone = 1; CHECK(!RoundTrip(b, o)); }
    CHECK(kLastRemoteKind == 6);
    auto b = Encode(m); b[1] = 9; CHECK(!Decode(b.data(), b.size(), o));                               // unknown kind
    b = Encode(m); b.pop_back(); CHECK(!Decode(b.data(), b.size(), o));
}

static void TestGather() {
    PushGather g;
    g.Add(2, 5, 0, V3{300, 0, 0}, 0, 1);
    g.Add(2, 5, 3, V3{500, 0, 0}, 20, 2);       // the strongest contact names the bone
    g.Add(2, 5, 3, V3{450, 0, 0}, 21, 2);       // the same frame: counted once, by its strongest
    CHECK(g.Drain(40).empty());                 // the window is still open
    auto out = g.Drain(60);
    CHECK(out.size() == 1 && out[0].from == 2 && out[0].target == 5 && out[0].bone == 3 && out[0].vec.x == 800);
    CHECK(g.Drain(200).empty());
    g.Add(2, 5, 0, V3{100, 0, 0}, 300, 3);      // a brush: below kPushMinImpulse
    CHECK(g.Drain(400).empty());
    g.Add(2, 5, 0, V3{0, 1e9, 0}, 500, 4);      // a solver spike: capped
    out = g.Drain(600);
    CHECK(out.size() == 1 && out[0].vec.y <= kPushMaxImpulse * 1.0001 && out[0].vec.y > kPushMaxImpulse * 0.999);
    RemoteWriteMsg o; auto b = Encode(out[0]); CHECK(Decode(b.data(), b.size(), o));   // a capped push is valid on the wire
    g.Add(2, 5, 1, V3{1000, 0, 0}, 700, 5);     // spine_01 has no body: ignored
    g.Add(2, 2, 0, V3{1000, 0, 0}, 700, 5);     // itself
    g.Add(2, 5, 0, V3{std::numeric_limits<double>::infinity(), 0, 0}, 700, 5);
    CHECK(g.Drain(800).empty());
    int sent = 0;                                // at most kPushesPerSecond per target per second
    for (int i = 0; i < 30; ++i) { g.Add(3, 6, 0, V3{1000, 0, 0}, 1000 + i * 60.0, uint64_t(10 + i)); sent += int(g.Drain(1000 + i * 60.0 + 55).size()); }
    CHECK(sent >= kPushesPerSecond && sent <= 2 * kPushesPerSecond);
    g.Add(4, 7, 0, V3{1000, 0, 0}, 5000, 50);
    g.Add(3, 7, 0, V3{1000, 0, 0}, 5000, 50);   // two attackers: two pushes
    CHECK(g.Drain(5100).size() == 2);
}

static void TestGate() {
    RemoteGate gate;
    CHECK(gate.Revive(4, 100));
    CHECK(!gate.Revive(4, 600));
    CHECK(gate.Revive(5, 600));
    CHECK(gate.Revive(4, 1200));
    CHECK(!gate.Revive(uint8_t(kMaxWillies + 1), 5000));
}

// A hold re-taken in a struggle presses once: one message per (target, bone) per kPressEveryMs, and what is sent decodes.
static void TestPress() {
    PressGate g;
    RemoteWriteMsg m, o;
    CHECK(g.Make(2, 5, 18, V3{5, 0, -3}, 1000, m) && m.kind == RemoteKind::Press && m.from == 2 && m.target == 5 && m.bone == 18);
    CHECK(m.vec.x == 5 && m.vec.z == -3 && RoundTrip(m, o));
    CHECK(!g.Make(2, 5, 18, V3{5, 0, -3}, 1016, m));                    // the hold slipped and was re-taken
    CHECK(!g.Make(2, 5, 18, V3{5, 0, -3}, 1000 + kPressEveryMs - 1, m));
    CHECK(g.Make(2, 5, 18, V3{5, 0, -3}, 1000 + kPressEveryMs, m));
    CHECK(g.Make(2, 5, 17, V3{5, 0, -3}, 1016 + kPressEveryMs, m));     // the other hand, on another bone
    CHECK(g.Make(2, 6, 18, V3{5, 0, -3}, 1016 + kPressEveryMs, m));     // another fighter
    CHECK(g.Make(3, 7, 0, V3{0, 500, 0}, 5000, m));                     // a grab point far off the bone: capped
    CHECK(m.vec.y <= kPressMaxOffset * 1.0001 && m.vec.y > kPressMaxOffset * 0.999 && RoundTrip(m, o));
    CHECK(!g.Make(3, 3, 0, V3{}, 9000, m));                             // itself
    CHECK(!g.Make(0, 3, 0, V3{}, 9000, m));
    CHECK(!g.Make(3, uint8_t(kMaxWillies + 1), 0, V3{}, 9000, m));
    CHECK(!g.Make(3, 4, 1, V3{}, 9000, m));                             // spine_01 has no body
    CHECK(!g.Make(3, 4, -1, V3{}, 9000, m) && !g.Make(3, 4, kPoseBones, V3{}, 9000, m));
    CHECK(!g.Make(3, 4, 0, V3{std::numeric_limits<double>::quiet_NaN(), 0, 0}, 9000, m));
    CHECK(g.Make(3, 4, 0, V3{}, 9000, m));                              // the refused ones did not start the wait
    g.Clear();
    CHECK(g.Make(2, 5, 18, V3{}, 1001 + kPressEveryMs, m));
}

// One line per speaker per kTalkEveryMs on both ends; the two values are clamped to what the wire takes.
static void TestTalk() {
    TalkGate g;
    RemoteWriteMsg m, o;
    CHECK(g.Make(2, 11, 0.5, 0.25, 1000, m) && m.kind == RemoteKind::Talk && m.from == 2 && m.target == 11);
    CHECK(m.vec.x == 0.5 && m.vec.y == 0.25 && m.vec.z == 0 && RoundTrip(m, o));
    CHECK(!g.Make(2, 12, -1, -1, 1200, m));                             // the same speaker, too soon
    CHECK(g.Make(3, 11, -1, -1, 1200, m));                              // another speaker
    CHECK(g.Make(2, 12, -1, -0.5, 1000 + kTalkEveryMs, m) && m.vec.x == -1 && m.vec.y == -0.5);
    CHECK(g.Make(4, 11, 7, -9, 1000, m) && m.vec.x == kTalkMaxValue && m.vec.y == -kTalkMaxValue && RoundTrip(m, o));
    CHECK(!g.Make(5, 5, 0, 0, 1000, m) && !g.Make(0, 5, 0, 0, 1000, m) && !g.Make(5, 0, 0, 0, 1000, m));
    CHECK(!g.Make(5, 6, std::numeric_limits<double>::infinity(), 0, 1000, m));
    CHECK(g.Make(5, 6, 0, 0, 1000, m));                                 // the refused ones did not start the wait
    TalkGate host;                                                      // the host's side: the same wait per speaker
    CHECK(host.Allow(2, 100) && !host.Allow(2, 300) && host.Allow(3, 300) && host.Allow(2, 100 + kTalkEveryMs));
    CHECK(!host.Allow(0, 5000) && !host.Allow(uint8_t(kMaxWillies + 1), 5000));
}

int main() {
    TestWire();
    TestGather();
    TestGate();
    TestPress();
    TestTalk();
    if (g_fail == 0) std::printf("test_hsm_remote: all passed\n");
    return g_fail == 0 ? 0 : 1;
}

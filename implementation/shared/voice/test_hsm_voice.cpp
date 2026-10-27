// Proximity voice (0.7.0-pre5): the Voice message, the level gate, the consciousness cut-off, who hears whom, the
// spatial gains, the frame guards and the mixer.
#include "hsm_protocol.h"
#include "voice/hsm_voice.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::voice;

static void TestWire() {
    VoiceMsg m; m.entity = 3; m.seq = 0xBEEF; m.flags = 1; m.data = {1, 2, 3, 4, 5};
    VoiceMsg o;
    auto b = Encode(m);
    CHECK(Decode(b.data(), b.size(), o) && o.entity == 3 && o.seq == 0xBEEF && o.flags == 1 && o.data == m.data);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    auto extra = b; extra.push_back(9); CHECK(!Decode(extra.data(), extra.size(), o));   // length must match
    VoiceMsg bad = m; bad.entity = 0; b = Encode(bad); CHECK(!Decode(b.data(), b.size(), o));
    bad = m; bad.flags = 2; b = Encode(bad); CHECK(!Decode(b.data(), b.size(), o));
    bad = m; bad.data.clear(); b = Encode(bad); CHECK(!Decode(b.data(), b.size(), o));     // an empty chunk is not sent
    VoiceMsg big = m; big.data.assign(kMaxVoiceBytes + 50, 7); b = Encode(big);           // the encoder bounds it
    CHECK(Decode(b.data(), b.size(), o) && o.data.size() == kMaxVoiceBytes);
    auto huge = b; huge[8] = uint8_t((kMaxVoiceBytes + 1) & 0xFF); huge[9] = uint8_t((kMaxVoiceBytes + 1) >> 8);
    huge.push_back(7); CHECK(!Decode(huge.data(), huge.size(), o));
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::Voice);
}

static void TestLevelGate() {
    std::vector<int16_t> quiet(480, 30), loud(480, 0);
    for (size_t i = 0; i < loud.size(); ++i) loud[i] = int16_t(i % 2 ? 6000 : -6000);
    CHECK(RmsDbfs(nullptr, 0) == -120.f && RmsDbfs(std::vector<int16_t>(10, 0).data(), 10) == -120.f);
    CHECK(RmsDbfs(quiet.data(), quiet.size()) < -40.f && RmsDbfs(loud.data(), loud.size()) > -20.f);
    LevelGate g({-40.f, 400});
    CHECK(!g.Feed(RmsDbfs(quiet.data(), quiet.size()), 1000));
    CHECK(g.Feed(-40.f, 1100));                 // at the threshold: open
    CHECK(g.Feed(-60.f, 1400) && g.Feed(-60.f, 1500));   // hold
    CHECK(!g.Feed(-60.f, 1501));                // hold over
    CHECK(!g.Feed(-41.f, 1600));
    g.Reset(); CHECK(!g.open());
    PreRoll pr;
    for (uint8_t i = 1; i <= 5; ++i) pr.Keep({i});
    pr.Keep({});
    auto got = pr.Take();
    CHECK(got.size() == 3 && got[0][0] == 3 && got[2][0] == 5 && pr.size() == 0);
}

static void TestConscious() {
    ConsciousGate c;
    CHECK(!c.ok());
    CHECK(c.Update(100.f, false, 0.3f));        // full: may talk (first sight opens at cut + margin)
    CHECK(c.Update(31.f, false, 0.3f));
    CHECK(!c.Update(29.f, false, 0.3f));        // below 30 % of full: cut off
    CHECK(!c.Update(33.f, false, 0.3f));        // hysteresis: not back until 35 %
    CHECK(c.Update(35.f, false, 0.3f));
    CHECK(!c.Update(80.f, true, 0.3f));         // dead
    CHECK(!c.Update(0.f, false, 0.3f));         // knocked out
    CHECK(c.Update(100.f, false, 0.3f) && c.peak() == 100.f);
    ConsciousGate unit;                         // another scale (0..1): the share is what counts
    CHECK(unit.Update(1.f, false, 0.3f) && !unit.Update(0.2f, false, 0.3f) && unit.Update(0.5f, false, 0.3f));
    CHECK(!unit.Update(std::nanf(""), false, 0.3f));
    ConsciousGate low; CHECK(!low.Update(0.f, false, 0.3f));   // never above 0: no voice
}

static void TestAudible() {
    CHECK(Audible(true, true, true) == Hear::Spatial);
    CHECK(Audible(true, true, false) == Hear::Spatial);   // a dead spectator hears the living
    CHECK(Audible(true, false, true) == Hear::None);      // a knocked-out speaker is cut off
    CHECK(Audible(true, false, false) == Hear::None);
    CHECK(Audible(false, false, true) == Hear::None);     // the living do not hear the dead
    CHECK(Audible(false, false, false) == Hear::Flat);    // the dead hear each other
    Range r;
    CHECK(RelayVoice(Hear::Spatial, 3400.f, true, r) && !RelayVoice(Hear::Spatial, 3600.f, true, r));
    CHECK(RelayVoice(Hear::Spatial, 9000.f, false, r) && RelayVoice(Hear::Spatial, -1.f, true, r));
    CHECK(RelayVoice(Hear::Flat, 9000.f, false, r) && !RelayVoice(Hear::None, 10.f, true, r));
}

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void TestSpatial() {
    Range r{500.f, 3000.f};
    CHECK(DistanceGain(0.f, r) == 1.f && DistanceGain(500.f, r) == 1.f && DistanceGain(3000.f, r) == 0.f);
    CHECK(Near(DistanceGain(1750.f, r), 0.25f) && DistanceGain(1000.f, r) > DistanceGain(2000.f, r));
    Gains front = Spatialize({0, 0, 0}, 0.f, {300, 0, 0}, r);         // straight ahead
    CHECK(Near(front.left, 1.f) && Near(front.right, 1.f));
    Gains right = Spatialize({0, 0, 0}, 0.f, {0, 300, 0}, r);         // +Y is to the right at yaw 0
    CHECK(Near(right.right, 1.f) && Near(right.left, kPanFloor));
    Gains turned = Spatialize({0, 0, 0}, 90.f, {0, 300, 0}, r);       // facing +Y: ahead again
    CHECK(Near(turned.left, 1.f) && Near(turned.right, 1.f));
    Gains left = Spatialize({0, 0, 0}, 90.f, {300, 0, 0}, r);         // facing +Y, +X is on the left
    CHECK(Near(left.left, 1.f) && Near(left.right, kPanFloor));
    Gains far = Spatialize({0, 0, 0}, 0.f, {4000, 0, 0}, r); CHECK(far.left == 0.f && far.right == 0.f);
    Gains above = Spatialize({0, 0, 0}, 0.f, {0, 0, 200}, r); CHECK(Near(above.left, 1.f) && Near(above.right, 1.f));
}

static void TestGuards() {
    SeqGuard s;
    CHECK(s.Accept(10, 0) && s.Accept(11, 20) && !s.Accept(11, 40) && !s.Accept(5, 60) && s.Accept(13, 80));
    CHECK(s.Accept(0xFFFF, 1200));             // a second of silence: starts over
    CHECK(s.Accept(0, 1220) && !s.Accept(0xFFFF, 1240));   // wraps
    VoiceRate v;
    int ok = 0; for (int i = 0; i < 100; ++i) ok += v.Allow(100, 1000) ? 1 : 0;
    CHECK(ok == int(VoiceRate::kFramesPerS));   // a second of burst, then nothing
    CHECK(!v.Allow(100, 1000) && v.Allow(100, 1100));   // refilled 6 frames in 100 ms
    VoiceRate b; CHECK(!b.Allow(VoiceRate::kBytesPerS + 1, 0) && b.Allow(1000, 0));
}

static void TestMixer() {
    Mixer m;
    std::vector<int16_t> pcm(Mixer::kPrebuffer - 1, 1000), out(2 * 240, 7);
    m.Push(2, pcm.data(), pcm.size());
    m.SetGains(2, {1.f, 0.5f});
    m.Mix(out.data(), 240);
    CHECK(out[0] == 0 && out[1] == 0 && !m.Playing(2));   // not enough queued yet: silence
    int16_t one = 1000; m.Push(2, &one, 1);
    m.Mix(out.data(), 240);
    CHECK(m.Playing(2) && out[2 * 239] == 1000 && out[2 * 239 + 1] == 500);   // ramped from 0 to the target
    CHECK(out[0] == 0 && out[1] == 0);                                         // the ramp starts at the old gain
    m.Mix(out.data(), 240);
    CHECK(out[0] == 1000 && out[1] == 500);
    std::vector<int16_t> lots(Mixer::kMaxQueue + 100, 20000);
    m.Push(3, lots.data(), lots.size()); m.Push(4, lots.data(), lots.size());
    CHECK(m.Queued(3) == Mixer::kMaxQueue && m.dropped() >= 100);
    m.SetGains(3, {1.f, 1.f}); m.SetGains(4, {1.f, 1.f});
    m.Mix(out.data(), 240); m.Mix(out.data(), 240);
    CHECK(out[0] == 32767);                                                    // clipped, never wrapped
    m.Push(99, lots.data(), 10); CHECK(m.Queued(99) == 0);                     // out of range: ignored
    m.Forget(3); CHECK(m.Queued(3) == 0);
    m.Clear(); CHECK(m.Queued(4) == 0 && !m.Playing(4));
}

int main() {
    TestWire(); TestLevelGate(); TestConscious(); TestAudible(); TestSpatial(); TestGuards(); TestMixer();
    std::printf(g_fail ? "%d FAILED\n" : "test_hsm_voice: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

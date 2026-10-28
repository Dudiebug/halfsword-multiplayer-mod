// Detours, step R1 (0.7.0-pre6): SipHash-2-4, the rev 23 RouteTable and Forward wire, the host's tables and keys,
// and a three-client lobby played offline: A's poses reach B through C, and every way C or anyone else could misuse
// a Forward is refused.
#include "session/hsm_detour.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestSipHash() {
    // Reference vectors from the SipHash paper's test code: key 00..0f, messages 00..(n-1).
    DetourKey k; for (size_t i = 0; i < k.size(); ++i) k[i] = uint8_t(i);
    uint8_t msg[64]; for (size_t i = 0; i < sizeof msg; ++i) msg[i] = uint8_t(i);
    CHECK(SipHash24(k, msg, 0) == 0x726fdb47dd0e0e31ULL);
    CHECK(SipHash24(k, msg, 1) == 0x74f839c593dc67fdULL);
    CHECK(SipHash24(k, msg, 7) == 0xab0200f58b01d137ULL);
    CHECK(SipHash24(k, msg, 8) == 0x93f5f5799a932462ULL);
    CHECK(SipHash24(k, msg, 15) == 0xa129ca6149be45e5ULL);
    CHECK(SipHash24(k, msg, 63) == 0x958a324ceb064572ULL);
    DetourKey k2 = k; k2[15] ^= 1;
    CHECK(SipHash24(k2, msg, 15) != SipHash24(k, msg, 15));
}

static std::vector<uint8_t> FakeStream(MsgType t, uint8_t fill, size_t n) {
    Writer w; w.header(t);
    for (size_t i = 0; i < n; ++i) w.u8(uint8_t(fill + i));
    return w.buf;
}

static void TestWire() {
    VoiceModeMsg vm; vm.reach = 2; VoiceModeMsg vo;
    auto b = Encode(vm);
    CHECK(Decode(b.data(), b.size(), vo) && vo.reach == 2);
    vm.reach = 3; b = Encode(vm); CHECK(!Decode(b.data(), b.size(), vo));
    for (size_t cut = 0; cut + 1 < Encode(VoiceModeMsg{1}).size(); ++cut) { auto c = Encode(VoiceModeMsg{1}); CHECK(!Decode(c.data(), cut, vo)); }

    VoiceMsg v; v.entity = 2; v.seq = 1; v.data = {1, 2, 3}; VoiceMsg o;
    for (uint8_t f : {uint8_t(0), kVoiceSpurt, kVoiceAdpcm, uint8_t(kVoiceSpurt | kVoiceAdpcm)}) {
        v.flags = f; b = Encode(v); CHECK(Decode(b.data(), b.size(), o) && o.flags == f);
    }
    v.flags = 16; b = Encode(v); CHECK(!Decode(b.data(), b.size(), o));   // rev 24 takes 4 and 8 (kVoiceNoCut, kVoiceEveryone)

    Welcome w; w.flags = kWelcomeVoice | kWelcomeVoiceAll; Welcome wo;
    b = Encode(w); CHECK(Decode(b.data(), b.size(), wo) && wo.flags == 24);
    w.flags = 32; b = Encode(w); CHECK(!Decode(b.data(), b.size(), wo));

    RouteTableMsg t; t.epoch = 7;
    RouteKey k1; k1.peer = 2; k1.via = 3; for (size_t i = 0; i < k1.key.size(); ++i) k1.key[i] = uint8_t(0xA0 + i);
    RouteKey k2; k2.peer = 3; k2.via = kRouteViaDirect;
    RouteKey k3; k3.peer = 4; k3.via = kRouteViaHost;
    t.keys = {k1, k2, k3}; t.relays = {{2, 5}, {4, 8}};
    RouteTableMsg to;
    b = Encode(t);
    CHECK(Decode(b.data(), b.size(), to) && to.epoch == 7 && to.keys.size() == 3 && to.relays.size() == 2);
    CHECK(to.keys[0].peer == 2 && to.keys[0].via == 3 && to.keys[0].key == k1.key && to.keys[1].via == kRouteViaDirect);
    CHECK(to.relays[1].a == 4 && to.relays[1].b == 8);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, to));
    auto extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), to));
    auto bad = [&](RouteTableMsg m) { auto e = Encode(m); RouteTableMsg x; return !Decode(e.data(), e.size(), x); };
    RouteTableMsg m = t; m.keys[0].peer = 0; CHECK(bad(m));                       // the host is no pair end
    m = t; m.keys[0].peer = 9; CHECK(bad(m));
    m = t; m.keys[1].peer = 2; CHECK(bad(m));                                     // a peer twice
    m = t; m.keys[0].via = 2; CHECK(bad(m));                                      // relayed through itself
    m = t; m.keys[0].via = 9; CHECK(bad(m));
    m = t; m.relays[0] = {5, 2}; CHECK(bad(m));                                   // a < b
    m = t; m.relays[0] = {0, 2}; CHECK(bad(m));
    m = t; m.relays[0] = {3, 3}; CHECK(bad(m));
    m = t; m.relays[1] = m.relays[0]; CHECK(bad(m));                              // twice
    RouteTableMsg empty; b = Encode(empty); CHECK(Decode(b.data(), b.size(), to) && to.keys.empty() && to.relays.empty());

    ForwardMsg f; f.from = 1; f.to = 2; f.epoch = 3; f.seq = 0xFFFE; f.tag = 0x0123456789ABCDEFULL;
    f.payload = FakeStream(MsgType::Pose, 9, 40);
    ForwardMsg fo;
    b = Encode(f);
    CHECK(Decode(b.data(), b.size(), fo) && fo.from == 1 && fo.to == 2 && fo.epoch == 3 && fo.seq == 0xFFFE && fo.tag == f.tag && fo.payload == f.payload);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, fo));
    extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), fo));
    auto badF = [&](ForwardMsg x) { auto e = Encode(x); ForwardMsg y; return !Decode(e.data(), e.size(), y); };
    ForwardMsg g = f; g.payload = FakeStream(MsgType::Vitals, 1, 10); CHECK(!badF(g));
    g = f; g.payload = FakeStream(MsgType::Forward, 1, 10); CHECK(badF(g));      // never a Forward in a Forward
    g = f; g.payload = FakeStream(MsgType::HitClaim, 1, 10); CHECK(badF(g));     // poses and vitals only
    g = f; g.payload = {1, 2, 3, 4, 5}; CHECK(badF(g));                          // not a message
    g = f; g.payload.clear(); CHECK(badF(g));
    g = f; g.from = 0; CHECK(badF(g));
    g = f; g.to = 9; CHECK(badF(g));
    g = f; g.to = 1; CHECK(badF(g));
}

static void Fill(uint8_t* d, size_t n) { static uint8_t c = 1; for (size_t i = 0; i < n; ++i) d[i] = c++; }

// The design's lobby: A=1, B=2, C=3, D=4. A <-> B direct is slow; through C is fast.
static RouteLinks DesignLinks() {
    RouteLinks l;
    auto both = [&](uint8_t a, uint8_t b, uint16_t rtt) { l.Note(a, b, rtt, 0); l.Note(b, a, rtt, 0); };
    both(0, 1, 60); both(0, 2, 70); both(0, 3, 80); both(0, 4, 90);
    both(1, 2, 180); both(1, 3, 40); both(2, 3, 30); both(1, 4, 100); both(2, 4, 110); both(3, 4, 120);
    return l;
}

static void TestTables() {
    RoutePlanner plan; PairKeys keys;
    const std::vector<uint8_t> clients = {0, 1, 2, 3, 4};
    plan.Plan(DesignLinks(), clients, 0);
    for (uint32_t t = 1; t <= 3; ++t) plan.Plan(DesignLinks(), clients, t * 5000);
    CHECK(plan.RouteOf(1, 2) == 3);
    const RouteTableMsg ta = MakeRouteTable(1, clients, plan, keys, 5, Fill);
    const RouteTableMsg tb = MakeRouteTable(2, clients, plan, keys, 5, Fill);
    const RouteTableMsg tc = MakeRouteTable(3, clients, plan, keys, 5, Fill);
    CHECK(ta.epoch == 5 && ta.keys.size() == 3 && tc.relays.size() >= 1);
    const RouteKey* ab = nullptr; const RouteKey* ba = nullptr;
    for (const RouteKey& k : ta.keys) if (k.peer == 2) ab = &k;
    for (const RouteKey& k : tb.keys) if (k.peer == 1) ba = &k;
    CHECK(ab && ba && ab->via == 3 && ba->via == 3 && ab->key == ba->key);         // both ends share the pair's key
    DetourKey zero{}; CHECK(ab && ab->key != zero);
    for (const RouteKey& k : tc.keys) CHECK(k.via == kRouteDirect || k.via == kRouteHost || k.key != zero);
    bool cRelaysAB = false; for (const RoutePair& p : tc.relays) cRelaysAB |= p.a == 1 && p.b == 2;
    CHECK(cRelaysAB);
    // The relay never gets the pair's key: C's own entries are for its own pairs only.
    for (const RouteKey& k : tc.keys) CHECK(k.peer != 3 && (k.via == kRouteDirect || k.via == kRouteHost || k.key != ab->key));
    for (const RouteKey& k : ta.keys) if (k.via == kRouteDirect || k.via == kRouteHost) CHECK(k.key == zero);
    // The host and a slot not in the lobby get empty tables; a returning player gets a new key.
    CHECK(MakeRouteTable(0, clients, plan, keys, 5, Fill).keys.empty() && MakeRouteTable(6, clients, plan, keys, 5, Fill).keys.empty());
    const DetourKey old = ab->key;
    keys.Forget(2);
    CHECK(!keys.Has(1, 2));
    CHECK(keys.Get(1, 2, Fill) != old);
}

static void TestLobby() {
    RoutePlanner plan; PairKeys keys;
    const std::vector<uint8_t> clients = {0, 1, 2, 3, 4};
    plan.Plan(DesignLinks(), clients, 0);
    for (uint32_t t = 1; t <= 3; ++t) plan.Plan(DesignLinks(), clients, t * 5000);
    DetourClient A, B, C, D;
    A.SetTable(1, MakeRouteTable(1, clients, plan, keys, 1, Fill));
    B.SetTable(2, MakeRouteTable(2, clients, plan, keys, 1, Fill));
    C.SetTable(3, MakeRouteTable(3, clients, plan, keys, 1, Fill));
    D.SetTable(4, MakeRouteTable(4, clients, plan, keys, 1, Fill));
    CHECK(A.RelayFor(2) == 3 && B.RelayFor(1) == 3 && C.RelayPairs() >= 1);
    CHECK(A.RelayFor(3) == 0 && A.RelayFor(1) == 0 && A.RelayFor(0) == 0);
    {   // a table naming this client as its own relay (a confused host) sends nothing through itself
        RouteTableMsg odd; odd.epoch = 9;
        RouteKey k; k.peer = 2; k.via = 1; odd.keys.push_back(k);
        DetourClient self; self.SetTable(1, odd);
        CHECK(self.RelayFor(2) == 0);
    }

    const std::vector<uint8_t> pose = FakeStream(MsgType::Pose, 3, 64);
    ForwardMsg f;
    CHECK(A.MakeForward(2, pose, f) && f.from == 1 && f.to == 2 && f.epoch == 1);
    CHECK(!A.MakeForward(3, pose, f));   // A -> C is not a detour
    CHECK(A.MakeForward(2, pose, f));
    // Through the wire and C to B.
    auto wire = Encode(f); ForwardMsg atC; CHECK(Decode(wire.data(), wire.size(), atC));
    CHECK(C.MayRelay(1, atC, 100) == RelayVerdict::Ok);
    ForwardMsg atB; wire = Encode(atC); CHECK(Decode(wire.data(), wire.size(), atB));
    CHECK(B.Accept(3, atB) == DetourVerdict::Ok && atB.payload == pose);
    CHECK(B.Accept(3, atB) == DetourVerdict::Seq);                           // a replay
    // B -> A the other way, through C too.
    ForwardMsg back; CHECK(B.MakeForward(1, pose, back) && C.MayRelay(2, back, 100) == RelayVerdict::Ok && A.Accept(3, back) == DetourVerdict::Ok);

    // C (or anyone) cannot alter a Forward.
    ForwardMsg x; CHECK(A.MakeForward(2, pose, x));
    ForwardMsg y = x; y.payload[10] ^= 1; CHECK(B.Accept(3, y) == DetourVerdict::Tag);
    y = x; y.seq = uint16_t(y.seq + 5); CHECK(B.Accept(3, y) == DetourVerdict::Tag);
    y = x; y.tag ^= 1; CHECK(B.Accept(3, y) == DetourVerdict::Tag);
    y = x; y.epoch = 9; CHECK(B.Accept(3, y) == DetourVerdict::Epoch);
    CHECK(B.Accept(4, x) == DetourVerdict::Relay);                           // through a relay the table does not name
    CHECK(B.Accept(1, x) == DetourVerdict::Relay);                           // straight from A: that is the mesh's job
    CHECK(C.Accept(3, x) == DetourVerdict::NotForMe && D.Accept(3, x) == DetourVerdict::NotForMe);
    CHECK(B.Accept(3, x) == DetourVerdict::Ok);                              // the untouched one still passes
    // D forging A's stream through C: no key, wrong sender.
    ForwardMsg forged = x; forged.seq = uint16_t(x.seq + 1);
    CHECK(C.MayRelay(4, forged, 100) == RelayVerdict::Sender);
    CHECK(B.Accept(3, forged) == DetourVerdict::Tag);
    // C relays only what its table lists.
    ForwardMsg ad; ad.from = 1; ad.to = 4; ad.epoch = 1; ad.payload = pose;
    CHECK(C.MayRelay(1, ad, 100) == RelayVerdict::NotListed);
    CHECK(C.MayRelay(3, ForwardMsg{2, 3, 1, 1, 0, pose}, 100) == RelayVerdict::ForMe);
    CHECK(D.MayRelay(1, x, 100) == RelayVerdict::NotListed);

    // The budget: a burst passes, a flood is cut, time refills it.
    DetourClient C2; C2.SetTable(3, MakeRouteTable(3, clients, plan, keys, 1, Fill));
    int ok = 0;
    for (int i = 0; i < 200; ++i) ok += C2.MayRelay(1, x, 1000) == RelayVerdict::Ok;
    CHECK(ok == int(kDetourRelayBurst));
    CHECK(C2.MayRelay(1, x, 1000) == RelayVerdict::Budget);
    CHECK(C2.MayRelay(2, back, 1000) == RelayVerdict::Ok);                   // the other direction has its own
    CHECK(C2.MayRelay(1, x, 1100) == RelayVerdict::Ok);                      // 100 ms: 12 more
    ok = 0; for (int i = 0; i < 100; ++i) ok += C2.MayRelay(1, x, 1100) == RelayVerdict::Ok;
    CHECK(ok == 11);
    // Wrapped clock.
    DetourClient C3; C3.SetTable(3, MakeRouteTable(3, clients, plan, keys, 1, Fill));
    for (int i = 0; i < 100; ++i) C3.MayRelay(1, x, 0xFFFFFF00u);
    CHECK(C3.MayRelay(1, x, 0xFFFFFF00u) == RelayVerdict::Budget);
    CHECK(C3.MayRelay(1, x, 0x00000100u) == RelayVerdict::Ok);

    // A new table: Forwards still on their way with the old epoch are taken, until the new one's stream begins.
    ForwardMsg late; CHECK(A.MakeForward(2, pose, late));
    DetourClient Aold = A;   // A as it was: still signing with epoch 1
    A.SetTable(1, MakeRouteTable(1, clients, plan, keys, 2, Fill));
    B.SetTable(2, MakeRouteTable(2, clients, plan, keys, 2, Fill));
    C.SetTable(3, MakeRouteTable(3, clients, plan, keys, 2, Fill));
    CHECK(C.MayRelay(1, late, 200) == RelayVerdict::Ok && B.Accept(3, late) == DetourVerdict::Ok);
    ForwardMsg fresh; CHECK(A.MakeForward(2, pose, fresh) && fresh.epoch == 2 && B.Accept(3, fresh) == DetourVerdict::Ok);
    // A valid epoch-1 Forward arriving after the epoch-2 stream began: dropped.
    ForwardMsg stale; CHECK(Aold.MakeForward(2, pose, stale) && stale.epoch == 1);
    CHECK(B.Accept(3, stale) == DetourVerdict::Seq);
    // Two tables on, the first epoch is gone.
    B.SetTable(2, MakeRouteTable(2, clients, plan, keys, 3, Fill));
    CHECK(B.Accept(3, late) == DetourVerdict::Epoch);

    // C leaves: A stops detouring through it at once; B refuses anything still arriving through C.
    ForwardMsg stray; CHECK(A.MakeForward(2, pose, stray));
    A.Forget(3); B.Forget(3);
    CHECK(A.RelayFor(2) == 0 && !A.MakeForward(2, pose, stray));
    ForwardMsg s2 = stray; s2.epoch = 2;
    CHECK(B.Accept(3, s2) != DetourVerdict::Ok);
}

static void TestPlannerFallback() {
    // The relay's link breaks: the next plan moves the pair off it, the next table says so.
    RoutePlanner plan; PairKeys keys;
    const std::vector<uint8_t> clients = {0, 1, 2, 3};
    RouteLinks l = DesignLinks();
    plan.Plan(l, clients, 0);
    for (uint32_t t = 1; t <= 3; ++t) plan.Plan(l, clients, t * 5000);
    CHECK(plan.RouteOf(1, 2) == 3);
    l.Note(1, 3, 40, 80); l.Note(3, 1, 40, 80);   // 80 % loss on A <-> C
    plan.Plan(l, clients, 30000);
    CHECK(plan.RouteOf(1, 2) != 3);
    DetourClient A; A.SetTable(1, MakeRouteTable(1, clients, plan, keys, 4, Fill));
    CHECK(A.RelayFor(2) == 0);
}

int main() {
    TestSipHash(); TestWire(); TestTables(); TestLobby(); TestPlannerFallback();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_detour: ok\n");
    return 0;
}

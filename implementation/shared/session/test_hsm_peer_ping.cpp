// Per-player ping (session/hsm_peer_ping.h) and the PingTable message (rev 22).
#include "session/hsm_peer_ping.h"
#include "hsm_protocol.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    PeerPings p;
    CHECK(p.Due(1, 0) && !p.Pinged(1) && !p.Due(kMaxFighters, 0));
    PingSummary s = p.Summary(1, 0);
    CHECK(s.medianMs == kPingUnknown && s.counted == 0 && s.lossPct == 0);
    p.OnSent(1, 1000);
    CHECK(p.Pinged(1) && !p.Due(1, 2999) && p.Due(1, 3000));
    CHECK(p.Summary(1, 2000).counted == 0);                       // unanswered, not yet lost
    CHECK(p.OnPong(1, 1000, 1080) == 80);
    CHECK(p.OnPong(1, 1000, 1090) == kPingUnknown);               // a repeated pong
    CHECK(p.OnPong(1, 999, 1090) == kPingUnknown);                // never sent
    CHECK(p.OnPong(2, 1000, 1090) == kPingUnknown);               // sent to another slot
    s = p.Summary(1, 1100);
    CHECK(s.medianMs == 80 && s.minMs == 80 && s.maxMs == 80 && s.answered == 1 && s.counted == 1 && s.lossPct == 0);
    p.OnSent(1, 3000); p.OnSent(1, 5000); p.OnSent(1, 7000);
    CHECK(p.OnPong(1, 3000, 3040) == 40 && p.OnPong(1, 7000, 7200) == 200);
    s = p.Summary(1, 7300);                                       // 5000 unanswered for 2300 ms: not counted yet
    CHECK(s.counted == 3 && s.answered == 3 && s.medianMs == 80 && s.minMs == 40 && s.maxMs == 200);
    s = p.Summary(1, 8000);                                       // now lost
    CHECK(s.counted == 4 && s.answered == 3 && s.lossPct == 25);
    CHECK(p.OnPong(1, 5000, 5000 + kPeerPingMaxRttMs + 1) == kPingUnknown);   // far too late
    p.OnSent(1, 7000);                                            // same millisecond as the last: not remembered
    // the ring keeps the last kPeerPingKeep pings
    for (uint32_t i = 0; i < kPeerPingKeep; ++i) { p.OnSent(1, 20000 + i * 2000); p.OnPong(1, 20000 + i * 2000, 20000 + i * 2000 + 30); }
    s = p.Summary(1, 60000);
    CHECK(s.counted == kPeerPingKeep && s.lossPct == 0 && s.medianMs == 30);
    CHECK(p.OnPong(1, 1000, 1080) == kPingUnknown);               // gone from the ring
    p.Forget(1); CHECK(!p.Pinged(1) && p.Summary(1, 60000).counted == 0);
    p.OnSent(0, 10); p.Clear(); CHECK(!p.Pinged(0));
    p.OnSent(kMaxFighters, 10); CHECK(p.OnPong(kMaxFighters, 10, 20) == kPingUnknown);

    PongLimiter lim;
    CHECK(lim.Allow(2, 1000) && !lim.Allow(2, 1000 + kPeerPingGapMs / 2 - 1) && lim.Allow(2, 1000 + kPeerPingGapMs / 2));
    CHECK(lim.Allow(3, 1001) && !lim.Allow(kMaxFighters, 0));
    lim.Clear(); CHECK(lim.Allow(2, 1001));

    // PingTable round trip and rejects
    PingTableMsg m;
    m.entries = {{0, 45, 40, 0}, {2, 120, kPingUnknown, 25}, {uint8_t(kMaxFighters - 1), kPingUnknown, kPingUnknown, 100}};
    std::vector<uint8_t> b = Encode(m);
    PingTableMsg back;
    CHECK(Decode(b.data(), b.size(), back) && back.entries.size() == 3);
    CHECK(back.entries[1].slot == 2 && back.entries[1].rttMs == 120 && back.entries[1].steamMs == kPingUnknown && back.entries[1].lossPct == 25);
    CHECK(back.entries[2].slot == kMaxFighters - 1 && back.entries[2].lossPct == 100);
    PingTableMsg bad = m; bad.entries[1].slot = 0;                // a slot twice
    b = Encode(bad); CHECK(!Decode(b.data(), b.size(), back));
    bad = m; bad.entries[0].slot = uint8_t(kMaxPeerMap); b = Encode(bad); CHECK(!Decode(b.data(), b.size(), back));
    bad = m; bad.entries[0].lossPct = 101; b = Encode(bad); CHECK(!Decode(b.data(), b.size(), back));
    b = Encode(m); b.push_back(0); CHECK(!Decode(b.data(), b.size(), back));     // trailing byte
    b = Encode(m); b.pop_back(); CHECK(!Decode(b.data(), b.size(), back));       // short
    b = Encode(m); b[4] = uint8_t(kMaxPeerMap + 1); CHECK(!Decode(b.data(), b.size(), back));
    PingTableMsg big; for (uint8_t i = 0; i < kMaxPeerMap + 3; ++i) big.entries.push_back({uint8_t(i % kMaxPeerMap), 1, 1, 0});
    b = Encode(big); CHECK(b[4] == kMaxPeerMap);                  // capped on encode
    PingTableMsg empty; b = Encode(empty); CHECK(Decode(b.data(), b.size(), back) && back.entries.empty());

    if (g_fail) { std::printf("test_hsm_peer_ping: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_peer_ping: all passed\n");
    return 0;
}

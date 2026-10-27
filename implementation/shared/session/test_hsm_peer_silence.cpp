// Host-side peer silence monitor and the director's travel-sequence check on Ready.
#include "session/hsm_reconnect.h"
#include "session/hsm_director.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    PeerSilenceMonitor m;
    m.Seen(1, 1000); m.Seen(2, 1000);
    CHECK(m.Expired(5000).empty());
    m.Seen(2, 14000);                                   // peer 2 keeps talking
    auto e = m.Expired(1000 + PeerSilenceMonitor::kSilentMs);
    CHECK(e.size() == 1 && e[0] == 1);
    CHECK(m.Expired(1000 + PeerSilenceMonitor::kSilentMs + 1).empty());   // reported once
    m.Seen(3, 10); m.Forget(3);
    CHECK(m.Expired(14000 + PeerSilenceMonitor::kSilentMs).size() == 1);   // only peer 2 left, now silent too
    PeerSilenceMonitor w; w.Seen(4, 0xFFFFFF00u);       // clock wrap
    CHECK(w.Expired(0x200u).empty());

    HostDirector d;
    d.OnJoin(0, 0); d.OnJoin(1, 0); d.OnBlob(1, MakeBlobId(1, 1));
    d.OnLevel(true, 100, 7);                            // travel #7 is loading
    d.OnReady(1, 6, 120, 7);                            // a late Ready from travel #6
    CHECK(d.Due(150) == std::vector<uint8_t>({0}));     // not ready for this level
    d.OnReady(1, 7, 160, 7);
    CHECK(d.Due(170) == std::vector<uint8_t>({0, 1}));
    d.OnLevel(true, 200, 8); d.OnReady(1, 8, 210);      // the unchecked form (current seq 0) as before
    CHECK(d.Due(220) == std::vector<uint8_t>({0, 1}));
    CHECK(d.ReadySeq(1) == 8 && d.ReadySeq(0) == 0 && d.ReadySeq(9) == 0);    // what the lease gate compares (W6)
    d.OnLevel(true, 300, 9);
    d.OnReady(1, 8, 310, 9);                                                  // a late Ready from travel #8: noted, not current
    CHECK(d.ReadySeq(1) == 8);
    d.OnReady(1, 9, 320, 9);
    CHECK(d.ReadySeq(1) == 9);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_peer_silence ok");
    return 0;
}

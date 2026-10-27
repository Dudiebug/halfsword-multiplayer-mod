// Reconnect and host-leave. A member that drops mid-match keeps its slot for kReserveMs; only the same key claims
// it back, and only before expiry; each lapsed slot is reported once. The client monitor shows "reconnecting"
// after silence and returns to the menu exactly once. All clock math is checked across the uint32 wrap.
#include "session/hsm_reconnect.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using V = std::vector<uint8_t>;

static uint8_t Got(std::optional<uint8_t> o) { return o ? *o : 0xFF; }
static int CountRtm(const std::vector<HostLossAction>& acts) {
    int n = 0;
    for (HostLossAction a : acts) n += a == HostLossAction::ReturnToMenu;
    return n;
}

static void TestClaimBeforeExpiryAndDoubleClaim() {
    ReservationTable t;
    CHECK(t.Reserve(100, 2, 1000));
    CHECK(Got(t.Claim(100, 1000 + kReserveMs - 1)) == 2);      // last live millisecond
    CHECK(Got(t.Claim(100, 1000 + kReserveMs - 1)) == 0xFF);   // double claim: already consumed
    CHECK(t.size() == 0);
    CHECK(t.Expire(1000 + 10 * kReserveMs).empty());           // a claimed slot is never reported as lapsed
}

static void TestClaimAfterExpiry() {
    ReservationTable t;
    CHECK(t.Reserve(100, 2, 1000));
    CHECK(Got(t.Claim(100, 1000 + kReserveMs)) == 0xFF);       // exactly at expiry: too late
    CHECK(Got(t.Claim(100, 1000 + kReserveMs + 5)) == 0xFF);   // still not claimable after the lapse
    CHECK(t.Expire(1000 + kReserveMs) == V({2}));              // the lapse is reported after the failed claim
    CHECK(Got(t.Claim(100, 1000 + kReserveMs + 5)) == 0xFF);
}

static void TestWrongKey() {
    ReservationTable t;
    CHECK(t.Reserve(100, 2, 0));
    CHECK(Got(t.Claim(101, 10)) == 0xFF);                      // another member's key
    CHECK(Got(t.Claim(100 + (uint64_t(1) << 32), 10)) == 0xFF);  // same low bits, different 64-bit key
    CHECK(Got(t.Claim(100, 10)) == 2);                         // the real key still holds it
}

static void TestExpiryList() {
    ReservationTable t;
    CHECK(t.Reserve(1, 1, 0));
    CHECK(t.Reserve(2, 2, 10000));
    CHECK(t.Reserve(3, 3, 20000));
    CHECK(t.Expire(kReserveMs - 1).empty());
    CHECK(t.Expire(kReserveMs) == V({1}));                     // slot 1 lapses first
    CHECK(t.Expire(kReserveMs).empty());                       // reported once
    CHECK(t.Expire(70000) == V({2}));
    CHECK(t.Expire(80000) == V({3}));
    CHECK(t.size() == 0);

    ReservationTable u;                                         // several lapse in one call: ascending slots
    CHECK(u.Reserve(3, 3, 0));
    CHECK(u.Reserve(1, 1, 0));
    CHECK(u.Reserve(5, 5, 0));
    CHECK(u.Expire(kReserveMs) == V({1, 3, 5}));
}

static void TestRefreshAndSameKeyRules() {
    ReservationTable t;
    CHECK(t.Reserve(100, 2, 0));
    CHECK(t.Reserve(100, 2, 30000));                           // same key again: timer restarts
    CHECK(t.size() == 1);
    CHECK(t.Expire(kReserveMs).empty());
    CHECK(Got(t.Claim(100, kReserveMs + 29999)) == 2);
    ReservationTable u;
    CHECK(u.Reserve(100, 2, 0));
    CHECK(u.Reserve(100, 2, kReserveMs + 1));                  // lapsed but unreported: same key re-reserves
    CHECK(u.Expire(kReserveMs + 1).empty());                   // no despawn is due for a member that came back
    CHECK(Got(u.Claim(100, kReserveMs + 2)) == 2);
}

static void TestRejections() {
    ReservationTable t;
    CHECK(!t.Reserve(100, 0, 0));                              // host slot is never reserved
    CHECK(!t.Reserve(100, uint8_t(kMaxFighters), 0));          // out of range
    CHECK(t.Reserve(100, 2, 0));
    CHECK(!t.Reserve(101, 2, 0));                              // slot held by another live key
    CHECK(t.size() == 1 && Got(t.Claim(100, 1)) == 2);
}

static void TestCap() {
    ReservationTable t;
    for (uint8_t slot = 1; slot < kMaxFighters; ++slot) CHECK(t.Reserve(uint64_t(slot) * 10, slot, 0));
    CHECK(t.size() == kMaxReservations && kMaxReservations == 8);
    CHECK(!t.Reserve(999, 1, 0));                              // a ninth entry cannot fit (slot 1 is held too)
    CHECK(t.size() == kMaxReservations);
    CHECK(Got(t.Claim(30, 1)) == 3);                           // free one slot
    CHECK(t.Reserve(999, 3, 1));                               // the freed slot is reusable
    CHECK(t.size() == kMaxReservations);
}

static void TestLapsedEvictedByReserve() {
    ReservationTable t;
    for (uint8_t slot = 1; slot < kMaxFighters; ++slot) CHECK(t.Reserve(uint64_t(slot), slot, 0));
    CHECK(!t.Reserve(60, 1, kReserveMs));                       // slot 1 lapsed but Expire() has not reported it yet
    CHECK(t.size() == 0);                                      // the lapsed entries left the live table
    CHECK(t.Expire(kReserveMs) == V({1, 2, 3, 4, 5, 6, 7, 8}));   // all eight are reported exactly once
    CHECK(t.Reserve(60, 1, kReserveMs + 1));                    // after the report the slot is free
    CHECK(Got(t.Claim(60, kReserveMs + 2)) == 1);

    ReservationTable u;                                        // a Reserve of another slot evicts without blocking
    CHECK(u.Reserve(1, 1, 0));
    CHECK(u.Reserve(2, 2, 30000));
    CHECK(u.Reserve(3, 3, kReserveMs));                        // slot 1 lapsed; slot 3 is unaffected
    CHECK(u.size() == 2);
    CHECK(u.Expire(kReserveMs) == V({1}));
    CHECK(u.Expire(kReserveMs).empty());
}

static void TestWrapAround() {
    const uint32_t t0 = 0xFFFFF000u;
    ReservationTable t;
    CHECK(t.Reserve(100, 3, t0));
    CHECK(Got(t.Claim(100, uint32_t(t0 + kReserveMs - 1))) == 3);   // the sum wraps past 2^32 and is still live
    ReservationTable u;
    CHECK(u.Reserve(100, 3, t0));
    CHECK(u.Expire(uint32_t(t0 + kReserveMs - 1)).empty());
    CHECK(u.Expire(uint32_t(t0 + kReserveMs)) == V({3}));           // lapses exactly across the wrap
    CHECK(ElapsedAtLeast(t0, uint32_t(t0 + 5), 5));
    CHECK(!ElapsedAtLeast(t0, uint32_t(t0 + 4), 5));
    // Stamped 10 ms before the wrap (0xFFFFFFF6), checked just after it: exactly 10 ms elapsed, 9 ms is not enough.
    CHECK(ElapsedAtLeast(0xFFFFFFF6u, 0u, 10));
    CHECK(!ElapsedAtLeast(0xFFFFFFF6u, 0xFFFFFFFFu, 10));
}

static void TestResendOrder() {
    CHECK(ReconnectResend({}) == std::vector<ResendItem>({ResendItem::Welcome, ResendItem::MatchInfo}));
    ResendFacts all{true, true, true};
    CHECK(ReconnectResend(all) == std::vector<ResendItem>({ResendItem::Welcome, ResendItem::LoadoutBlobs,
                                                           ResendItem::CurrentTravel, ResendItem::LiveSpawns,
                                                           ResendItem::MatchInfo}));
    // Outside a fight level there is no Travel and no Spawns, even if stale state claims otherwise.
    ResendFacts menu{false, true, true};
    CHECK(ReconnectResend(menu) == std::vector<ResendItem>({ResendItem::Welcome, ResendItem::LoadoutBlobs,
                                                            ResendItem::MatchInfo}));
    ResendFacts fightNoSpawns{true, false, false};
    CHECK(ReconnectResend(fightNoSpawns) == std::vector<ResendItem>({ResendItem::Welcome, ResendItem::CurrentTravel,
                                                                     ResendItem::MatchInfo}));
}

static void TestMonitorSilenceToLostThenMenu() {
    HostLossMonitor m(0);
    CHECK(m.Tick(2999) == HostLossAction::None && m.link() == HostLink::Connected);
    CHECK(m.Tick(3000) == HostLossAction::ShowReconnecting && m.link() == HostLink::HostSilent);
    CHECK(m.Tick(3500) == HostLossAction::None);                // reconnecting shown once, not re-emitted
    CHECK(m.Tick(9999) == HostLossAction::None && m.link() == HostLink::HostSilent);
    CHECK(m.Tick(10000) == HostLossAction::ReturnToMenu && m.link() == HostLink::HostLost);
}

static void TestMonitorPacketRecovers() {
    HostLossMonitor m(0);
    CHECK(m.Tick(3000) == HostLossAction::ShowReconnecting);
    m.OnPacket(4000);
    CHECK(m.link() == HostLink::Connected);
    CHECK(m.Tick(6999) == HostLossAction::None);                // silence restarts from the packet
    CHECK(m.Tick(7000) == HostLossAction::ShowReconnecting);
    m.OnPacket(7500);
    CHECK(m.link() == HostLink::Connected);
    CHECK(m.Tick(10499) == HostLossAction::None);               // 2999 ms since the packet
    CHECK(m.Tick(10500) == HostLossAction::ShowReconnecting);   // 3000 ms
    CHECK(m.Tick(17499) == HostLossAction::None);               // 9999 ms, still below kLostMs
    CHECK(m.Tick(17500) == HostLossAction::ReturnToMenu);       // 10000 ms since the last packet
}

static void TestMonitorLateTickGoesToMenu() {
    HostLossMonitor m(0);
    CHECK(m.Tick(10000) == HostLossAction::ReturnToMenu);       // no ShowReconnecting when already lost
    CHECK(m.link() == HostLink::HostLost);
}

static void TestMonitorExplicitDisconnect() {
    HostLossMonitor m(0);
    m.OnPacket(500);
    CHECK(m.OnDisconnect(1000) == HostLossAction::ReturnToMenu);
    CHECK(m.OnDisconnect(1001) == HostLossAction::None);
    CHECK(m.Tick(99999) == HostLossAction::None);
    m.OnPacket(100000);                                          // a late packet does not undo a lost host
    CHECK(m.link() == HostLink::HostLost);

    HostLossMonitor s(0);
    CHECK(s.Tick(3000) == HostLossAction::ShowReconnecting);
    CHECK(s.OnDisconnect(3001) == HostLossAction::ReturnToMenu);  // disconnect while silent goes straight to menu
    CHECK(s.Tick(10000) == HostLossAction::None);
}

static void TestMonitorReturnsOnceAndResets() {
    HostLossMonitor m(0);
    std::vector<HostLossAction> acts;
    for (uint32_t t = 0; t <= 30000; t += 500) acts.push_back(m.Tick(t));
    acts.push_back(m.OnDisconnect(30001));
    acts.push_back(m.OnDisconnect(30002));
    CHECK(CountRtm(acts) == 1);
    m.Reset(40000);                                              // a fresh connection re-arms the monitor
    CHECK(m.link() == HostLink::Connected);
    CHECK(m.Tick(42999) == HostLossAction::None);
    CHECK(m.Tick(43000) == HostLossAction::ShowReconnecting);
}

static void TestMonitorWrap() {
    const uint32_t t0 = 0xFFFFF000u;
    HostLossMonitor m(t0);
    CHECK(m.Tick(uint32_t(t0 + HostLossMonitor::kSilentMs - 1)) == HostLossAction::None);
    CHECK(m.Tick(uint32_t(t0 + HostLossMonitor::kSilentMs)) == HostLossAction::ShowReconnecting);
    CHECK(m.Tick(uint32_t(t0 + HostLossMonitor::kLostMs)) == HostLossAction::ReturnToMenu);
}

// N3: an ENet client whose host ends the connection outside a fight level leaves; a Steam client leaves it to the lobby.
static void TestClientDisconnectAction() {
    CHECK(ClientDisconnectAction(false, true, true) == DisconnectAction::HostLossMonitor);    // in a fight: the monitor
    CHECK(ClientDisconnectAction(true, true, true) == DisconnectAction::HostLossMonitor);
    CHECK(ClientDisconnectAction(false, true, false) == DisconnectAction::Leave);             // welcomed, in the menu
    CHECK(ClientDisconnectAction(false, false, false) == DisconnectAction::Leave);            // the connect timed out
    CHECK(ClientDisconnectAction(false, false, true) == DisconnectAction::Leave);             // not welcomed in a map
    CHECK(ClientDisconnectAction(true, true, false) == DisconnectAction::Ignore);
    CHECK(ClientDisconnectAction(true, false, false) == DisconnectAction::Ignore);
}

// M5: a connected peer with no Hello within kHelloMs is reported once; a Hello or a disconnect ends the watch.
static void TestHelloWatch() {
    HelloWatch w;
    w.Connected(1, 1000); w.Connected(2, 1000); w.Connected(3, 1000);
    w.Hello(2);                                                                  // peer 2 spoke
    w.Forget(3);                                                                 // peer 3 left
    CHECK(w.Expired(1000 + HelloWatch::kHelloMs - 1).empty());
    const auto e = w.Expired(1000 + HelloWatch::kHelloMs);
    CHECK(e.size() == 1 && e[0] == 1);
    CHECK(w.Expired(1000 + 10 * HelloWatch::kHelloMs).empty() && w.size() == 0);   // reported once
    HelloWatch r;
    r.Connected(4, 0xFFFFFF00u);                                                 // clock wrap
    CHECK(r.Expired(0x100u).empty());
    CHECK(r.Expired(0xFFFFFF00u + HelloWatch::kHelloMs).size() == 1);
    r.Connected(5, 10); r.Connected(5, 9000);                                    // a reused id starts afresh
    CHECK(r.Expired(9000 + HelloWatch::kHelloMs - 1).empty());
    r.Clear(); CHECK(r.size() == 0);
}

// N6: a peer silent for 15 s is reported once; a packet resets its clock; a forgotten peer is never reported.
static void TestPeerSilence() {
    constexpr uint32_t S = PeerSilenceMonitor::kSilentMs;
    PeerSilenceMonitor m;
    m.Seen(1, 1000); m.Seen(2, 1000); m.Seen(3, 1000);                         // loaded and playing (Ready)
    m.Loaded(1, 1000); m.Loaded(2, 1000); m.Loaded(3, 1000);
    m.Seen(2, 5000);                                                             // peer 2 spoke again
    m.Forget(3);                                                                 // peer 3 left
    CHECK(m.Expired(1000 + S - 1, true).empty());
    const auto e = m.Expired(1000 + S, true);
    CHECK(e.size() == 1 && e[0] == 1);
    CHECK(m.Expired(5000 + S - 1, true).empty());
    const auto e2 = m.Expired(5000 + S, true);
    CHECK(e2.size() == 1 && e2[0] == 2);
    CHECK(m.Expired(1000 + 10 * S, true).empty());                               // reported once
    m.Seen(4, 0xFFFFFF00u); m.Loaded(4, 0xFFFFFF00u);                            // clock wrap
    CHECK(m.Expired(0x100u, true).empty() && m.Expired(0xFFFFFF00u + S, true).size() == 1);
    m.Seen(5, 10); m.Clear(); CHECK(m.Expired(10 + 10 * S, true).empty());
}

// Public #9: a client that joined and waits in the lobby menu sends nothing (0.7.0-pre3 clients have no keep-alive); the
// host dropped it 15 s after it joined, and the Brawl started with bots only.
static void TestPeerSilenceLobby() {
    constexpr uint32_t S = PeerSilenceMonitor::kSilentMs, L = PeerSilenceMonitor::kLoadingMs;
    PeerSilenceMonitor m;
    m.Seen(1, 1000);                                                             // Hello; then nothing in the menu
    CHECK(m.Expired(1000 + S, false).empty());
    CHECK(m.Expired(1000 + 20 * S, false).empty());                              // five minutes in the lobby
    // the fight starts: its silence counts from now, never from the Hello
    const uint32_t fight = 1000 + 20 * S;
    m.LevelLoaded(fight);
    CHECK(m.Expired(fight + S, true).empty());                                   // still loading on a slower machine
    m.Seen(1, fight + S + 100);                                                  // a packet it queued before the load
    CHECK(m.Expired(fight + S + 100 + S, true).empty());                         // keeps the load window
    m.Seen(1, fight + S + 500); m.Loaded(1, fight + S + 500);                    // its Ready: streaming now
    CHECK(m.Expired(fight + S + 500 + S - 1, true).empty());
    const auto e = m.Expired(fight + S + 500 + S, true);                         // then it froze
    CHECK(e.size() == 1 && e[0] == 1);
    // a client that never comes back from the load is dropped once the load window is over
    m.Seen(2, 100); m.Loaded(2, 100); m.LevelLoaded(200);
    CHECK(m.Expired(200 + L - 1, true).empty());
    CHECK(m.Expired(200 + L, true).size() == 1);
    // a client with the keep-alive is timed in the menu too: a crashed one frees its slot there
    m.Seen(3, 5000, true); m.Seen(3, 6000); m.Loaded(3, 6000);                   // a later message without the flag keeps it
    CHECK(m.Expired(6000 + S - 1, false).empty());
    const auto e3 = m.Expired(6000 + S, false);
    CHECK(e3.size() == 1 && e3[0] == 3);
    // without the keep-alive, the fight's silence counts from the last menu tick, never from the Hello
    m.Seen(4, 0); m.Loaded(4, 0); m.Expired(S - 1, false);
    CHECK(m.Expired(2 * S - 2, true).empty() && m.Expired(2 * S - 1, true).size() == 1);
}

static void TestPeerSilenceJoinAndSpectate() {
    constexpr uint32_t S = PeerSilenceMonitor::kSilentMs, L = PeerSilenceMonitor::kLoadingMs;
    // a client that joins mid-fight loads the arena after its Hello: the load window, not 15 s (public #8, #10)
    PeerSilenceMonitor m;
    m.Seen(1, 1000, true);                                                       // Hello, then its first Ping, then the load
    CHECK(m.Expired(1000 + S, true).empty());
    CHECK(m.Expired(1000 + L - 1, true).empty());
    CHECK(m.Expired(1000 + L, true).size() == 1);                                // it never came back
    m.Seen(2, 0, true); m.Loaded(2, 30000);                                      // Ready after 30 s: timed from it
    CHECK(m.Expired(30000 + S - 1, true).empty() && m.Expired(30000 + S, true).size() == 1);
    // a Ready only ends the load it answers: a new level load restarts the window
    m.Seen(3, 0, true); m.Loaded(3, 0); m.LevelLoaded(100);
    CHECK(m.Expired(100 + S, true).empty() && m.Expired(100 + L, true).size() == 1);
    // an older client (no keep-alive) that spectates sends nothing: not timed while it has no live fighter
    bool alive = false;
    const auto streams = [&](uint32_t) { return alive; };
    m.Seen(4, 0); m.Loaded(4, 0);
    CHECK(m.Expired(S, true, streams).empty());
    CHECK(m.Expired(10 * S, true, streams).empty());                             // a whole round spectating
    alive = true;                                                                // respawned: its clock starts now
    CHECK(m.Expired(10 * S + S - 1, true, streams).empty());
    CHECK(m.Expired(11 * S, true, streams).size() == 1);                         // and then it went silent
    // one with the keep-alive is timed whatever `streams` says
    m.Seen(5, 0, true); m.Loaded(5, 0);
    alive = false;
    CHECK(m.Expired(S, true, streams).size() == 1);
}

int main() {
    TestClientDisconnectAction(); TestHelloWatch(); TestPeerSilence(); TestPeerSilenceLobby(); TestPeerSilenceJoinAndSpectate();
    TestClaimBeforeExpiryAndDoubleClaim(); TestClaimAfterExpiry(); TestWrongKey(); TestExpiryList();
    TestRefreshAndSameKeyRules(); TestRejections(); TestCap(); TestLapsedEvictedByReserve(); TestWrapAround();
    TestResendOrder(); TestMonitorSilenceToLostThenMenu(); TestMonitorPacketRecovers(); TestMonitorLateTickGoesToMenu();
    TestMonitorExplicitDisconnect(); TestMonitorReturnsOnceAndResets(); TestMonitorWrap();
    std::printf(g_fail ? "%d FAILED\n" : "all reconnect tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

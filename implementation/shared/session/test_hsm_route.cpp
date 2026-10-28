// Detour route planner (session/hsm_route.h, step R0: log only).
#include "session/hsm_route.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

// Both ends measured the same round trip.
static void Both(RouteLinks& l, uint8_t a, uint8_t b, uint16_t rtt, uint8_t loss = 0) { l.Note(a, b, rtt, loss); l.Note(b, a, rtt, loss); }

// The design page's lobby: 1 and 2 peer badly (180 ms), 3 is near both, 4 is no help.
static RouteLinks Lobby() {
    RouteLinks l;
    Both(l, 1, 0, 60); Both(l, 2, 0, 70); Both(l, 3, 0, 80); Both(l, 4, 0, 100);
    Both(l, 1, 2, 180); Both(l, 1, 3, 30); Both(l, 3, 2, 40); Both(l, 1, 4, 55); Both(l, 4, 2, 95); Both(l, 3, 4, 50);
    return l;
}

static const RouteChange* Find(const std::vector<RouteChange>& v, uint8_t a, uint8_t b) {
    for (const RouteChange& c : v) if (c.a == a && c.b == b) return &c;
    return nullptr;
}

int main() {
    // hop costs
    {
        RouteLinks l;
        CHECK(l.HopMs(1, 2) == kRouteNone);
        l.Note(1, 2, 100, 0);
        CHECK(l.HopMs(1, 2) == 50 && l.HopMs(2, 1) == 50);         // one end's figure serves both directions
        l.Note(2, 1, 60, 0);
        CHECK(l.HopMs(1, 2) == 40);                                 // the mean of both ends
        l.Note(2, 1, kPingUnknown, 10);
        CHECK(l.HopMs(1, 2) == 50 + 10 * kRouteLossMsPerPct);       // unanswered end: its loss still counts
        l.Note(2, 1, 60, kRouteMaxLossPct);
        CHECK(l.HopMs(1, 2) == 40 + kRouteMaxLossPct * kRouteLossMsPerPct);
        l.Note(2, 1, 60, kRouteMaxLossPct + 1);
        CHECK(l.HopMs(1, 2) == kRouteNone);                         // too lossy
        l.Note(1, 2, kPingUnknown, 0); l.Note(2, 1, kPingUnknown, 0);
        CHECK(l.HopMs(1, 2) == kRouteNone);
        CHECK(l.HopMs(1, 1) == kRouteNone && l.HopMs(1, kMaxFighters) == kRouteNone);
        l.Note(1, 1, 10, 0); l.Note(kMaxFighters, 1, 10, 0);        // ignored
        Both(l, 3, 4, 20); l.ClearRow(3);
        CHECK(l.HopMs(3, 4) == 10);                                 // 4's figure stays
        l.Forget(4); CHECK(l.HopMs(3, 4) == kRouteNone);
        Both(l, 5, 6, 20); l.Clear(); CHECK(l.HopMs(5, 6) == kRouteNone);
        CHECK(RouteAdd(1, 2) == 3 && RouteAdd(kRouteNone, 2) == kRouteNone && RouteAdd(2, kRouteNone) == kRouteNone);
    }
    // path costs (the design page's table)
    {
        const RouteLinks l = Lobby();
        CHECK(RoutePathMs(l, 1, 2, kRouteDirect) == 90);
        CHECK(RoutePathMs(l, 1, 2, kRouteHost) == 30 + 35 + kRouteRelayMs);
        CHECK(RoutePathMs(l, 1, 2, 3) == 15 + 20 + kRouteRelayMs);
        CHECK(RoutePathMs(l, 1, 2, 4) == 27 + 47 + kRouteRelayMs);
        CHECK(RoutePathMs(l, 2, 1, 3) == RoutePathMs(l, 1, 2, 3));
        CHECK(RoutePathMs(l, 1, 2, 1) == kRouteNone && RoutePathMs(l, 1, 2, 2) == kRouteNone);
        CHECK(RoutePathMs(l, 1, 2, 5) == kRouteNone);               // nobody measured slot 5
        CHECK(RoutePathMs(l, 1, 2, kMaxFighters) == kRouteNone);
    }
    // first plan: direct or host; the detour after two plans in a row; then it holds
    {
        RouteLinks l = Lobby();
        RoutePlanner p;
        const std::vector<uint8_t> clients{0, 1, 2, 3, 4};
        std::vector<RouteChange> c = p.Plan(l, clients, 1000);
        CHECK(c.size() == 6);                                       // every pair is new
        const RouteChange* x = Find(c, 1, 2);
        CHECK(x && x->why == RouteWhy::New && x->to == kRouteHost && x->directMs == 90 && x->hostMs == 73 && x->toMs == 73);
        CHECK(Find(c, 1, 3) && Find(c, 1, 3)->to == kRouteDirect);
        CHECK(!Find(c, 0, 1));                                      // the host is no pair member
        CHECK(p.RouteOf(1, 2) == kRouteHost && p.RouteOf(2, 1) == kRouteHost && p.RouteOf(1, 3) == kRouteDirect);
        c = p.Plan(l, clients, 11000);
        CHECK(c.empty());                                           // once is not enough
        c = p.Plan(l, clients, 21000);
        CHECK(c.size() == 1);
        x = Find(c, 1, 2);
        CHECK(x && x->why == RouteWhy::Faster && x->from == kRouteHost && x->to == 3 && x->toMs == 43 && x->hostMs == 73);
        CHECK(p.RouteOf(2, 1) == 3 && p.Load(3) == 1 && p.Load(4) == 0);
        CHECK(p.Plan(l, clients, 31000).empty());
        // 3 starts losing pings to 1: back to the best plain route at once, hold or not
        Both(l, 1, 3, 30, 50);
        c = p.Plan(l, clients, 32000);
        x = Find(c, 1, 2);
        CHECK(x && x->why == RouteWhy::Lost && x->from == 3 && x->to == kRouteHost && p.Load(3) == 0);
        CHECK(Find(c, 1, 3) && Find(c, 1, 3)->why == RouteWhy::Lost);   // the direct hop itself is gone
        CHECK(p.RouteOf(1, 3) == kRouteHost);
        // healthy again: held until 42000, then two plans
        Both(l, 1, 3, 30);
        CHECK(p.Plan(l, clients, 35000).empty());
        CHECK(p.Plan(l, clients, 40000).empty());
        CHECK(p.Plan(l, clients, 43000).empty());                   // first plan after the hold
        c = p.Plan(l, clients, 53000);
        CHECK(Find(c, 1, 2) && Find(c, 1, 2)->to == 3 && Find(c, 1, 3) && Find(c, 1, 3)->to == kRouteDirect);
        // 3 leaves the lobby: back at once
        const std::vector<uint8_t> without3{1, 2, 4};
        c = p.Plan(l, without3, 54000);
        x = Find(c, 1, 2);
        CHECK(x && x->why == RouteWhy::Lost && x->to == kRouteHost && c.size() == 1);
        // 3 is back but forgotten: its pairs start over
        p.Forget(3);
        CHECK(p.RouteOf(1, 3) == kRouteHost);
        c = p.Plan(l, clients, 55000);
        CHECK(c.size() == 3 && Find(c, 1, 3) && Find(c, 1, 3)->why == RouteWhy::New && !Find(c, 1, 2));
        p.Clear(); CHECK(p.RouteOf(1, 3) == kRouteHost && p.Load(3) == 0);
    }
    // hysteresis: a small saving is not worth a change
    {
        RouteLinks l;
        Both(l, 1, 0, 60); Both(l, 2, 0, 60);                       // host: 30 + 30 + 8 = 68
        Both(l, 1, 2, 300);                                         // direct 150
        Both(l, 1, 3, 40); Both(l, 3, 2, 42); Both(l, 3, 0, 60);    // via 3: 20 + 21 + 8 = 49: saves 19 (more than 15% of 68)
        RoutePlanner p;
        const std::vector<uint8_t> clients{1, 2, 3};
        p.Plan(l, clients, 0); p.Plan(l, clients, 10000);
        CHECK(p.Plan(l, clients, 20000).size() == 1 && p.RouteOf(1, 2) == 3);
        RouteLinks m;
        Both(m, 1, 0, 60); Both(m, 2, 0, 60); Both(m, 1, 2, 300); Both(m, 3, 0, 60);
        Both(m, 1, 3, 60); Both(m, 3, 2, 62);                       // via 3: 30 + 31 + 8 = 69: no saving
        RoutePlanner q;
        q.Plan(m, clients, 0); q.Plan(m, clients, 10000); q.Plan(m, clients, 20000);
        CHECK(q.RouteOf(1, 2) == kRouteHost);
        Both(m, 1, 3, 44); Both(m, 3, 2, 44);                       // via 3: 22 + 22 + 8 = 52: saves 16 < max(10, 10)? no: saves
        q.Plan(m, clients, 30000); q.Plan(m, clients, 40000);
        CHECK(q.RouteOf(1, 2) == 3);
        // a long path: 15% is more than 10 ms
        RouteLinks h;
        Both(h, 1, 0, 400); Both(h, 2, 0, 400); Both(h, 1, 2, 2000); Both(h, 3, 0, 400);   // host 408
        Both(h, 1, 3, 360); Both(h, 3, 2, 360);                     // via 3: 368: saves 40 < 61
        RoutePlanner r;
        r.Plan(h, clients, 0); r.Plan(h, clients, 10000); r.Plan(h, clients, 20000);
        CHECK(r.RouteOf(1, 2) == kRouteHost);
        Both(h, 1, 3, 320); Both(h, 3, 2, 320);                     // via 3: 328: saves 80 >= 61
        r.Plan(h, clients, 30000); r.Plan(h, clients, 40000);
        CHECK(r.RouteOf(1, 2) == 3);
        // exactly the minimum: 10 ms on a 60 ms path
        RouteLinks e;
        Both(e, 1, 0, 52); Both(e, 2, 0, 52); Both(e, 1, 2, 300); Both(e, 3, 0, 52);       // host 26 + 26 + 8 = 60
        Both(e, 1, 3, 42); Both(e, 3, 2, 42);                       // via 3: 50
        RoutePlanner s;
        s.Plan(e, clients, 0); s.Plan(e, clients, 10000); s.Plan(e, clients, 20000);
        CHECK(s.RouteOf(1, 2) == 3);
        RouteLinks f = e;
        Both(f, 1, 3, 44);                                          // via 3: 22 + 21 + 8 = 51: saves 9
        RoutePlanner t;
        t.Plan(f, clients, 0); t.Plan(f, clients, 10000); t.Plan(f, clients, 20000);
        CHECK(t.RouteOf(1, 2) == kRouteHost);
    }
    // two plans in a row for the same candidate
    {
        RouteLinks l = Lobby();
        RoutePlanner p;
        const std::vector<uint8_t> clients{1, 2, 3, 4};
        p.Plan(l, clients, 0);
        p.Plan(l, clients, 10000);                                  // 3 once
        RouteLinks m = l;
        Both(m, 1, 3, 400); Both(m, 1, 4, 10); Both(m, 4, 2, 10);   // now 4 is the best: via 4 = 5 + 5 + 8
        CHECK(p.Plan(m, clients, 20000).empty());                   // 4 once: 3's count does not carry over
        CHECK(p.Plan(l, clients, 30000).empty());                   // 3 once again
        CHECK(p.Plan(l, clients, 40000).size() == 1 && p.RouteOf(1, 2) == 3);
    }
    // load cap: one relay serves at most maxRelayPairs pairs
    {
        RouteLinks l = Lobby();
        Both(l, 2, 4, 300);                                         // 2 <-> 4: direct 150, host 93, via 3 53
        const std::vector<uint8_t> clients{1, 2, 3, 4};
        RoutePlanner one(1);
        for (uint32_t t = 0; t <= 20000; t += 10000) one.Plan(l, clients, t);
        CHECK(one.RouteOf(1, 2) == 3 && one.RouteOf(2, 4) == kRouteHost && one.Load(3) == 1);
        RoutePlanner two;
        for (uint32_t t = 0; t <= 20000; t += 10000) two.Plan(l, clients, t);
        CHECK(two.RouteOf(1, 2) == 3 && two.RouteOf(2, 4) == 3 && two.Load(3) == 2);
        RoutePlanner none(0);
        for (uint32_t t = 0; t <= 20000; t += 10000) none.Plan(l, clients, t);
        CHECK(none.RouteOf(1, 2) == kRouteHost && none.Load(3) == 0);
    }
    // nothing measured: the host relay, never a change
    {
        RouteLinks l;
        RoutePlanner p;
        const std::vector<uint8_t> clients{1, 2, 3};
        std::vector<RouteChange> c = p.Plan(l, clients, 0);
        CHECK(c.size() == 3 && c[0].to == kRouteHost && c[0].toMs == kRouteNone && c[0].directMs == kRouteNone);
        CHECK(p.Plan(l, clients, 10000).empty() && p.Plan(l, clients, 20000).empty());
        // the host link unknown, a detour known: taken (anything beats nothing)
        Both(l, 1, 3, 40); Both(l, 3, 2, 40);
        p.Plan(l, clients, 30000);
        c = p.Plan(l, clients, 40000);
        CHECK(Find(c, 1, 2) && Find(c, 1, 2)->to == 3);
        CHECK(Find(c, 1, 3) && Find(c, 1, 3)->to == kRouteDirect && p.RouteOf(1, 3) == kRouteDirect);   // direct known, host not
        CHECK(p.RouteOf(9, 1) == kRouteHost && p.RouteOf(1, 1) == kRouteHost);
        p.Forget(kMaxFighters);
    }
    // a clock that wraps during the hold
    {
        RouteLinks l = Lobby();
        RoutePlanner p;
        const std::vector<uint8_t> clients{1, 2, 3, 4};
        const uint32_t t0 = 0xFFFFFFFFu - 25000;
        p.Plan(l, clients, t0); p.Plan(l, clients, t0 + 10000);
        CHECK(p.Plan(l, clients, t0 + 20000).size() == 1);          // via 3, held until past the wrap
        Both(l, 1, 3, 30, 50);
        CHECK(Find(p.Plan(l, clients, t0 + 21000), 1, 2));          // lost: host, held again across the wrap
        Both(l, 1, 3, 30);
        CHECK(p.Plan(l, clients, t0 + 26000).empty() && p.Plan(l, clients, t0 + 30000).empty());   // still held
        p.Plan(l, clients, t0 + 32000);
        CHECK(Find(p.Plan(l, clients, t0 + 42000), 1, 2));
    }
    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::printf("test_hsm_route: ok\n");
    return 0;
}

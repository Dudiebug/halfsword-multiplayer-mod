// Unit tests for hsm_linksim.h. Run via implementation/shared/run_tests.sh test_hsm_linksim.
#include "sync/hsm_linksim.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;

struct Pkt { int id = -1; };

// Arrival of every packet: sends one packet per ms for `count` ms, polls each 0.5 ms up to `drainMs`.
struct Arrival { int id; double atMs; };
static std::vector<Arrival> Run(LinkSim<Pkt, 4096>& link, int count, double drainMs) {
    std::vector<Arrival> out;
    int next = 0;
    for (double t = 0.0; t <= drainMs; t += 0.5) {
        if (next < count && t >= next) { link.Send(Pkt{next}, t); ++next; }
        Pkt p;
        while (link.Receive(t, p)) out.push_back(Arrival{p.id, t});
    }
    return out;
}

// 1. An ideal link (all zero) hands each packet back at once, in order.
static void TestIdealLink() {
    LinkSim<Pkt, 8> link;
    for (int i = 0; i < 5; ++i) CHECK(link.Send(Pkt{i}, 10.0));
    Pkt p;
    for (int i = 0; i < 5; ++i) { CHECK(link.Receive(10.0, p)); CHECK(p.id == i); }
    CHECK(!link.Receive(10.0, p));
    CHECK(link.Stats().sent == 5 && link.Stats().delivered == 5 && link.Stats().lost == 0 && link.Stats().overflow == 0);
}

// 2. A fixed delay holds a packet until exactly sent + delay, then releases it.
static void TestFixedDelay() {
    LinkSim<Pkt, 8> link(LinkConfig{ 80.0, 0.0, 0.0, 7 });
    CHECK(link.Send(Pkt{1}, 100.0));
    Pkt p;
    CHECK(!link.Receive(179.999, p));
    CHECK(link.InFlight() == 1);
    CHECK(link.Receive(180.0, p) && p.id == 1);
    CHECK(link.InFlight() == 0);
}

// 3. Jitter: every arrival lies in [delay - jitter, delay + jitter] (+ the 0.5 ms poll step), both ends of that range
// are actually used, and packets reorder.
static void TestJitterBoundsAndReorder() {
    LinkSim<Pkt, 4096> link(LinkConfig{ 80.0, 20.0, 0.0, 42 });
    std::vector<Arrival> a = Run(link, 2000, 2200.0);
    CHECK(a.size() == 2000);
    double lo = 1e9, hi = -1e9;
    int reorders = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = a[i].atMs - a[i].id;   // packet id = send time
        lo = std::min(lo, d); hi = std::max(hi, d);
        if (i > 0 && a[i].id < a[i - 1].id) ++reorders;
    }
    CHECK(lo >= 60.0 && hi <= 100.5);
    CHECK(lo < 61.0 && hi > 99.0);
    CHECK(reorders > 100);
}

// 4. A jitter larger than the delay never delivers a packet before it was sent.
static void TestNoNegativeDelay() {
    LinkSim<Pkt, 4096> link(LinkConfig{ 5.0, 20.0, 0.0, 3 });
    std::vector<Arrival> a = Run(link, 1000, 1100.0);
    CHECK(a.size() == 1000);
    int atSend = 0;
    for (const Arrival& x : a) { CHECK(x.atMs >= x.id); if (x.atMs - x.id < 0.5) ++atSend; }
    CHECK(atSend > 100);   // the floor at 0 is reached (about a third of the packets)

    // Polled a moment before its send time, no packet is there yet -- whatever its draw (about a third of these
    // would be due before they were sent without the floor).
    LinkSim<Pkt, 8> early(LinkConfig{ 5.0, 20.0, 0.0, 5 });
    int heldEarly = 0, deliveredAfter = 0;
    Pkt p;
    for (int i = 0; i < 1000; ++i) {
        const double t = 100.0 * i;
        CHECK(early.Send(Pkt{i}, t));
        if (!early.Receive(t - 0.25, p)) ++heldEarly;
        if (early.Receive(t + 30.0, p) && p.id == i) ++deliveredAfter;
    }
    CHECK(heldEarly == 1000);
    CHECK(deliveredAfter == 1000);
}

// 5. Loss rate: 2 % over 100k packets lands within 0.15 points of 2 %; 0 % loses none; 100 % loses all.
static void TestLossRate() {
    LinkSim<Pkt, 4> link(LinkConfig{ 0.0, 0.0, 2.0, 99 });
    Pkt p;
    for (int i = 0; i < 100000; ++i) { link.Send(Pkt{i}, i); while (link.Receive(i, p)) {} }
    const double pct = 100.0 * double(link.Stats().lost) / double(link.Stats().sent);
    CHECK(pct > 1.85 && pct < 2.15);
    CHECK(link.Stats().delivered + link.Stats().lost == 100000);

    LinkSim<Pkt, 4> none(LinkConfig{ 0.0, 0.0, 0.0, 99 });
    LinkSim<Pkt, 4> all(LinkConfig{ 0.0, 0.0, 100.0, 99 });
    for (int i = 0; i < 1000; ++i) {
        CHECK(none.Send(Pkt{i}, i)); while (none.Receive(i, p)) {}
        CHECK(!all.Send(Pkt{i}, i));
    }
    CHECK(none.Stats().lost == 0 && all.Stats().lost == 1000 && !all.Receive(1e9, p));
}

// 6. Same config and seed = identical arrivals; another seed differs.
static void TestDeterministic() {
    LinkSim<Pkt, 4096> a(LinkConfig{ 80.0, 20.0, 2.0, 5 }), b(LinkConfig{ 80.0, 20.0, 2.0, 5 }), c(LinkConfig{ 80.0, 20.0, 2.0, 6 });
    std::vector<Arrival> ra = Run(a, 1000, 1200.0), rb = Run(b, 1000, 1200.0), rc = Run(c, 1000, 1200.0);
    CHECK(ra.size() == rb.size());
    bool same = ra.size() == rb.size(), differs = ra.size() != rc.size();
    for (size_t i = 0; same && i < ra.size(); ++i) same = ra[i].id == rb[i].id && ra[i].atMs == rb[i].atMs;
    for (size_t i = 0; !differs && i < ra.size(); ++i) differs = ra[i].id != rc[i].id || ra[i].atMs != rc[i].atMs;
    CHECK(same);
    CHECK(differs);
}

// 7. Bounded: past Cap in flight, packets are refused and counted; draining frees room again.
static void TestCapacity() {
    LinkSim<Pkt, 4> link(LinkConfig{ 100.0, 0.0, 0.0, 1 });
    for (int i = 0; i < 4; ++i) CHECK(link.Send(Pkt{i}, 0.0));
    CHECK(!link.Send(Pkt{4}, 0.0));
    CHECK(link.Stats().overflow == 1 && link.InFlight() == 4);
    Pkt p;
    CHECK(link.Receive(100.0, p) && p.id == 0);
    CHECK(link.Send(Pkt{5}, 1.0));
    int n = 0;
    while (link.Receive(1000.0, p)) ++n;
    CHECK(n == 4 && link.InFlight() == 0);
}

// 8. Equal due times come out in send order; earlier due beats earlier send.
static void TestOrdering() {
    LinkSim<Pkt, 8> link(LinkConfig{ 10.0, 0.0, 0.0, 1 });
    link.Send(Pkt{0}, 5.0);   // due 15
    link.Send(Pkt{1}, 5.0);   // due 15
    link.Send(Pkt{2}, 2.0);   // due 12
    Pkt p;
    CHECK(link.Receive(20.0, p) && p.id == 2);
    CHECK(link.Receive(20.0, p) && p.id == 0);
    CHECK(link.Receive(20.0, p) && p.id == 1);
}

// 9. Bad settings: NaN / negative become 0, loss above 100 is 100.
static void TestSanitize() {
    const LinkConfig c = SanitizeLink(LinkConfig{ std::nan(""), -5.0, 250.0, 1 });
    CHECK(c.delayMs == 0.0 && c.jitterMs == 0.0 && c.lossPct == 100.0);
    const LinkConfig d = SanitizeLink(LinkConfig{ INFINITY, 3.0, -1.0, 1 });
    CHECK(d.delayMs == 0.0 && d.jitterMs == 3.0 && d.lossPct == 0.0);
    LinkSim<Pkt, 4> link(LinkConfig{ -10.0, std::nan(""), std::nan(""), 1 });
    Pkt p;
    CHECK(link.Send(Pkt{1}, 3.0) && link.Receive(3.0, p) && p.id == 1);
}

// 10. Clear drops what is in flight.
static void TestClear() {
    LinkSim<Pkt, 4> link(LinkConfig{ 50.0, 0.0, 0.0, 1 });
    link.Send(Pkt{1}, 0.0);
    link.Clear();
    Pkt p;
    CHECK(link.InFlight() == 0 && !link.Receive(1000.0, p));
}

int main() {
    TestIdealLink();
    TestFixedDelay();
    TestJitterBoundsAndReorder();
    TestNoNegativeDelay();
    TestLossRate();
    TestDeterministic();
    TestCapacity();
    TestOrdering();
    TestSanitize();
    TestClear();
    if (g_fail) { std::printf("test_hsm_linksim: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_linksim: all passed\n");
    return 0;
}

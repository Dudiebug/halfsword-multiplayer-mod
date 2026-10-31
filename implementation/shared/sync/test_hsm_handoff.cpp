// Hand-off: the wire, the lease table, the host's write check and the owner's answer to an ask.
// Run via implementation/shared/run_tests.sh test_hsm_handoff.
#include "sync/hsm_handoff.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::sync;

static LeaseEntry Entry(uint8_t e, uint8_t epoch, uint8_t holder, uint8_t by = 0, uint8_t why = 0) {
    LeaseEntry x; x.entity = e; x.lease.epoch = epoch; x.lease.holder = holder; x.lease.by = by; x.lease.why = why;
    return x;
}

static void TestWire() {
    HandOffMsg m; m.entries = {Entry(2, 3, 0, 1, kCoupleGrab), Entry(9, 254, kNoSlot)};
    auto b = Encode(m);
    HandOffMsg o;
    CHECK(Decode(b.data(), b.size(), o) && !o.full && o.entries.size() == 2);
    CHECK(o.entries[0].entity == 2 && o.entries[0].lease.epoch == 3 && o.entries[0].lease.holder == 0 && o.entries[0].lease.by == 1 &&
          o.entries[0].lease.why == kCoupleGrab && o.entries[1].lease.holder == kNoSlot && o.entries[1].lease.epoch == 254);
    auto c = b; c.pop_back(); CHECK(!Decode(c.data(), c.size(), o));
    c = b; c.push_back(0); CHECK(!Decode(c.data(), c.size(), o));
    auto bad = [](LeaseEntry e) { HandOffMsg x; x.entries = {e}; auto w = Encode(x); HandOffMsg y; return !Decode(w.data(), w.size(), y); };
    CHECK(bad(Entry(uint8_t(kMaxFighters + 1), 1, kNoSlot)));          // a bot is never handed
    CHECK(bad(Entry(0, 1, kNoSlot)));
    CHECK(bad(Entry(3, 1, 2, 1, kCoupleGrab)));                         // to its own slot
    CHECK(bad(Entry(3, 1, uint8_t(kMaxFighters), 1, kCoupleGrab)));     // no such slot
    CHECK(bad(Entry(3, 1, 0, 3, kCoupleGrab)));                         // coupled to itself
    CHECK(bad(Entry(3, 1, 0, 1, 4)));                                   // unknown coupling
    HandOffMsg f; f.full = true; b = Encode(f); CHECK(Decode(b.data(), b.size(), o) && o.full && o.entries.empty());

    HandOffAskMsg a; a.victim = 3; a.by = 12; a.why = kCoupleStuck;
    b = Encode(a);
    HandOffAskMsg ao;
    CHECK(Decode(b.data(), b.size(), ao) && ao.victim == 3 && ao.by == 12 && ao.why == kCoupleStuck);
    a.why = 0; b = Encode(a); CHECK(!Decode(b.data(), b.size(), ao));
    a.why = kCoupleGrab; a.by = 3; b = Encode(a); CHECK(!Decode(b.data(), b.size(), ao));
    a.by = 1; a.victim = 12; b = Encode(a); CHECK(!Decode(b.data(), b.size(), ao));
}

static void TestTable() {
    LeaseTable t;
    CHECK(!t.Active(2) && t.Holder(2) == kNoSlot && t.Epoch(2) == 0 && !t.Any());
    // Slot 1's fighter (entity 2) grants itself to slot 0 (the host's fighter grabbed it).
    LeaseEntry g = t.Write(2, kHostSlot, 1, kCoupleGrab);
    CHECK(g.entity == 2 && g.lease.epoch == 1 && t.Active(2) && t.Holder(2) == kHostSlot && t.Of(2).by == 1 && t.Any());
    CHECK(t.HoldsAny(kHostSlot) && !t.HoldsAny(1) && t.IsBy(1) && !t.IsBy(2));
    CHECK(t.holders().Of(2) == kHostSlot && t.holders().Of(3) == kNoSlot && OwnerSlotOf(2, t.holders()) == kHostSlot);
    // Another machine hears it; a straggler of the same epoch or older changes nothing.
    LeaseTable o;
    Lease was;
    CHECK(o.Apply(g, &was) && !was.Active() && o.Holder(2) == kHostSlot);
    CHECK(!o.Apply(g) && !o.Apply(Entry(2, 0, kNoSlot)));
    // Given back the moment they part.
    LeaseEntry back = t.Write(2, kNoSlot);
    CHECK(back.lease.epoch == 2 && !back.lease.Active() && back.lease.by == 0 && !t.Active(2));
    CHECK(o.Apply(back, &was) && was.holder == kHostSlot && !o.Active(2));
    // Wrap at 256.
    LeaseTable w;
    for (int i = 0; i < 300; ++i) w.Write(4, i % 2 ? kNoSlot : uint8_t(1), 1, kCoupleStuck);
    CHECK(w.Epoch(4) == uint8_t(300 % 256));
    LeaseTable w2; w2.Set(Entry(4, 250, kNoSlot));
    CHECK(w2.Apply(Entry(4, 5, 1, 1, kCoupleGrab)) && w2.Holder(4) == 1 && !w2.Apply(Entry(4, 250, kNoSlot)));
    // A joiner takes the host's table as it is.
    HandOffMsg snap = t.Snapshot();
    CHECK(snap.full && snap.entries.size() == 1 && snap.entries[0].entity == 2 && snap.entries[0].lease.epoch == 2);
    LeaseTable j;
    CHECK(j.Set(snap.entries[0]) && j.Epoch(2) == 2 && !j.Set(snap.entries[0]));
    t.Clear(); CHECK(t.Epoch(2) == 0 && !t.Any());
}

static void TestWriteOk() {
    Lease free; free.epoch = 4;
    Lease held; held.epoch = 5; held.holder = 3; held.by = 4;
    CHECK(LeaseWriteOk(1, Entry(2, 5, 3, 4, kCoupleGrab), free));        // the owner grants
    CHECK(!LeaseWriteOk(3, Entry(2, 5, 3, 4, kCoupleGrab), free));       // the asker cannot take it itself
    CHECK(!LeaseWriteOk(1, Entry(2, 6, 3, 4, kCoupleGrab), free));       // skips an epoch
    CHECK(!LeaseWriteOk(1, Entry(2, 4, 3, 4, kCoupleGrab), free));       // not newer
    CHECK(!LeaseWriteOk(1, Entry(2, 6, 0, 1, kCoupleGrab), held));       // already held: no grant over it
    CHECK(LeaseWriteOk(3, Entry(2, 6, kNoSlot), held));                  // the holder gives it back
    CHECK(LeaseWriteOk(1, Entry(2, 6, kNoSlot), held));                  // the owner takes it back
    CHECK(!LeaseWriteOk(2, Entry(2, 6, kNoSlot), held));                 // a third machine cannot
    CHECK(!LeaseWriteOk(3, Entry(2, 5, kNoSlot), free));                 // not the holder of a free one
}

static void TestJudge() {
    LeaseTable t;
    HandOffAskMsg a; a.victim = 3; a.by = 2; a.why = kCoupleGrab;        // slot 1's fighter grabbed slot 2's
    CHECK(JudgeAsk(t, a, 1, 2, false) == AskVerdict::Grant);
    CHECK(JudgeAsk(t, a, 1, 4, false) == AskVerdict::Wrong);             // not my fighter
    CHECK(JudgeAsk(t, a, 3, 2, false) == AskVerdict::Wrong);             // the asker does not own `by`
    CHECK(JudgeAsk(t, a, 1, 2, true) == AskVerdict::Grant);              // both asked: slot 1 is lower, it takes the pair
    HandOffAskMsg back; back.victim = 2; back.by = 3; back.why = kCoupleGrab;
    CHECK(JudgeAsk(t, back, 2, 1, true) == AskVerdict::Mine);            // ... and slot 1 refuses slot 2's ask
    HandOffAskMsg bot; bot.victim = 3; bot.by = 12; bot.why = kCoupleStuck;
    CHECK(JudgeAsk(t, bot, kHostSlot, 2, false) == AskVerdict::Grant);   // a bot's blade: the host takes it
    t.Write(3, kHostSlot, 12, kCoupleStuck);
    CHECK(JudgeAsk(t, a, 1, 2, false) == AskVerdict::Leased);
    LeaseTable c; c.Write(4, 1, 2, kCoupleGrab);                         // slot 1 already holds entity 4: it may take a second
    CHECK(JudgeAsk(c, a, 1, 2, false) == AskVerdict::Grant);
    LeaseTable c2; c2.Write(4, 2, 3, kCoupleGrab);                       // I hold entity 4: my own fighter is not handed on
    CHECK(JudgeAsk(c2, a, 1, 2, false) == AskVerdict::Chain);
    LeaseTable d; d.Write(2, 2, 3, kCoupleGrab);                         // the asker's fighter is held by the victim's machine
    CHECK(JudgeAsk(d, a, 1, 2, false) == AskVerdict::Chain);
    LeaseTable e; e.Write(5, 3, 3, kCoupleGrab);                         // the victim is coupled to a lease already
    CHECK(JudgeAsk(e, a, 1, 2, false) == AskVerdict::Chain);
}

static void TestGeometry() {
    std::vector<HandOffPoint> body{{0, 0, 100}, {0, 0, 150}};
    CHECK(HandOn({0, kGrabReachUu - 1, 150}, body, kGrabReachUu));
    CHECK(!HandOn({0, kGrabReachUu + 1, 150}, body, kGrabReachUu) && HandOn({0, kGrabReachUu + 1, 150}, body, kGrabKeepUu));
    CHECK(!HandOn({0, 0, 0}, {}, kGrabKeepUu));
    CHECK(std::string(CoupleName(kCoupleGrab | kCoupleStuck)) == "grab+stuck");
}

int main() {
    TestWire();
    TestTable();
    TestWriteOk();
    TestJudge();
    TestGeometry();
    if (g_fail == 0) std::printf("test_hsm_handoff: all passed\n");
    return g_fail == 0 ? 0 : 1;
}

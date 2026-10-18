// Unit tests for hsm_handmatch.h. Run via implementation/shared/run_tests.sh test_hsm_handmatch.
#include "sync/hsm_handmatch.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;

namespace {

constexpr uint32_t kSword = 0x5304D001u, kShield = 0x5419E1D2u;
constexpr auto kNone = HandAct::None, kAttach = HandAct::Attach, kRelease = HandAct::Release;

// The owner's pose shows cls in this hand since sinceMs; the owner's Equip token for it is other; the stand-in's hand
// is empty.
HandFacts Shows(uint32_t cls, double sinceMs) {
    HandFacts f;
    f.havePose = true;
    f.poseClass = cls;
    f.poseSinceMs = sinceMs;
    f.tokenKnown = true;
    f.token = 0;
    return f;
}

HandFacts Holds(HandFacts f, uint32_t cls, bool prop) {
    f.holding = true;
    f.heldClass = cls;
    f.heldIsProp = prop;
    return f;
}

void TestAttach() {
    HandMatch m;
    // Nothing in either hand, or no pose: nothing to do.
    CHECK(m.Step(Shows(kHandEmpty, 0), 5000) == kNone);
    HandFacts f = Shows(kSword, 1000);
    f.havePose = false;
    CHECK(m.Step(f, 5000) == kNone);
    CHECK(m.Step(Shows(kHandUnknown, 1000), 5000) == kNone);   // a weapon of unknown class: never matched
    // A pickup: the pose shows a sword in the empty hand; after the grace the stand-in takes one.
    CHECK(m.Step(Shows(kSword, 1000), 1000) == kNone);
    CHECK(m.Step(Shows(kSword, 1000), 1000 + kAttachGraceMs - 1) == kNone);
    CHECK(m.Step(Shows(kSword, 1000), 1000 + kAttachGraceMs) == kAttach);
    CHECK(m.Step(Shows(kSword, 1000), 1000 + kAttachGraceMs + 5000) == kAttach);   // until it does
    // The owner's Equip state names a slot for this hand: a draw, Equip's business.
    f = Shows(kSword, 1000);
    f.token = 2;
    CHECK(m.Step(f, 9000) == kNone);
    // No Equip state from the owner yet: not known whether it is a draw.
    f = Shows(kSword, 1000);
    f.tokenKnown = false;
    CHECK(m.Step(f, 9000) == kNone);
    // The stand-in has a sword sheathed where this hand reaches and the owner still shows that slot sheathed: maybe a
    // draw whose Equip state is on its way, so it waits longer.
    f = Shows(kSword, 1000);
    f.mayBeDraw = true;
    CHECK(m.Step(f, 1000 + kAttachGraceMs) == kNone);
    CHECK(m.Step(f, 1000 + kDrawGraceMs - 1) == kNone);
    CHECK(m.Step(f, 1000 + kDrawGraceMs) == kAttach);
    // A hand holding something never attaches.
    CHECK(m.Step(Holds(Shows(kSword, 1000), kSword, false), 9000) == kNone);
}

void TestRelease() {
    HandMatch m;
    // The stand-in holds what the pose shows: kept.
    CHECK(m.Step(Holds(Shows(kSword, 0), kSword, true), 1000) == kNone);
    CHECK(m.Step(Holds(Shows(kSword, 0), kSword, false), 90000) == kNone);
    // A prop the owner no longer holds: let go after kReleasePropMs of the pose showing the hand empty.
    CHECK(m.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 2000) == kNone);
    CHECK(m.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 2000 + kReleasePropMs - 1) == kNone);
    CHECK(m.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 2000 + kReleasePropMs) == kRelease);
    // ... or showing another class.
    HandMatch n;
    CHECK(n.Step(Holds(Shows(kShield, 0), kSword, true), 2000) == kNone);
    CHECK(n.Step(Holds(Shows(kShield, 0), kSword, true), 2000 + kReleasePropMs) == kRelease);
    // Any other weapon waits kReleaseOtherMs (a drop's Grant may still be on its way).
    HandMatch o;
    CHECK(o.Step(Holds(Shows(kHandEmpty, 0), kSword, false), 2000) == kNone);
    CHECK(o.Step(Holds(Shows(kHandEmpty, 0), kSword, false), 2000 + kReleasePropMs) == kNone);
    CHECK(o.Step(Holds(Shows(kHandEmpty, 0), kSword, false), 2000 + kReleaseOtherMs - 1) == kNone);
    CHECK(o.Step(Holds(Shows(kHandEmpty, 0), kSword, false), 2000 + kReleaseOtherMs) == kRelease);
    // The mismatch must last: the pose showing the sword again starts it over.
    HandMatch p;
    CHECK(p.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000) == kNone);
    CHECK(p.Step(Holds(Shows(kSword, 0), kSword, true), 1200) == kNone);
    CHECK(p.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1300) == kNone);
    CHECK(p.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1300 + kReleasePropMs - 1) == kNone);
    CHECK(p.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1300 + kReleasePropMs) == kRelease);
    // ... and so does a pose that went missing, or one of unknown class.
    HandMatch q;
    CHECK(q.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000) == kNone);
    HandFacts gone = Holds(Shows(kHandEmpty, 0), kSword, true);
    gone.havePose = false;
    CHECK(q.Step(gone, 1200) == kNone);
    CHECK(q.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1300) == kNone);
    CHECK(q.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1300 + kReleasePropMs - 1) == kNone);
    CHECK(q.Step(Holds(Shows(kHandUnknown, 0), kSword, true), 1560) == kNone);
    CHECK(q.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1600) == kNone);
    CHECK(q.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1600 + kReleasePropMs - 1) == kNone);
    CHECK(q.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1600 + kReleasePropMs) == kRelease);
    // A drop being resolved by prop_sync is left to it; the clock keeps running, so a drop it gives up on is let go of
    // as soon as it does.
    HandMatch r;
    HandFacts pending = Holds(Shows(kHandEmpty, 0), kSword, false);
    pending.dropPending = true;
    CHECK(r.Step(pending, 1000) == kNone);
    CHECK(r.Step(pending, 1000 + kReleaseOtherMs + 500) == kNone);
    CHECK(r.Step(Holds(Shows(kHandEmpty, 0), kSword, false), 1000 + kReleaseOtherMs + 600) == kRelease);
    // A held weapon of unknown class is never let go of.
    HandMatch s;
    CHECK(s.Step(Holds(Shows(kHandEmpty, 0), 0, true), 1000) == kNone);
    CHECK(s.Step(Holds(Shows(kHandEmpty, 0), 0, true), 90000) == kNone);
    // ... and does not start the clock: a held class learned later starts it then.
    HandMatch s2;
    CHECK(s2.Step(Holds(Shows(kShield, 0), 0, true), 1000) == kNone);
    CHECK(s2.Step(Holds(Shows(kShield, 0), kSword, true), 1000 + kReleasePropMs) == kNone);
    CHECK(s2.Step(Holds(Shows(kShield, 0), kSword, true), 1000 + 2 * kReleasePropMs) == kRelease);
    // Reset forgets the running mismatch.
    HandMatch t;
    CHECK(t.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000) == kNone);
    t.Reset();
    CHECK(t.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000 + kReleasePropMs) == kNone);
    CHECK(t.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000 + 2 * kReleasePropMs) == kRelease);
    // An empty hand in between (released elsewhere) starts it over too.
    HandMatch u;
    CHECK(u.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000) == kNone);
    CHECK(u.Step(Shows(kHandEmpty, 0), 1100) == kNone);
    CHECK(u.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1200) == kNone);
    CHECK(u.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000 + kReleasePropMs) == kNone);
    CHECK(u.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1200 + kReleasePropMs) == kRelease);
    // An empty hand is empty whatever class it last held.
    HandMatch v;
    CHECK(v.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000) == kNone);
    HandFacts emptied = Shows(kHandEmpty, 0);
    emptied.heldClass = kSword;
    CHECK(v.Step(emptied, 1100) == kNone);
    CHECK(v.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1200) == kNone);
    CHECK(v.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1000 + kReleasePropMs) == kNone);
    CHECK(v.Step(Holds(Shows(kHandEmpty, 0), kSword, true), 1200 + kReleasePropMs) == kRelease);
}

void TestPick() {
    const V3 now{1000, 0, 100}, picked{0, 0, 20};
    // The nearest sword near where the owner holds it now.
    std::vector<LooseWeapon> w = {{{1100, 0, 100}, kSword}, {{1050, 0, 100}, kSword}, {{1010, 0, 100}, kShield}};
    CHECK(PickAttach(w, kSword, now, picked) == 1);
    CHECK(PickAttach(w, kShield, now, picked) == 2);
    // ... or near where it picked it up (a weapon still lying here where it lay there).
    w = {{{3000, 0, 0}, kSword}, {{40, 0, 0}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == 1);
    // The nearer of the two distances counts.
    w = {{{1100, 0, 100}, kSword}, {{50, 0, 20}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == 1);
    // Within kAttachRadiusUu, inclusive.
    w = {{{1000 + kAttachRadiusUu, 0, 100}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == 0);
    w = {{{1000 + kAttachRadiusUu + 0.01, 0, 100}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == -1);
    w = {{{0, kAttachRadiusUu + 0.01, 20}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == -1);
    w = {{{0, 0, 20 - kAttachRadiusUu}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == 0);
    w = {{{0, 0, 20 - kAttachRadiusUu - 0.01}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == -1);
    // Other classes, an empty list, and the empty or unknown class never match.
    w = {{{1000, 0, 100}, kShield}};
    CHECK(PickAttach(w, kSword, now, picked) == -1);
    CHECK(PickAttach({}, kSword, now, picked) == -1);
    w = {{{1000, 0, 100}, kHandEmpty}, {{1000, 0, 100}, kHandUnknown}};
    CHECK(PickAttach(w, kHandEmpty, now, picked) == -1);
    CHECK(PickAttach(w, kHandUnknown, now, picked) == -1);
    // A tie goes to the first.
    w = {{{1000, 10, 100}, kSword}, {{1000, -10, 100}, kSword}};
    CHECK(PickAttach(w, kSword, now, picked) == 0);
}

}  // namespace

int main() {
    TestAttach();
    TestRelease();
    TestPick();
    if (g_fail) { std::printf("test_hsm_handmatch: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_handmatch: ok\n");
    return 0;
}

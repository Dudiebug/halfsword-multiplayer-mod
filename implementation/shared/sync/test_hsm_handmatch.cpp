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
    // The stand-in holds what the pose shows: kept.
    HandMatch k;
    CHECK(k.Step(Holds(Shows(kSword, 0), kSword, true), 1000) == kNone);
    CHECK(k.Step(Holds(Shows(kSword, 0), kSword, false), 90000) == kNone);
    // A prop the owner no longer holds (and the pose never showed in this hand here, TestFlickerHold): let go after
    // kReleasePropMs of the pose showing the hand empty.
    HandMatch m;
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
    // The mismatch must last: the pose showing the sword again starts it over (another class: an empty pose right after
    // the sword is held longer, TestFlickerHold).
    HandMatch p;
    CHECK(p.Step(Holds(Shows(kShield, 0), kSword, true), 1000) == kNone);
    CHECK(p.Step(Holds(Shows(kSword, 0), kSword, true), 1200) == kNone);
    CHECK(p.Step(Holds(Shows(kShield, 0), kSword, true), 1300) == kNone);
    CHECK(p.Step(Holds(Shows(kShield, 0), kSword, true), 1300 + kReleasePropMs - 1) == kNone);
    CHECK(p.Step(Holds(Shows(kShield, 0), kSword, true), 1300 + kReleasePropMs) == kRelease);
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

// Test 5 (10:22:26-29): the owner's pose showed a hand empty for half a second or a second now and then while it held a
// loose weapon, and the stand-in took that prop and let it go four times in three seconds. The last class the pose
// showed in a hand is held for kReattachMinMs, counted from the last step that showed it: an empty pose lets the weapon
// of that class go only after it.
void TestFlickerHold() {
    HandMatch m;
    const auto sword = [](double since) { return Holds(Shows(kSword, since), kSword, true); };
    const auto empty = [](double since) { return Holds(Shows(kHandEmpty, since), kSword, true); };
    // The pickup: the pose shows a sword in the empty hand, the stand-in takes one (prop_sync's SetHeld resets the
    // matcher, which does not forget what the pose showed) and the pose shows the hand empty right after.
    CHECK(m.Step(Shows(kSword, 1000), 1000 + kAttachGraceMs) == kAttach);
    m.Reset();
    CHECK(m.Step(empty(1260), 1300) == kNone);
    CHECK(m.Step(empty(1260), 1300 + kReleasePropMs) == kNone);
    CHECK(m.Step(sword(1600), 1600) == kNone);
    CHECK(m.Step(sword(1600), 5000) == kNone);
    // The flicker: empty for 600 ms, the sword, empty for nearly kReattachMinMs, the sword: never let go of.
    CHECK(m.Step(empty(5001), 5010) == kNone);
    CHECK(m.Step(empty(5001), 5600) == kNone);
    CHECK(m.Step(sword(5601), 5610) == kNone);
    CHECK(m.Step(empty(5611), 5620) == kNone);
    CHECK(m.Step(empty(5611), 5620 + kReleasePropMs) == kNone);
    CHECK(m.Step(empty(5611), 5610 + kReattachMinMs - 100) == kNone);
    CHECK(m.Step(sword(7600), 7600) == kNone);
    // An empty pose that lasts: let go of kReattachMinMs after the last step that showed the sword (not after the
    // pose's own change at 7600, nor after the first sword at 1000).
    CHECK(m.Step(empty(7650), 7700) == kNone);
    CHECK(m.Step(empty(7650), 7600 + kReattachMinMs - 1) == kNone);
    CHECK(m.Step(empty(7650), 7600 + kReattachMinMs) == kRelease);
    // Counted from the step, not the pose's change: the sword shown since 1000 and last at 9000.
    HandMatch n;
    CHECK(n.Step(sword(1000), 9000) == kNone);
    CHECK(n.Step(empty(9001), 9001 + kReleasePropMs) == kNone);
    CHECK(n.Step(empty(9001), 9000 + kReattachMinMs - 1) == kNone);
    CHECK(n.Step(empty(9001), 9000 + kReattachMinMs) == kRelease);
    // Only the class the pose showed is held: a shield shown last lets a sword go as any other class does, and a
    // pose of unknown class (or none) is no class shown.
    HandMatch o;
    CHECK(o.Step(Holds(Shows(kShield, 1000), kSword, true), 1000) == kNone);
    CHECK(o.Step(empty(1100), 1100) == kNone);
    CHECK(o.Step(empty(1100), 1000 + kReleasePropMs) == kRelease);
    HandMatch q;
    CHECK(q.Step(Holds(Shows(kHandUnknown, 1000), kSword, true), 1000) == kNone);
    HandFacts gone = sword(1000);
    gone.havePose = false;
    CHECK(q.Step(gone, 1050) == kNone);
    CHECK(q.Step(empty(1100), 1100) == kNone);
    CHECK(q.Step(empty(1100), 1100 + kReleasePropMs) == kRelease);
    // ... nor does it end the hold of the sword shown before it.
    HandMatch t;
    CHECK(t.Step(sword(1000), 1000) == kNone);
    CHECK(t.Step(Holds(Shows(kHandUnknown, 1010), kSword, true), 1010) == kNone);
    CHECK(t.Step(empty(1020), 1020) == kNone);
    CHECK(t.Step(empty(1020), 1020 + kReleasePropMs) == kNone);
    CHECK(t.Step(empty(1020), 1000 + kReattachMinMs) == kRelease);
    // Another class shown after the sword is no flicker: let go as soon as any other mismatch.
    HandMatch r;
    CHECK(r.Step(sword(1000), 1000) == kNone);
    CHECK(r.Step(Holds(Shows(kShield, 1010), kSword, true), 1010) == kNone);
    CHECK(r.Step(Holds(Shows(kShield, 1010), kSword, true), 1010 + kReleasePropMs) == kRelease);
    // Any other weapon: still kReleaseOtherMs after the mismatch began (longer than the hold).
    HandMatch s;
    CHECK(kReleaseOtherMs > kReattachMinMs);
    CHECK(s.Step(Holds(Shows(kSword, 1000), kSword, false), 1000) == kNone);
    CHECK(s.Step(Holds(Shows(kHandEmpty, 1010), kSword, false), 1010) == kNone);
    CHECK(s.Step(Holds(Shows(kHandEmpty, 1010), kSword, false), 1010 + kReleaseOtherMs - 1) == kNone);
    CHECK(s.Step(Holds(Shows(kHandEmpty, 1010), kSword, false), 1010 + kReleaseOtherMs) == kRelease);
    // The hold is the matcher's, and as long as prop_sync's reattach limit for a weapon that left a hand by itself.
    CHECK(kReattachMinMs == 2000.0 && kReattachMinMs > kReleasePropMs);
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

// Test 5: a drop heard while the stand-in's hand was empty fired its deadline 18 s later at the secondary Equip had
// drawn into that hand since, and let go of it ("flung out of his hand"). A drop resolves only against the weapon its
// hand held when the drop was heard (the witness).
void TestDropWitness() {
    constexpr auto kWait = DropAct::Wait, kLetGo = DropAct::Release, kGiveUp = DropAct::GiveUp;
    // The usual drop: the copy is in hand when the drop is heard and let go of once the pose shows the hand let go ...
    DropFacts f;
    f.witnessed = true;
    f.holdsWitness = true;
    CHECK(StepDrop(f) == kWait);
    f.poseShowsLetGo = true;
    CHECK(StepDrop(f) == kLetGo);
    // ... or at the deadline, whatever the pose shows (no pose, or one that never caught up).
    f.poseShowsLetGo = false;
    f.deadline = true;
    CHECK(StepDrop(f) == kLetGo);
    // A hand that has drawn slot 3 since (another weapon in it now) is not a drop candidate: it waits, then the drop is
    // given up, and that weapon is never let go of for it.
    DropFacts drew;
    drew.witnessed = true;
    drew.holdsWitness = false;
    drew.poseShowsLetGo = true;
    CHECK(StepDrop(drew) == kWait);
    drew.deadline = true;
    CHECK(StepDrop(drew) == kGiveUp);
    // A drop whose weapon was not on the stand-in when it was heard stays unresolved and is never forced, whatever the
    // hand holds later (holdsWitness has nothing to name then).
    DropFacts none;
    none.witnessed = false;
    for (int pose = 0; pose < 2; ++pose)
        for (int holds = 0; holds < 2; ++holds) {
            none.poseShowsLetGo = pose != 0;
            none.holdsWitness = holds != 0;
            none.deadline = false;
            CHECK(StepDrop(none) == kWait);
            none.deadline = true;
            CHECK(StepDrop(none) == kGiveUp);
        }
}

// A weapon the stand-in let go of from the drop's hand (loose here) is the drop's only when it is the witness, or, with
// that hand empty when the drop was heard, when it was let go of before (the matcher or the weapon itself was quicker).
void TestAdoptCandidate() {
    CHECK(AdoptCandidate(true, true, 5000, 1000));      // the witness, let go of after the drop was heard
    CHECK(AdoptCandidate(true, true, 500, 1000));
    CHECK(!AdoptCandidate(true, false, 500, 1000));     // another weapon, even one let go of before
    CHECK(!AdoptCandidate(true, false, 5000, 1000));
    CHECK(AdoptCandidate(false, false, 500, 1000));     // nothing in hand when heard: one let go of before it
    CHECK(AdoptCandidate(false, false, 1000, 1000));
    CHECK(!AdoptCandidate(false, false, 1000.5, 1000)); // ... never one let go of since (test 5's drawn secondary)
    CHECK(!AdoptCandidate(false, true, 5000, 1000));
}

}  // namespace

// Test 7: the owner picked up a weapon whose copy here had drifted past kAttachRadiusUu, and the stand-in's hand stayed
// empty. PickAttachFar is the fallback once PickAttach found none: the nearest of the class within kAttachFarRadiusUu.
void TestPickFar() {
    const V3 now{1000, 0, 100}, picked{0, 0, 20};
    CHECK(kAttachFarRadiusUu == 600.0 && kAttachFarRadiusUu > kAttachRadiusUu);
    std::vector<LooseWeapon> w = {{{1400, 0, 100}, kSword}, {{1300, 0, 100}, kSword}, {{1010, 0, 100}, kShield}};
    CHECK(PickAttach(w, kSword, now, picked) == -1);
    CHECK(PickAttachFar(w, kSword, now, picked) == 1);
    w = {{{1000 + kAttachFarRadiusUu, 0, 100}, kSword}};
    CHECK(PickAttachFar(w, kSword, now, picked) == 0);                       // inclusive
    w = {{{1000 + kAttachFarRadiusUu + 0.01, 0, 100}, kSword}};
    CHECK(PickAttachFar(w, kSword, now, picked) == -1);
    w = {{{0, 0, 20 - kAttachFarRadiusUu}, kSword}};
    CHECK(PickAttachFar(w, kSword, now, picked) == 0);                       // or near where it was picked up
    CHECK(PickAttachFar(w, kShield, now, picked) == -1);
    CHECK(PickAttachFar(w, kHandEmpty, now, picked) == -1 && PickAttachFar(w, kHandUnknown, now, picked) == -1);
}

int main() {
    TestPickFar();
    TestAttach();
    TestRelease();
    TestFlickerHold();
    TestPick();
    TestDropWitness();
    TestAdoptCandidate();
    if (g_fail) { std::printf("test_hsm_handmatch: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_handmatch: ok\n");
    return 0;
}

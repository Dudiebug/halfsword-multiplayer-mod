// Ground clear: a thrown weapon whose "Last Parent" the game already emptied is cleared (the #249 / #252 bug); held, sheathed,
// body and untouched level weapons stay.
#include "session/hsm_ground_clear.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestDrops() {
    DropCandidate c;                                         // no parent, nothing else: lying on the ground
    CHECK(ClearableDrop(c) == DropVerdict::Clear);           // the bug: the old rule also needed a Last Parent
    c = DropCandidate{}; c.levelStart = false; CHECK(ClearableDrop(c) == DropVerdict::Clear);
}

static void TestKept() {
    DropCandidate c; c.hasParent = true; c.inHand = true;
    CHECK(ClearableDrop(c) == DropVerdict::KeepHeld);
    c = DropCandidate{}; c.hasParent = true;
    CHECK(ClearableDrop(c) == DropVerdict::KeepSheathed);
    c = DropCandidate{}; c.bodyWeapon = true; CHECK(ClearableDrop(c) == DropVerdict::KeepOther);
    c = DropCandidate{}; c.bodyWeapon = true; c.hasParent = true; c.inHand = true; CHECK(ClearableDrop(c) == DropVerdict::KeepOther);
    c = DropCandidate{}; c.beingDestroyed = true; CHECK(ClearableDrop(c) == DropVerdict::KeepOther);
    c = DropCandidate{}; c.levelStart = true; CHECK(ClearableDrop(c) == DropVerdict::KeepLevel);
}

static void TestParentBeatsLevel() {
    DropCandidate c; c.levelStart = true; c.hasParent = true;           // a level weapon someone holds: held, never counted as level
    CHECK(ClearableDrop(c) == DropVerdict::KeepSheathed);
    c.inHand = true; CHECK(ClearableDrop(c) == DropVerdict::KeepHeld);
}

static void TestLevelRest() {
    CHECK(AtLevelStart(0.0));                                            // where it lay
    CHECK(AtLevelStart(100.0 * 100.0));                                  // settled by a metre
    CHECK(AtLevelStart(kLevelWeaponRestUu * kLevelWeaponRestUu));        // the edge
    CHECK(!AtLevelStart(300.0 * 300.0));                                 // kicked or thrown: picked up and put down elsewhere
    CHECK(!AtLevelStart(-1.0));                                          // no position read
    DropCandidate moved; moved.levelStart = AtLevelStart(500.0 * 500.0);
    CHECK(ClearableDrop(moved) == DropVerdict::Clear);                   // a moved level weapon is a drop
}

int main() {
    TestDrops(); TestKept(); TestParentBeatsLevel(); TestLevelRest();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}

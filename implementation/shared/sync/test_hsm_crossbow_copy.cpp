// Unit tests for hsm_crossbow_copy.h. Run via implementation/shared/run_tests.sh test_hsm_crossbow_copy.
#include "sync/hsm_crossbow_copy.h"
#include "sync/hsm_handmatch.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::sync;

namespace {

constexpr uint32_t kSword = 0x5304D001u;

// The class the logs name: "stand-in of e2 hand 0: no loose weapon of the owner's class 4d7ba529".
void TestClass() {
    CHECK(kCrossbowClass == 0x4d7ba529u);
}

// Report 7etrzM8kfJ (host, Teams 5 players, crossbows on): e2 was given the crossbow at 21:52:27.057 and its stand-in got a
// copy at 21:52:29.920, after the first crossbow replay (21:52:28.705, "its stand-in holds no crossbow"). The spawn gift
// is due the moment the pose shows the class, however young that pose is.
void TestGiftIsImmediate() {
    GiftBook g;
    g.Note(2, kGiftFlag, 1000.0);
    CHECK(g.Due(2, kCrossbowClass, 1000.0));
    CHECK(g.Due(2, kCrossbowClass, 1300.0));                // 300 ms of pose: sync::kAttachGraceMs, far below the 1500 ms wait
    CHECK(!g.Due(2, kSword, 1300.0));                        // another weapon in the hand is the ordinary match's
    CHECK(!g.Due(2, kHandEmpty, 1300.0) && !g.Due(2, kHandUnknown, 1300.0));
    CHECK(!g.Due(3, kCrossbowClass, 1300.0));                // another fighter, no gift
    CHECK(g.AgeMs(2, 1300.0) == 300.0);
}

// Report 7etrzM8kfJ 21:58:57: e3 respawned with a fresh crossbow, its stand-in took the last life's copy lying 600 uu away
// and let it go 250 ms later; the hand then took nothing for the rest of the pose. The gift is due whatever lies about.
void TestGiftWithStaleCopyAround() {
    GiftBook g;
    g.Note(3, kGiftFlag, 5000.0);
    const std::vector<LooseWeapon> stale = {{{0, 0, 580}, kCrossbowClass}};
    CHECK(PickAttachFar(stale, kCrossbowClass, {0, 0, 0}, {0, 0, 0}) == 0);   // the old rule would have taken it
    CHECK(g.Due(3, kCrossbowClass, 5400.0));                                  // the new one builds its own, ahead of the pick
}

void TestGiftEnds() {
    GiftBook g;
    g.Note(2, kGiftFlag, 0.0);
    CHECK(g.Due(2, kCrossbowClass, kGiftWindowMs));
    CHECK(!g.Due(2, kCrossbowClass, kGiftWindowMs + 1.0));   // a crossbow in the hand this late was picked up: the ordinary match
    g.Note(2, kGiftFlag, 100.0);
    g.Done(2);                                               // its copy exists
    CHECK(!g.Due(2, kCrossbowClass, 200.0));
    g.Note(2, kGiftFlag, 300.0);                             // a respawn with the gift again
    CHECK(g.Due(2, kCrossbowClass, 400.0));
    g.Note(2, 0, 500.0);                                     // a respawn without it (the round's draw went elsewhere)
    CHECK(!g.Due(2, kCrossbowClass, 600.0));
    g.Note(2, 2, 700.0);                                     // kSpawnZombie only
    CHECK(!g.Due(2, kCrossbowClass, 800.0));
    g.Note(2, 3, 900.0);                                     // zombie and crossbow bits: the gift bit counts
    CHECK(g.Due(2, kCrossbowClass, 1000.0));
    g.Forget(2);
    CHECK(!g.Due(2, kCrossbowClass, 1000.0));
    g.Note(200, kGiftFlag, 0.0);                             // out of range: ignored
    CHECK(!g.Due(200, kCrossbowClass, 1.0));
    CHECK(!g.Due(2, kCrossbowClass, -5.0));                  // a clock before the note
}

// Report aWKV3a4YCh prev2 (client): the owner's crossbow moved to the left hand to pull the string. The stand-in of e1 had
// a copy in hand 0 (21:11:50.327), built another for hand 1 (21:11:56.233), let the first go 1 s later (re-armed as a
// dropped weapon, 11 bodies), and again at 21:12:31 / 21:12:34, 21:12:47 / 21:12:57. The copy is moved instead.
void TestMoveCopyToTheOtherHand() {
    CHECK(MoveHeldCopy(kCrossbowClass, true, kCrossbowClass, false, kHandEmpty));    // pose: hand 0 empty, hand 1 crossbow
    CHECK(!MoveHeldCopy(kCrossbowClass, true, kCrossbowClass, false, kCrossbowClass)); // the owner holds two crossbows: a second copy
    CHECK(!MoveHeldCopy(kCrossbowClass, false, 0, false, kHandEmpty));                // the other hand holds nothing
    CHECK(!MoveHeldCopy(kCrossbowClass, true, kSword, false, kHandEmpty));            // it holds another class
    CHECK(!MoveHeldCopy(kCrossbowClass, true, kCrossbowClass, true, kHandEmpty));     // a synced prop: its stream carries it
    CHECK(MoveHeldCopy(kCrossbowClass, true, kCrossbowClass, false, kSword));         // the owner swapped it for a sword there
    CHECK(!MoveHeldCopy(kHandEmpty, true, kHandEmpty, false, kSword));
    CHECK(!MoveHeldCopy(kHandUnknown, true, kHandUnknown, false, kHandEmpty));
}

}  // namespace

int main() {
    TestClass();
    TestGiftIsImmediate();
    TestGiftWithStaleCopyAround();
    TestGiftEnds();
    TestMoveCopyToTheOtherHand();
    if (g_fail) { std::printf("test_hsm_crossbow_copy: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crossbow_copy: ok\n");
    return 0;
}

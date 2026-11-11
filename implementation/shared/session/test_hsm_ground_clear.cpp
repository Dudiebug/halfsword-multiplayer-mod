// Ground clear: a thrown weapon whose "Last Parent" the game already emptied is cleared (the #249 / #252 bug); held, sheathed,
// body and untouched level weapons stay.
#include "session/hsm_ground_clear.h"
#include "session/hsm_structure.h"
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

static void TestClearMask() {
    CHECK(kClearAll == 7 && ParseClearMask("") == 7 && ParseClearMask("ALL") == 7 && ParseClearMask("Rocks") == kClearRocks);
    CHECK(ParseClearMask("armor") == 2 && ParseClearMask("armour") == 2 && ParseClearMask("weapons") == 1 && ParseClearMask("nope") == 0);
    CHECK(ClearMaskText(kClearAll) == "weapons, armour, rocks" && ClearMaskText(kClearRocks) == "rocks" && ClearMaskText(3) == "weapons, armour");
}

static void TestClearWire() {
    GroundClearMsg m; m.seq = 200; m.mask = kClearWeapons | kClearRocks;
    const auto b = Encode(m);
    GroundClearMsg back;
    CHECK(b.size() == 6 && Decode(b.data(), b.size(), back) && back.seq == 200 && back.mask == 5);
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::GroundClear && uint8_t(t) == 59 && kLastDecodedMsgType >= 59);
    for (uint8_t mask : {uint8_t(0), uint8_t(8), uint8_t(0x80), uint8_t(0xFF)}) {
        GroundClearMsg bad; bad.seq = 1; bad.mask = mask;
        const auto e = Encode(bad); CHECK(!Decode(e.data(), e.size(), back));            // mask 0 or a bit outside 7
    }
    auto ok = Encode(m);
    CHECK(!Decode(ok.data(), ok.size() - 1, back) && !Decode(ok.data(), 0, back));      // short
    auto longer = ok; longer.push_back(0); CHECK(!Decode(longer.data(), longer.size(), back));   // trailing byte
    auto other = ok; other[3] = uint8_t(MsgType::MapReset); CHECK(!Decode(other.data(), other.size(), back));   // another type
}

static void TestClearableProp() {
    PropCandidate c;
    CHECK(!ClearableProp(c));                                                    // nothing known: kept
    PropCandidate moved; moved.level = true; moved.haveStart = true;
    CHECK(ClearableProp(moved));                                                 // a moved level prop (a rock) goes
    PropCandidate still = moved; still.unmoved = true; CHECK(!ClearableProp(still));         // scenery stays
    PropCandidate noStart = moved; noStart.haveStart = false; CHECK(!ClearableProp(noStart));
    PropCandidate spawned; spawned.spawned = true; spawned.bornInMatch = true; CHECK(ClearableProp(spawned));
    PropCandidate early = spawned; early.bornInMatch = false; CHECK(!ClearableProp(early)); // the level's own start-up object
    PropCandidate weapon; weapon.haveStart = true; CHECK(!ClearableProp(weapon));            // neither a level prop nor spawned
    for (PropCandidate base : {moved, spawned}) {
        PropCandidate h = base; h.held = true; CHECK(!ClearableProp(h));
        h = base; h.denied = true; CHECK(!ClearableProp(h));
        h = base; h.sub = true; CHECK(!ClearableProp(h));
        h = base; h.structure = true; CHECK(!ClearableProp(h));
    }
    // the structure shape (a bridge: one actor, bodies joined by constraints) is a level prop and is kept in every sync mode
    const uint64_t level = MakeLevelPropId(5);
    CHECK(IsStructure(level, false, 6, 5) && !IsStructure(level, false, 1, 0) && !IsStructure(level, true, 6, 5));
}

int main() {
    TestClearMask(); TestClearWire(); TestClearableProp();
    TestDrops(); TestKept(); TestParentBeatsLevel(); TestLevelRest();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}

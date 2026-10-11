// Unit tests for hsm_gore.h: the pure per-stand-in dislocation + sever-fallback decision state machine.
// Run via implementation/shared/run_tests.sh gore. Style follows protocol/test_hsm_owner.cpp.
#include "combat/hsm_sever.h"   // reference for the spawn-bone table the roots below must match
#include "sync/hsm_gore.h"
#include "sync/hsm_pose.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

// Bit helpers (hsm::kCombatPartBits numbering: bit per Enum_DismembermentPart value).
constexpr uint32_t kHead = 1u << 0;     // neck_01 (decapitation)
constexpr uint32_t kTorso = 1u << 2;    // pelvis -- the one part the fallback cannot represent (see standin_gore.cpp)
constexpr uint32_t kArmR = 1u << 3;
constexpr uint32_t kArmL = 1u << 6;
constexpr uint32_t kLegL = 1u << 12;

// Gore joint bits (hsm_gore.h numbering: ArmR, ArmL, LegR, LegL, Neck, Spine).
constexpr uint32_t kJArmR = 1u << 0;
constexpr uint32_t kJArmL = 1u << 1;
constexpr uint32_t kJLegR = 1u << 2;
constexpr uint32_t kJLegL = 1u << 3;
constexpr uint32_t kJNeck = 1u << 4;
constexpr uint32_t kJSpine = 1u << 5;

static void TestGracePeriodBoundaries() {
    GoreTracker t;
    GoreActions a = t.OnVitals(kArmR, 0, 0.0);               // first report of a severed part at t=0
    CHECK(a.showFallback == 0 && !a.reset);                  // nothing due yet
    a = t.Tick(kSeverGraceMs - 1.0);
    CHECK(a.showFallback == 0);                              // one ms short of the grace period: still not due
    a = t.Tick(kSeverGraceMs - 0.001);
    CHECK(a.showFallback == 0);
    a = t.Tick(kSeverGraceMs);
    CHECK(a.showFallback == kArmR);                          // exactly the grace period: due now
    a = t.Tick(kSeverGraceMs + 50.0);
    CHECK(a.showFallback == 0);                              // never fires twice for the same part (idempotent)
}

static void TestGraceTimerStartsWhenFirstReported() {
    GoreTracker t;
    t.OnVitals(0, 0, 1000.0);                                // nothing severed yet
    GoreActions a = t.OnVitals(kArmL, 0, 1200.0);             // first seen at t=1200, not t=0
    CHECK(a.showFallback == 0);
    a = t.Tick(1200.0 + kSeverGraceMs - 1.0);
    CHECK(a.showFallback == 0);
    a = t.Tick(1200.0 + kSeverGraceMs);
    CHECK(a.showFallback == kArmL);
}

static void TestNativeAppliedSuppressesFallback() {
    // Native cut already landed before any Vitals reports the part severed: the fallback must never fire.
    GoreTracker t;
    t.NoteNativeSever(3 /* ArmR index */);
    GoreActions a = t.OnVitals(kArmR, 0, 0.0);
    CHECK(a.showFallback == 0);
    a = t.Tick(kSeverGraceMs + 1000.0);
    CHECK(a.showFallback == 0);                              // still never fires

    // Native cut arrives mid-grace-period: cancels the pending timer.
    GoreTracker t2;
    t2.OnVitals(kArmL, 0, 0.0);
    t2.NoteNativeSever(6 /* ArmL index */);
    GoreActions a2 = t2.Tick(kSeverGraceMs + 1.0);
    CHECK(a2.showFallback == 0);
}

static void TestMultipleParts() {
    GoreTracker t;
    t.OnVitals(kArmR, 0, 0.0);
    GoreActions a = t.OnVitals(kArmR | kArmL, 0, 50.0);      // ArmL freshly severed 50 ms later
    CHECK(a.showFallback == 0);
    a = t.Tick(kSeverGraceMs);                                // ArmR's timer (started at 0) is due
    CHECK(a.showFallback == kArmR);
    a = t.Tick(50.0 + kSeverGraceMs);                         // ArmL's timer (started at 50) is now due too
    CHECK(a.showFallback == kArmL);
    // A third part reported together with the first two at a later time gets its own fresh timer.
    a = t.OnVitals(kArmR | kArmL | kHead, 0, 500.0);
    CHECK(a.showFallback == 0);
    a = t.Tick(500.0 + kSeverGraceMs);
    CHECK(a.showFallback == kHead);
}

static void TestIdempotence() {
    GoreTracker t;
    t.OnVitals(kArmR, 0, 0.0);
    GoreActions a = t.Tick(kSeverGraceMs);
    CHECK(a.showFallback == kArmR);
    // Repeated Vitals with the same mask, and repeated Tick calls, must never re-fire.
    for (double ms : {kSeverGraceMs + 1.0, kSeverGraceMs + 100.0, kSeverGraceMs + 10000.0}) {
        a = t.OnVitals(kArmR, 0, ms);
        CHECK(a.showFallback == 0);
        a = t.Tick(ms);
        CHECK(a.showFallback == 0);
    }
}

static void TestRespawnResetOnMaskDrop() {
    GoreTracker t;
    t.OnVitals(kArmR | kArmL, 0, 0.0);
    GoreActions a = t.Tick(kSeverGraceMs);
    CHECK(a.showFallback == (kArmR | kArmL));
    t.OnVitals(kJArmR, kJArmR, 10.0);                         // (unrelated dislocation bit also set, cleared below)
    // A fresh body (respawn) reports nothing severed even though this tracker had seen severed bits:
    // the real game never un-severs a part on a live body, so this is unambiguously a reset.
    a = t.OnVitals(0, 0, 20.0);
    CHECK(a.reset);
    // Nothing should be pending or re-fire from the old state, and a part severed again afterwards
    // gets a brand-new grace timer from the new report time.
    a = t.Tick(20.0 + kSeverGraceMs);
    CHECK(a.showFallback == 0);
    a = t.OnVitals(kArmR, 0, 1000.0);
    CHECK(a.showFallback == 0 && !a.reset);
    a = t.Tick(1000.0 + kSeverGraceMs);
    CHECK(a.showFallback == kArmR);
}

static void TestExplicitReset() {
    GoreTracker t;
    t.OnVitals(kArmR, kJArmR, 0.0);
    t.Reset();
    GoreActions a = t.Tick(kSeverGraceMs);
    CHECK(a.showFallback == 0);                               // the pending ArmR timer is gone
    a = t.OnVitals(kArmR, kJArmR, 0.0);                        // behaves like a brand-new tracker
    CHECK(a.dislocate == kJArmR);
}

static void TestDislocateRelocateTransitions() {
    GoreTracker t;
    GoreActions a = t.OnVitals(0, kJNeck, 0.0);
    CHECK(a.dislocate == kJNeck && a.relocate == 0);
    a = t.OnVitals(0, kJNeck, 100.0);                          // still dislocated: no repeat
    CHECK(a.dislocate == 0 && a.relocate == 0);
    a = t.OnVitals(0, kJNeck | kJSpine, 200.0);                // Spine newly dislocated too
    CHECK(a.dislocate == kJSpine && a.relocate == 0);
    a = t.OnVitals(0, kJSpine, 300.0);                         // Neck cleared: relocate once
    CHECK(a.dislocate == 0 && a.relocate == kJNeck);
    a = t.OnVitals(0, kJSpine, 400.0);                         // no repeat relocate
    CHECK(a.dislocate == 0 && a.relocate == 0);
    a = t.OnVitals(0, 0, 500.0);                               // Spine clears too
    CHECK(a.relocate == kJSpine);
}

static void TestInvalidBitsIgnored() {
    GoreTracker t;
    // Severed bits at/above bit 15, and joint bits at/above bit 6, do not exist and must be masked off
    // rather than corrupting state or ever being reported back.
    GoreActions a = t.OnVitals(0xFFFFFFFFu, 0xFFFFFFFFu, 0.0);
    CHECK((a.dislocate & ~kGoreJointMask) == 0);
    CHECK(a.dislocate == kGoreJointMask);
    a = t.Tick(kSeverGraceMs);
    CHECK((a.showFallback & ~kGoreSeveredMask) == 0);
    CHECK(a.showFallback == kGoreSeveredMask);
    t.NoteNativeSever(200);                                    // out-of-range part index: ignored, must not crash
    GoreTracker t2;                                            // only stray bits: nothing at all
    GoreActions b = t2.OnVitals(~kGoreSeveredMask, ~kGoreJointMask, 0.0);
    CHECK(b.dislocate == 0 && b.relocate == 0 && !b.reset);
    CHECK(t2.Tick(kSeverGraceMs).showFallback == 0);
}

static void TestUnrelatedPartsDoNotStartTimers() {
    GoreTracker t;
    t.OnVitals(kArmR, 0, 0.0);
    GoreActions a = t.Tick(kSeverGraceMs);
    CHECK(a.showFallback == kArmR);
    a = t.Tick(kSeverGraceMs + 10000.0);
    CHECK(a.showFallback == 0);                                // kArmL, kLegL, kTorso etc. were never reported
    (void)kLegL; (void)kTorso;
}

static void TestAnyClockOrigin() {
    // hsm_gore.h promises that which clock the caller uses does not matter: a clock whose reading happens to be
    // -1.0 (or negative at all) when a part is first reported must still time out like any other.
    for (double t0 : {-1.0, -400.0, -0.5}) {
        GoreTracker t;
        GoreActions a = t.OnVitals(kArmR, 0, t0);
        CHECK(a.showFallback == 0);
        a = t.Tick(t0 + kSeverGraceMs - 1.0);
        CHECK(a.showFallback == 0);
        a = t.Tick(t0 + kSeverGraceMs);
        CHECK(a.showFallback == kArmR);
        a = t.Tick(t0 + kSeverGraceMs + 1000.0);
        CHECK(a.showFallback == 0);
    }
}

static void TestConstantsPinned() {
    // The header duplicates hsm::kCombatPartBits and the Willie_BP *_Dislocated field count by hand: pin them.
    CHECK(kGoreSeveredBits == 15 && kGoreSeveredMask == 0x7FFFu);    // Enum_DismembermentPart values 0..14
    CHECK(kGoreJoints == 6 && kGoreJointMask == 0x3Fu);              // ArmR, ArmL, LegR, LegL, Neck, Spine
    // The grace period has to ride out a few dropped 15 Hz Vitals samples (66.7 ms each) without leaving a
    // severed limb visibly attached for long.
    CHECK(kSeverGraceMs >= 250.0 && kSeverGraceMs <= 1000.0);
}

static void TestEveryPartAndJointIndividually() {
    for (uint8_t part = 0; part < kGoreSeveredBits; ++part) {
        const uint32_t bit = 1u << part;
        GoreTracker t;
        GoreActions a = t.OnVitals(bit, 0, 100.0);
        CHECK(a.showFallback == 0 && !a.reset);
        a = t.Tick(100.0 + kSeverGraceMs);
        CHECK(a.showFallback == bit);                            // only that part, and every part has a working timer
        GoreTracker n;
        n.NoteNativeSever(part);                                 // ... and a working native-cut suppression
        n.OnVitals(bit, 0, 100.0);
        CHECK(n.Tick(100.0 + kSeverGraceMs).showFallback == 0);
    }
    for (uint32_t joint = 0; joint < kGoreJoints; ++joint) {
        const uint32_t bit = 1u << joint;
        GoreTracker t;
        GoreActions a = t.OnVitals(0, bit, 0.0);
        CHECK(a.dislocate == bit && a.relocate == 0);
        a = t.OnVitals(0, 0, 66.0);
        CHECK(a.dislocate == 0 && a.relocate == bit);
    }
}

static void TestStrayHighBitsAreNotRemembered() {
    // One Vitals sample with a stray bit above the valid ranges must neither be thrown away nor poison the
    // tracker: the next clean sample is not a "respawn" (the stray bit was never a severed part).
    GoreTracker t;
    GoreActions a = t.OnVitals(kArmR | (1u << 20), kJNeck | (1u << 10), 0.0);
    CHECK(a.dislocate == kJNeck && !a.reset);
    a = t.OnVitals(kArmR, kJNeck, 66.0);
    CHECK(!a.reset && a.relocate == 0 && a.dislocate == 0);
    a = t.Tick(kSeverGraceMs);
    CHECK(a.showFallback == kArmR);                              // the timer from the first sample was kept
}

static void TestNativeNoteAffectsOnlyThatPart() {
    GoreTracker t;
    t.OnVitals(kArmR | kArmL, 0, 0.0);
    t.NoteNativeSever(3 /* ArmR */);
    GoreActions a = t.Tick(kSeverGraceMs);
    CHECK(a.showFallback == kArmL);                              // ArmL still gets its fallback
    t.NoteNativeSever(15);                                       // first invalid index and the largest one: ignored
    t.NoteNativeSever(255);
    a = t.OnVitals(kArmR | kArmL | kHead, 0, 1000.0);
    CHECK(a.showFallback == 0);
    CHECK(t.Tick(1000.0 + kSeverGraceMs).showFallback == kHead);   // a stray note did not suppress anything real
}

static void TestResetForgetsNativeNotes() {
    GoreTracker t;
    t.NoteNativeSever(3 /* ArmR */);
    t.Reset();                                                   // a respawn: the old body's cut says nothing about the new one
    t.OnVitals(kArmR, 0, 0.0);
    CHECK(t.Tick(kSeverGraceMs).showFallback == kArmR);
}

static void TestVitalsReportsADueFallbackItself() {
    // A caller that only feeds Vitals (no per-frame Tick yet) still gets the fallback on the first sample past
    // the grace period, exactly once.
    GoreTracker t;
    t.OnVitals(kArmR, 0, 0.0);
    GoreActions a = t.OnVitals(kArmR, 0, kSeverGraceMs);
    CHECK(a.showFallback == kArmR);
    CHECK(t.Tick(kSeverGraceMs).showFallback == 0);
    CHECK(t.OnVitals(kArmR, 0, kSeverGraceMs + 66.0).showFallback == 0);
}

// After a respawn notice the old body's in-flight samples (its gore) must not reach the new body: they are ignored until
// the first whole sample, which ends the wait; gore reported after that is the new body's.
static void TestAwaitWholeIgnoresTheOldBody() {
    GoreTracker t;
    t.OnVitals(kArmR, kJNeck, 0.0);                              // the old body
    t.AwaitWhole();                                              // respawned
    GoreActions a = t.OnVitals(kArmR, kJNeck, 100.0);            // a stale sample of the old body
    CHECK(a.dislocate == 0 && a.relocate == 0 && a.showFallback == 0 && !a.reset);
    CHECK(t.Tick(100.0 + kSeverGraceMs).showFallback == 0);      // it started no grace timer
    a = t.OnVitals(0, 0, 200.0);                                 // the new body, whole
    CHECK(a.dislocate == 0 && a.relocate == 0 && a.showFallback == 0 && !a.reset);
    a = t.OnVitals(kArmR, kJNeck, 300.0);                        // the new body's own gore: taken as usual
    CHECK(a.dislocate == kJNeck);
    CHECK(t.Tick(300.0 + kSeverGraceMs).showFallback == kArmR);
}

static void TestAwaitWholeEnds() {
    // Stray high bits are not gore: a sample with only those is whole.
    GoreTracker t;
    t.AwaitWhole();
    t.OnVitals(~kGoreSeveredMask, ~kGoreJointMask, 0.0);
    CHECK(t.OnVitals(0, kJArmL, 10.0).dislocate == kJArmL);
    // The new body was hurt before any whole sample got through: its gore is taken once it has lasted kAwaitWholeMs from
    // the first ignored sample (clock origin 0 included).
    GoreTracker u;
    u.AwaitWhole();
    CHECK(u.OnVitals(kArmL, kJArmR, 0.0).dislocate == 0);
    CHECK(u.OnVitals(kArmL, kJArmR, kAwaitWholeMs - 1.0).dislocate == 0);
    CHECK(u.Tick(kAwaitWholeMs - 1.0).showFallback == 0);
    GoreActions a = u.OnVitals(kArmL, kJArmR, kAwaitWholeMs);
    CHECK(a.dislocate == kJArmR && !a.reset);
    CHECK(u.Tick(kAwaitWholeMs + kSeverGraceMs).showFallback == kArmL);
    CHECK(u.OnVitals(kArmL, kJArmR | kJLegR, kAwaitWholeMs + 600.0).dislocate == kJLegR);   // the wait is over for good
    // Reset (level change, first attach) does not wait: the first sample is taken as it is.
    GoreTracker v;
    v.AwaitWhole();
    v.Reset();
    CHECK(v.OnVitals(0, kJSpine, 0.0).dislocate == kJSpine);
    CHECK(kAwaitWholeMs == 1500.0);
}

// StandInGore's bookkeeping. What is owed to an entity accumulates the tracker's decisions (a reset drops what came before
// it); the record of one actor keeps a part or joint from being done twice.
static void TestOwedAccumulates() {
    GoreOwed o;
    GoreActions a; a.showFallback = kArmR; a.dislocate = kJNeck | kJArmL;
    o.Apply(a);
    CHECK(o.due == kArmR && o.dislocated == (kJNeck | kJArmL));
    GoreActions b; b.showFallback = kHead; b.relocate = kJArmL;
    o.Apply(b);
    CHECK(o.due == (kArmR | kHead) && o.dislocated == kJNeck);
    GoreActions r; r.reset = true; r.dislocate = kJSpine; r.showFallback = kLegL;
    o.Apply(r);
    CHECK(o.due == kLegL && o.dislocated == kJSpine);
    GoreActions both; both.dislocate = kJArmR; both.relocate = kJArmR;   // never both for one joint, but relocate wins
    o.Apply(both);
    CHECK(o.dislocated == kJSpine);
}

static void TestRecordRefusesRepeats() {
    GoreRecord r;
    CHECK(r.PartsToShow(kArmR | kHead) == (kArmR | kHead));
    CHECK(r.PartsToShow(0xFFFFFFFFu) == kGoreSeveredMask);
    r.hidden = kArmR; r.native = kHead; r.gaveUp = kTorso;
    CHECK(r.PartsToShow(kArmR | kHead | kTorso | kLegL) == kLegL);
    CHECK(r.JointsToFree(kJArmL) == kJArmL);
    CHECK(r.JointsToFree(0xFFFFFFFFu) == kGoreJointMask);
    r.freed = kJNeck; r.jointsGaveUp = kJSpine;
    CHECK(r.JointsToFree(kJNeck | kJSpine | kJArmR) == kJArmR);
    CHECK(r.DeadBones() == GoreDeadBones(kArmR, kHead));
    GoreRecord n; n.native = kArmL;                              // the game's own cut takes its subtree out too
    CHECK(n.DeadBones() == GoreDeadBones(0, kArmL) && n.DeadBones() != 0);
    // A native cut, then a reordered sample reads as a respawn and the tracker owes the fallback again: the record refuses it,
    // so one actor never gets both.
    GoreTracker t; GoreOwed o; GoreRecord rec;
    o.Apply(t.OnVitals(kArmR, 0, 0.0));
    t.NoteNativeSever(3); rec.native |= kArmR;
    o.Apply(t.OnVitals(0, 0, 100.0));
    o.Apply(t.OnVitals(kArmR, 0, 120.0));
    o.Apply(t.Tick(120.0 + kSeverGraceMs));
    CHECK(o.due == kArmR);
    CHECK(rec.PartsToShow(o.due) == 0);
}

// Pose-bone bit (sync/hsm_pose.h numbering) helpers for the dead-bone tests.
static constexpr uint32_t B(int pose) { return 1u << pose; }
constexpr int kBNeck01 = 6, kBNeck02 = 7, kBHead = 8, kBHandL = 12, kBUpperArmR = 14, kBLowerArmR = 15, kBHandR = 16;
constexpr int kBThighL = 17, kBCalfL = 18, kBFootL = 19;

static void TestRootsMatchTheGame() {
    // The fallback hides the part's spawn bone (Willie_BP.cpp:3269, hsm::combat::SeverSpawnBone); the game's own
    // cut hides "Hide Bone Local" (Willie_BP.cpp:3270). Both tables are pose-bone indices and must name those bones.
    static const char* const kHideLocal[kGoreSeveredBits] = {nullptr, nullptr, nullptr, "lowerarm_r", "hand_r", nullptr, "lowerarm_l",
                                                             "hand_l", nullptr, "calf_r", "foot_r", nullptr, "calf_l", "foot_l", nullptr};
    for (uint8_t part = 0; part < kGoreSeveredBits; ++part) {
        const char* spawn = hsm::combat::SeverSpawnBone(part);
        const int fb = kGoreFallbackRoot[part];
        if (part == 2) CHECK(fb == -1);                          // the pelvis cannot be hidden without hiding the whole body
        else CHECK(fb >= 0 && fb < kPoseBones && std::strcmp(kPoseBoneNames[fb], spawn) == 0);
        const int nat = kGoreNativeRoot[part];
        if (!kHideLocal[part]) CHECK(nat == -1);
        else CHECK(nat >= 0 && nat < kPoseBones && std::strcmp(kPoseBoneNames[nat], kHideLocal[part]) == 0);
    }
}

static void TestDeadBones() {
    CHECK(GoreDeadBones(0, 0) == 0);
    // The fallback removes the spawn bone and everything below it.
    CHECK(GoreDeadBones(kArmR, 0) == (B(kBUpperArmR) | B(kBLowerArmR) | B(kBHandR)));
    CHECK(GoreDeadBones(1u << 8 /* hand L */, 0) == B(kBHandL));
    CHECK(GoreDeadBones(kLegL, 0) == (B(kBThighL) | B(kBCalfL) | B(kBFootL)));
    CHECK(GoreDeadBones(kHead, 0) == (B(kBNeck01) | B(kBNeck02) | B(kBHead)));
    CHECK(GoreDeadBones(1u << 1, 0) == GoreDeadBones(kHead, 0));    // part 1 shares part 0's spawn bone
    CHECK(GoreDeadBones(1u << 5 /* hand R */, 0) == B(kBHandR));
    CHECK(GoreDeadBones(1u << 11 /* foot R */, 0) == B(22));
    CHECK(GoreDeadBones(1u << 14 /* foot L */, 0) == B(kBFootL));    // the last part: the loop must reach it
    CHECK(GoreDeadBones(kTorso, 0) == 0);                            // the fallback cannot represent the pelvis: nothing is called dead
    // The game's own cut leaves the part's own bone and removes only what "Hide Bone Local" names and below.
    CHECK(GoreDeadBones(0, kArmR) == (B(kBLowerArmR) | B(kBHandR)));
    CHECK(GoreDeadBones(0, 1u << 4 /* forearm R */) == B(kBHandR));
    CHECK(GoreDeadBones(0, 1u << 5 /* hand R */) == 0);              // a hand cut hides no bone
    CHECK(GoreDeadBones(0, kHead) == 0);                              // a neck cut is vertex paint only
    CHECK(GoreDeadBones(0, kLegL) == (B(kBCalfL) | B(kBFootL)));
    // Both at once is the union; the same part in both is the larger set.
    CHECK(GoreDeadBones(kHead, kArmR) == (B(kBNeck01) | B(kBNeck02) | B(kBHead) | B(kBLowerArmR) | B(kBHandR)));
    CHECK(GoreDeadBones(kArmR, kArmR) == GoreDeadBones(kArmR, 0));
    // Bits that are not parts are ignored, and no cut ever kills the trunk.
    CHECK(GoreDeadBones(0xFFFF8000u, 0xFFFF8000u) == 0);
    const uint32_t all = GoreDeadBones(kGoreSeveredMask, kGoreSeveredMask);
    for (int trunk : {0, 1, 2, 3, 4, 5, 9, 13}) CHECK((all & B(trunk)) == 0);   // pelvis, spine_01..05, both clavicles
    CHECK(all == (B(kBNeck01) | B(kBNeck02) | B(kBHead) | B(10) | B(11) | B(12) | B(14) | B(15) | B(16) | B(17) | B(18) | B(19) |
                  B(20) | B(21) | B(22)));
}

int main() {
    TestGracePeriodBoundaries();
    TestGraceTimerStartsWhenFirstReported();
    TestNativeAppliedSuppressesFallback();
    TestMultipleParts();
    TestIdempotence();
    TestRespawnResetOnMaskDrop();
    TestExplicitReset();
    TestDislocateRelocateTransitions();
    TestInvalidBitsIgnored();
    TestUnrelatedPartsDoNotStartTimers();
    TestAnyClockOrigin();
    TestConstantsPinned();
    TestEveryPartAndJointIndividually();
    TestStrayHighBitsAreNotRemembered();
    TestNativeNoteAffectsOnlyThatPart();
    TestResetForgetsNativeNotes();
    TestVitalsReportsADueFallbackItself();
    TestAwaitWholeIgnoresTheOldBody();
    TestAwaitWholeEnds();
    TestOwedAccumulates();
    TestRecordRefusesRepeats();
    TestRootsMatchTheGame();
    TestDeadBones();
    std::printf(g_fail ? "%d FAILED\n" : "all gore state machine tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

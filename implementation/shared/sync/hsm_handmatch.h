// What a stand-in's hands hold, matched to what its owner's pose shows (owner sync, Phase 4b props).
//
// A stand-in's tick is blocked, so its hands change only when this mod changes them: Equip mirrors draws and sheathes
// (combat/hsm_equip.h), prop_sync lets go of a weapon the owner dropped. Two things were missing (two-player test 3):
//   a pickup  the owner took a loose weapon (one dropped earlier, or one lying in the level) and the stand-in's hand
//             stayed empty, while its copy elsewhere was only teleported along the owner's prop stream, or lay where
//             it was;
//   a leftover the stand-in's hand kept a weapon the owner no longer held (put away with no slot token, or dropped
//             without a drop this machine heard of), and that hand could then neither draw nor pick up.
// Per stand-in hand, HandMatch decides from the owner's newest pose (the weapon class per hand, WeaponSample::classHash)
// and its Equip token for that hand:
//   Attach   the hand is empty, the pose shows a known class there, the Equip token is other (not a draw), and the
//            pose has shown that class for kAttachGraceMs -- kDrawGraceMs when the stand-in has a sheathed weapon of that
//            class the hand reaches and the owner still shows that slot sheathed, as a draw looks until its token comes.
//            PickAttach then names the loose weapon to take: the nearest of that class within kAttachRadiusUu of the
//            owner's weapon now or where it was when the pose first showed it (a weapon lying here where it lay there);
//   Release  the hand holds a weapon of a known class and the pose has shown that hand empty or holding another known
//            class for kReleasePropMs (a synced prop: the prop stream carries it on) or kReleaseOtherMs (anything else:
//            longer than a drop's Grant takes to arrive, so prop_sync's own drop gets it first), not while prop_sync is
//            resolving a drop of that hand; and a pose showing the hand empty lets go of the weapon of the class it
//            showed last only kReattachMinMs after the last step that showed it (test 5, 10:22:26-29: the owner's pose
//            flickered empty for half a second now and then while it held a loose weapon, and the stand-in took that
//            prop and let it go four times in three seconds).
// A pose missing or of unknown class decides nothing, starts the release clock over and is no class shown.
//
// A drop of a stand-in's hand (prop_sync, heard of as its Touch Grant) resolves only against the weapon that hand held
// when the drop was heard, its witness (test 5: a drop heard while the hand was empty fired its deadline at a weapon
// Equip drew into that hand since, and let it go): StepDrop lets the witness go once the pose shows the hand let go, or
// at the deadline; with no witness in hand the drop waits and is then given up, never forced. A weapon the stand-in let
// go of from that hand already is the drop's only by AdoptCandidate. Pure, no engine headers.
#pragma once
#include "sync/hsm_pose.h"
#include <cstdint>
#include <vector>

namespace hsm::sync {

constexpr uint32_t kHandEmpty = 0;          // pose hand class: nothing in hand (fists are never sampled)
constexpr uint32_t kHandUnknown = 1;        // pose hand class: a weapon whose class hash is unknown
constexpr double kAttachGraceMs = 250.0;
constexpr double kDrawGraceMs = 1500.0;
constexpr double kReleasePropMs = 250.0;
constexpr double kReleaseOtherMs = 3000.0;
constexpr double kReattachMinMs = 2000.0;   // the hold above; prop_sync takes back a weapon that left a hand by itself at
                                            // most this often
constexpr double kAttachRadiusUu = 150.0;   // the pose's own "held" distance (standin_rig kHeldMaxUu)
constexpr double kAttachFarRadiusUu = 600.0;   // PickAttachFar: a copy drifted this far still stands for the owner's (test 7)
constexpr double kStandInSettleMs = 1500.0;    // a new stand-in's own loadout weapons arrive within this (logs 3-9: 0.85-1.2 s)

struct HandFacts {
    bool havePose = false;
    uint32_t poseClass = kHandEmpty;        // the owner's newest pose, this hand
    double poseSinceMs = 0;                 // local time the pose's class for this hand last changed
    bool tokenKnown = false;                // an Equip state from the owner has arrived
    uint8_t token = 0;                      // its token for this hand: 0 other, k + 1 drawn from slot k
    bool mayBeDraw = false;                 // see Attach above
    bool holding = false;                   // the stand-in's hand holds a real weapon (not fists)
    uint32_t heldClass = 0;                 // its class hash, 0 unknown
    bool heldIsProp = false;                // it is a synced prop (prop_sync tracks it)
    bool dropPending = false;               // prop_sync is resolving a drop of this hand's weapon
    double boundMs = -kStandInSettleMs;     // local time the stand-in was bound (new or respawned); the default is settled
};

enum class HandAct : uint8_t { None, Attach, Release };

class HandMatch {
public:
    HandAct Step(const HandFacts& f, double nowMs) {
        if (f.havePose && f.poseClass != kHandEmpty && f.poseClass != kHandUnknown) { shownClass_ = f.poseClass; shownMs_ = nowMs; }
        if (!f.havePose || f.poseClass == kHandUnknown || !f.holding || f.heldClass == 0 || f.heldClass == f.poseClass)
            mismatch_ = false;
        if (!f.havePose || f.poseClass == kHandUnknown) return HandAct::None;
        if (!f.holding) {
            if (f.poseClass == kHandEmpty || !f.tokenKnown || f.token != 0) return HandAct::None;
            if (nowMs - f.boundMs < kStandInSettleMs) return HandAct::None;   // its own loadout is still on its way
            const double grace = f.mayBeDraw ? kDrawGraceMs : kAttachGraceMs;
            return nowMs - f.poseSinceMs >= grace ? HandAct::Attach : HandAct::None;
        }
        if (f.heldClass == 0 || f.heldClass == f.poseClass) return HandAct::None;
        if (!mismatch_) { mismatch_ = true; mismatchSinceMs_ = nowMs; }
        if (f.dropPending) return HandAct::None;
        // Held while the pose shows the hand empty and showed the held class last (another known class shown is the last).
        if (shownClass_ == f.heldClass && nowMs - shownMs_ < kReattachMinMs) return HandAct::None;
        const double wait = f.heldIsProp ? kReleasePropMs : kReleaseOtherMs;
        return nowMs - mismatchSinceMs_ >= wait ? HandAct::Release : HandAct::None;
    }
    void Reset() { mismatch_ = false; }   // what the pose showed is the pose's, not the held weapon's: kept

private:
    bool mismatch_ = false;
    double mismatchSinceMs_ = 0;
    uint32_t shownClass_ = kHandEmpty;      // the last known class the pose showed in this hand, at shownMs_
    double shownMs_ = 0;
};

struct DropFacts {
    bool witnessed = false;                 // that hand held a weapon prop_sync does not track when the drop was heard
    bool holdsWitness = false;              // ... and holds that very weapon now
    bool poseShowsLetGo = false;            // ResolveDropNow: the owner's newest pose shows that hand empty or another class
    bool deadline = false;                  // the drop's resolve window is over
};

enum class DropAct : uint8_t { Wait, Release, GiveUp };

inline DropAct StepDrop(const DropFacts& f) {
    if (f.witnessed && f.holdsWitness) return f.poseShowsLetGo || f.deadline ? DropAct::Release : DropAct::Wait;
    return f.deadline ? DropAct::GiveUp : DropAct::Wait;
}

// A weapon the stand-in let go of from the drop's hand, at letGoMs (loose here, not a prop), is the drop's: the witness
// itself, or, when that hand held none as the drop was heard (heardMs), one let go of before (the hand matching or the
// weapon itself was quicker than the Grant). Never one let go of since: that hand drew or picked it up after the drop.
inline bool AdoptCandidate(bool witnessed, bool isWitness, double letGoMs, double heardMs) {
    return witnessed ? isWitness : letGoMs <= heardMs;
}

struct LooseWeapon {
    V3 pos;                                 // actor location, world
    uint32_t classHash = 0;
};

// Index into loose of the weapon of class cls nearest to either point (the owner's weapon now, and where the pose first
// showed it), within kAttachRadiusUu; -1 when none, and always for the empty or unknown class. Ties: the first.
namespace detail {
inline int PickAttachWithin(const std::vector<LooseWeapon>& loose, uint32_t cls, const V3& now, const V3& firstAt, double radius) {
    if (cls == kHandEmpty || cls == kHandUnknown) return -1;
    auto dist2 = [](const V3& a, const V3& b) {
        const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };
    int best = -1;
    double bestD2 = radius * radius;
    for (size_t i = 0; i < loose.size(); ++i) {
        if (loose[i].classHash != cls) continue;
        const double a = dist2(loose[i].pos, now), b = dist2(loose[i].pos, firstAt);
        const double d2 = a < b ? a : b;
        if (d2 <= bestD2 && (best < 0 || d2 < bestD2)) { best = int(i); bestD2 = d2; }
    }
    return best;
}
}  // namespace detail
inline int PickAttach(const std::vector<LooseWeapon>& loose, uint32_t cls, const V3& now, const V3& firstAt) {
    return detail::PickAttachWithin(loose, cls, now, firstAt, kAttachRadiusUu);
}
// The fallback once PickAttach found none (test 7: a loose copy here had drifted past kAttachRadiusUu from the owner's,
// and the stand-in's hand stayed empty): the same pick within kAttachFarRadiusUu.
inline int PickAttachFar(const std::vector<LooseWeapon>& loose, uint32_t cls, const V3& now, const V3& firstAt) {
    return detail::PickAttachWithin(loose, cls, now, firstAt, kAttachFarRadiusUu);
}

}  // namespace hsm::sync

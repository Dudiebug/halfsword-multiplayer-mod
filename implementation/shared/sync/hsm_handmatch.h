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
#include <cmath>
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
constexpr double kAmbiguousMarginUu = 30.0;    // PickGuarded: two copies of the class this close in distance are not told apart (see there)
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

// Why PickGuarded took or refused a weapon (logged: one log settles which rule fired).
enum class PickWhy : uint8_t {
    None,           // no loose copy of the class within kAttachFarRadiusUu (or the empty / unknown class)
    Near,           // the nearest within kAttachRadiusUu, clearly nearer than any other copy
    FarOnly,        // no copy within kAttachRadiusUu; exactly one within kAttachFarRadiusUu (test 7: a drifted copy)
    AmbiguousNear,  // two or more copies within kAttachRadiusUu, the two nearest less than kAmbiguousMarginUu apart
    AmbiguousFar    // none near; two or more within kAttachFarRadiusUu
};
struct GuardedPick {
    int index = -1;             // into loose; -1 refuses
    PickWhy why = PickWhy::None;
    int inRange = 0;            // copies of the class within the radius the rule looked at (near, or far)
    double distUu = 0;          // distance of the chosen copy, or of the nearest when refused
};

// The guarded pick (bugfix/weapon-pick-guard). The pose carries only a class hash, so with two copies of a class around
// the nearest one is often not the owner's (player reports: the wrong hand, an invisible weapon, one taken off the floor or
// out of another fighter's hand). PickAttach alone took the nearest within 150 uu, and PickAttachFar anything within 600.
//   near   the nearest copy within kAttachRadiusUu, unless the next nearest is within kAmbiguousMarginUu of its distance
//          (30 uu: about a weapon's hilt-to-guard; the pose's own position is only good to a similar error, so a smaller gap
//          is a coin toss); a lone copy, or one clearly nearer, is taken;
//   far    only when no copy is within kAttachRadiusUu AND exactly one copy of the class lies within kAttachFarRadiusUu.
// A refusal waits: the owner's weapon moves and the pose changes, and the next try sees a clearer picture.
inline GuardedPick PickGuarded(const std::vector<LooseWeapon>& loose, uint32_t cls, const V3& now, const V3& firstAt) {
    GuardedPick r;
    if (cls == kHandEmpty || cls == kHandUnknown) return r;
    auto dist = [](const V3& a, const V3& b) {
        const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    int best = -1, nearCount = 0, farCount = 0;
    double bestD = 0, secondD = 0;
    bool haveSecond = false;
    for (size_t i = 0; i < loose.size(); ++i) {
        if (loose[i].classHash != cls) continue;
        const double a = dist(loose[i].pos, now), b = dist(loose[i].pos, firstAt);
        const double d = a < b ? a : b;
        if (d > kAttachFarRadiusUu) continue;
        ++farCount;
        if (d <= kAttachRadiusUu) ++nearCount;
        if (best < 0 || d < bestD) {
            if (best >= 0) { secondD = bestD; haveSecond = true; }
            best = int(i); bestD = d;
        } else if (!haveSecond || d < secondD) { secondD = d; haveSecond = true; }
    }
    if (best < 0) return r;
    r.distUu = bestD;
    if (nearCount > 0) {
        r.inRange = nearCount;
        if (nearCount > 1 && secondD - bestD < kAmbiguousMarginUu) { r.why = PickWhy::AmbiguousNear; return r; }
        r.index = best; r.why = PickWhy::Near;
        return r;
    }
    r.inRange = farCount;
    if (farCount > 1) { r.why = PickWhy::AmbiguousFar; return r; }
    r.index = best; r.why = PickWhy::FarOnly;
    return r;
}


// The object id the owner's pose carried for a hand's weapon (protocol rev 52, feat/weapon-prop-id), against what this
// machine has under that id. Take: attach exactly that actor. Anything else: the guarded class pick (PickGuarded) decides,
// and the reason is logged.
enum class IdAttach : uint8_t {
    NoId,         // the pose carried none (a weapon the owner's machine does not track, or an older sender)
    NoActor,      // the id is not valid, or maps to no live actor here (not announced here yet, or gone)
    WrongClass,   // the actor is of another class than the pose's (the id was reused, or the maps disagree)
    NotFree,      // the actor is held by someone (this stand-in's other hand, another fighter, the local player)
    Take
};
inline IdAttach DecideIdAttach(uint64_t id, bool idValid, bool actorFound, bool classMatches, bool free) {
    if (id == 0) return IdAttach::NoId;
    if (!idValid || !actorFound) return IdAttach::NoActor;
    if (!classMatches) return IdAttach::WrongClass;
    return free ? IdAttach::Take : IdAttach::NotFree;
}

// A hand that already holds a weapon of the pose's class, but not the one the pose names, swaps to the named one once it is
// here and free: an earlier class pick (before the named weapon's announce reached this machine, or a guess between two
// copies) took another copy. Only a settled hand swaps (held class == pose class); a hand changing class is the matcher's.
inline bool SwapToNamed(bool holding, uint32_t heldClass, uint32_t poseClass, bool heldIsNamed, IdAttach named) {
    return holding && heldClass == poseClass && !heldIsNamed && named == IdAttach::Take;
}

}  // namespace hsm::sync

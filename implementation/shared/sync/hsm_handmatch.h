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
//            resolving a drop of that hand.
// A pose missing or of unknown class decides nothing and starts the release clock over. Pure, no engine headers.
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
constexpr double kAttachRadiusUu = 150.0;   // the pose's own "held" distance (standin_rig kHeldMaxUu)

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
};

enum class HandAct : uint8_t { None, Attach, Release };

class HandMatch {
public:
    HandAct Step(const HandFacts& f, double nowMs) {
        if (!f.havePose || f.poseClass == kHandUnknown || !f.holding || f.heldClass == 0 || f.heldClass == f.poseClass)
            mismatch_ = false;
        if (!f.havePose || f.poseClass == kHandUnknown) return HandAct::None;
        if (!f.holding) {
            if (f.poseClass == kHandEmpty || !f.tokenKnown || f.token != 0) return HandAct::None;
            const double grace = f.mayBeDraw ? kDrawGraceMs : kAttachGraceMs;
            return nowMs - f.poseSinceMs >= grace ? HandAct::Attach : HandAct::None;
        }
        if (f.heldClass == 0 || f.heldClass == f.poseClass) return HandAct::None;
        if (!mismatch_) { mismatch_ = true; mismatchSinceMs_ = nowMs; }
        if (f.dropPending) return HandAct::None;
        const double wait = f.heldIsProp ? kReleasePropMs : kReleaseOtherMs;
        return nowMs - mismatchSinceMs_ >= wait ? HandAct::Release : HandAct::None;
    }
    void Reset() { mismatch_ = false; }

private:
    bool mismatch_ = false;
    double mismatchSinceMs_ = 0;
};

struct LooseWeapon {
    V3 pos;                                 // actor location, world
    uint32_t classHash = 0;
};

// Index into loose of the weapon of class cls nearest to either point (the owner's weapon now, and where the pose first
// showed it), within kAttachRadiusUu; -1 when none, and always for the empty or unknown class. Ties: the first.
inline int PickAttach(const std::vector<LooseWeapon>& loose, uint32_t cls, const V3& now, const V3& firstAt) {
    if (cls == kHandEmpty || cls == kHandUnknown) return -1;
    auto dist2 = [](const V3& a, const V3& b) {
        const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };
    int best = -1;
    double bestD2 = kAttachRadiusUu * kAttachRadiusUu;
    for (size_t i = 0; i < loose.size(); ++i) {
        if (loose[i].classHash != cls) continue;
        const double a = dist2(loose[i].pos, now), b = dist2(loose[i].pos, firstAt);
        const double d2 = a < b ? a : b;
        if (d2 <= bestD2 && (best < 0 || d2 < bestD2)) { best = int(i); bestD2 = d2; }
    }
    return best;
}

}  // namespace hsm::sync

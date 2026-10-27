// Where a weapon sits in the hand that holds it, and when a stand-in's seat is wrong enough to make again (pure, no
// engine types).
//
// The rig seats a stand-in's weapon from the owner's grip flags (hsm_grip.h SeatFor), as the game's own re-seat would.
// The owner's game does not always re-seat: an alt grip on an axe slides the hand along the shaft and keeps the weapon's
// turn, so the guessed seat can hold the weapon upside down (mirror test, 2026-10-25: hands 5 uu off, tip 50-125 uu off,
// the axe head up where the owner's hung down). The pose carries the owner's hand_r and weapon root, so the stand-in can
// take the owner's real seat: the weapon root in the hand's frame. HeldCheck says when the stand-in's own seat has
// differed from the owner's for long enough, with the owner's seat steady, to make the joint again there.
#pragma once
#include "sync/hsm_pose.h"
#include "sync/hsm_pose_codec.h"

namespace hsm::sync {

struct HeldPose {
    V3 pos;   // weapon root in the hand bone's frame, uu
    Q4 rot;   // weapon root rotation in the hand bone's frame
};

inline HeldPose HeldIn(V3 handPos, Q4 handRot, V3 weaponPos, Q4 weaponRot) {
    const Q4 inv = QuatConj(QuatNormalize(handRot));
    return HeldPose{QuatRotateVec(inv, Vec3Sub(weaponPos, handPos)), QuatNormalize(QuatMul(inv, QuatNormalize(weaponRot)))};
}

// The weapon root's world pose for a hand at (handPos, handRot) holding it as `held`.
inline void HeldWorld(V3 handPos, Q4 handRot, const HeldPose& held, V3* pos, Q4* rot) {
    const Q4 h = QuatNormalize(handRot);
    *pos = Vec3Add(handPos, QuatRotateVec(h, held.pos));
    *rot = QuatNormalize(QuatMul(h, held.rot));
}

constexpr double kHeldReseatDeg = 25.0;     // a seat turned further than this from the owner's is wrong
constexpr double kHeldReseatUu = 10.0;      // ... or moved further than this along or off the shaft
constexpr double kHeldSteadyDeg = 10.0;     // the owner's seat counts as steady while it stays this close to where it was
constexpr double kHeldSteadyUu = 4.0;
constexpr double kHeldPersistMs = 300.0;    // wrong and steady this long: re-seat (a hand mid-slide is not a seat)
constexpr double kHeldMinGapMs = 1000.0;    // the locked joint needs time to carry the weapon to a new seat

// A flail's chain drags its handle about: a seat that is a little off is the chain's doing, and a re-seat shakes the chain
// (the joint is made again and the chain re-armed), so a flail is re-seated later and less often.
struct HeldTolerance {
    double deg, uu, minGapMs;
};
inline HeldTolerance HeldToleranceFor(bool hasChain) {
    return hasChain ? HeldTolerance{45.0, 20.0, 3000.0} : HeldTolerance{kHeldReseatDeg, kHeldReseatUu, kHeldMinGapMs};
}

inline bool HeldDiffers(const HeldPose& a, const HeldPose& b, double deg, double uu) {
    return QuatAngleDeg(a.rot, b.rot) > deg || Vec3Dist(a.pos, b.pos) > uu;
}

class HeldCheck {
public:
    void Reset() { since_ = -1.0; }
    // Another joint or weapon: the seat last made says nothing about it.
    void Forget() { Reset(); seated_ = false; }
    // True when the stand-in's seat has differed from the owner's for kHeldPersistMs while the owner's seat held steady,
    // and the last re-seat is kHeldMinGapMs old. The caller re-seats at once on true. A seat already made for the owner's
    // current seat is not made again: when the joint could not carry the weapon all the way there (mirror test,
    // 2026-10-25: "28 deg, 0.6 uu off" logged every second), another re-seat at the same place only shakes it. The owner
    // moving the weapon in the hand (kHeldSteadyDeg / kHeldSteadyUu from that seat) allows the next one.
    bool Update(double nowMs, const HeldPose& owner, const HeldPose& standIn, const HeldTolerance& tol = HeldToleranceFor(false)) {
        if (!HeldDiffers(owner, standIn, tol.deg, tol.uu)) { since_ = -1.0; return false; }
        if (since_ < 0.0 || HeldDiffers(owner, anchor_, kHeldSteadyDeg, kHeldSteadyUu)) {
            since_ = nowMs;
            anchor_ = owner;
            return false;
        }
        if (nowMs - since_ < kHeldPersistMs || nowMs - lastSeatMs_ < tol.minGapMs) return false;
        if (seated_ && !HeldDiffers(owner, seatedAt_, kHeldSteadyDeg, kHeldSteadyUu)) return false;
        lastSeatMs_ = nowMs;
        since_ = -1.0;
        seated_ = true;
        seatedAt_ = owner;
        return true;
    }

private:
    double since_ = -1.0;          // when the current wrong, steady stretch began; < 0: none
    double lastSeatMs_ = -1e18;
    HeldPose anchor_;              // the owner's seat at the start of the stretch
    bool seated_ = false;          // a re-seat was made (since Forget) ...
    HeldPose seatedAt_;            // ... at this owner's seat
};

}  // namespace hsm::sync

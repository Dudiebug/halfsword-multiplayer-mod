// Which grip type the stand-in's Willie gets for one hand (pure, no engine types).
//
// The owner's Willie holds R_GripType_Current / L_GripType_Current; the pose carries them (hsm_pose.h GripState). On the
// stand-in a hand that holds a weapon must never be given grip 0: the game's "Set Up ... Hand Weapon" with a null grip
// destroys the weapon (and prop_sync.cpp DropFromHands refuses on 0). Grip 0 reaches such a hand when the owner's hand is
// empty, or holds the weapon only through the offhand constraint (the left hand of a two-handed grip never has a Weapon L,
// so the pose says L grip 0 for a moment), while the stand-in's hand does hold something.
#pragma once
#include <cstdint>

namespace hsm::sync {

// poseGrip: the owner's grip type for this hand (0 = none). standInHandHolds: the stand-in's hand holds a weapon
// (Weapon R / Weapon L non-null). weaponDefaultGrip: that weapon's "Grip R Hand Default" / "Grip L Hand Default".
// A holding hand gets the weapon's default when the pose says 0, otherwise the pose's value; an empty hand gets the
// pose's value unchanged. Returns 0 for a holder only when the weapon's own default is 0 (the caller leaves the field).
constexpr uint8_t GripToApply(uint8_t poseGrip, bool standInHandHolds, uint8_t weaponDefaultGrip) {
    if (standInHandHolds && poseGrip == 0) return weaponDefaultGrip;
    return poseGrip;
}

}  // namespace hsm::sync

// Which grip type the stand-in's Willie gets for one hand, and where its right hand holds the weapon (pure, no engine types).
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


// Where the stand-in's right hand holds its weapon. The game re-seats the weapon in the hand in its ReceiveTick
// (Willie_BP.cpp:18042-18066) after an alt-grip slide or Event Reverse Grip (mordhau, :9907): the weapon is placed at the
// hand socket, turned by the reverse grip, then moved so the chosen attach point sits in the hand, and the hand's joint
// is made again there. A stand-in's ReceiveTick is blocked, so the rig re-seats it (standin_rig.cpp ApplySeat).
enum class SeatPoint : uint8_t { Default, Alt, Reverse };   // Grip R Hand Default Pos / Grip R Hand Alt Pos / ... Pos Reverse
enum class SeatFlip : uint8_t { None, Yaw180, Pitch180 };   // Rot(0,180,0) / Rot(180,0,0) on the socket's rotation
struct GripSeat {
    SeatPoint point = SeatPoint::Default;
    SeatFlip flip = SeatFlip::None;
    bool hammerSocket = false;   // weapon_rSocket_Hammer, not weapon_rSocket
    bool lowerArm = false;       // the joint holds lowerarm_r, not hand_r
};

// gripR: R_GripType_Current; offhand: L Hand In Offhand Attached; reverseType: the stand-in's R Weapon Reverse Grip Type.
constexpr GripSeat SeatFor(uint8_t gripR, bool offhand, bool alt, bool mordhau, bool reverse, uint8_t reverseType) {
    GripSeat s;
    const bool oneHandLong = (gripR == 13 || gripR == 14 || gripR == 16) && !offhand;
    s.point = mordhau ? SeatPoint::Reverse : (alt || gripR == 3 || oneHandLong) ? SeatPoint::Alt : SeatPoint::Default;
    if (reverse && reverseType == 0) s.flip = SeatFlip::Yaw180;
    if (reverse && (reverseType == 1 || reverseType == 2)) s.flip = SeatFlip::Pitch180;
    s.hammerSocket = gripR == 15 || gripR == 5 || gripR == 8;
    s.lowerArm = gripR == 6 || gripR == 7;
    return s;
}

// The owner's toggles that make the game re-seat its weapon: alt grip, mordhau, reverse grip. A new joint starts at 0
// (the game makes it at the weapon's default seat).
constexpr uint8_t SeatKey(bool alt, bool mordhau, bool reverse) {
    return uint8_t((alt ? 1u : 0u) | (mordhau ? 2u : 0u) | (reverse ? 4u : 0u));
}

}  // namespace hsm::sync

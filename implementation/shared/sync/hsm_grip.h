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
// Socket: the weapon's own origin at the socket, no attach point (Event Master Stroke Switch, Willie_BP.cpp:15868).
// LeftDefault: Grip L Hand Default Pos, grip 3's point in build 26820056 (SeatForBuild).
enum class SeatPoint : uint8_t { Default, Alt, Reverse, Socket, LeftDefault };   // Grip R Hand Default Pos / Alt Pos / ... Pos Reverse / none
// Yaw90: Rot(0,90,0), the master stroke grip's turn when the grip is not reversed (:15870, :25862).
enum class SeatFlip : uint8_t { None, Yaw180, Pitch180, Yaw90 };   // Rot(0,180,0) / Rot(180,0,0) on the socket's rotation
struct GripSeat {
    SeatPoint point = SeatPoint::Default;
    SeatFlip flip = SeatFlip::None;
    bool hammerSocket = false;   // weapon_rSocket_Hammer, not weapon_rSocket
    bool shieldSocket = false;   // shield_r_socket (only a master stroke seat picks it: a weapon whose default grip is 6 or 7)
    bool lowerArm = false;       // the joint holds lowerarm_r, not hand_r
    bool jointHammer = false;    // the joint is attached at weapon_rSocket_Hammer whatever socket placed the weapon
};

constexpr SeatFlip ReverseFlip(uint8_t reverseType) {
    return reverseType == 0 ? SeatFlip::Yaw180 : (reverseType == 1 || reverseType == 2) ? SeatFlip::Pitch180 : SeatFlip::None;
}

// gripR: R_GripType_Current; offhand: L Hand In Offhand Attached; reverseType: the stand-in's R Weapon Reverse Grip Type.
// master: Master Stroke Grip (protocol 54), whose turn the game adds when the grip is not reversed (:25862).
constexpr GripSeat SeatFor(uint8_t gripR, bool offhand, bool alt, bool mordhau, bool reverse, uint8_t reverseType,
                           bool master = false) {
    GripSeat s;
    const bool oneHandLong = (gripR == 13 || gripR == 14 || gripR == 16) && !offhand;
    s.point = mordhau ? SeatPoint::Reverse : (alt || gripR == 3 || oneHandLong) ? SeatPoint::Alt : SeatPoint::Default;
    if (reverse) s.flip = ReverseFlip(reverseType);
    else if (master) s.flip = SeatFlip::Yaw90;
    s.hammerSocket = gripR == 15 || gripR == 5 || gripR == 8;
    s.lowerArm = gripR == 6 || gripR == 7;
    return s;
}

// The re-seat's inputs as build 26820056 reads them (Willie_BP.cpp:25864-25868).
struct SeatInput {
    uint8_t gripR = 0;           // R_GripType_Current
    bool offhand = false;        // L Hand In Offhand Attached
    bool alt = false;            // R Alt Grip, as the rig counts it (SeatKeyFor)
    bool mordhau = false;
    bool reverse = false;        // R Hand Reverse Grip
    uint8_t reverseType = 3;     // R Weapon Reverse Grip Type
    bool master = false;         // Master Stroke Grip
    bool offSearching = false;   // L Offhand Searching Grip (protocol 54)
};

// The attach point as build 26820056's re-seat picks it (R Hand Attach Point, :25864): mordhau the reverse point; the alt
// grip, or 13, 14, 16 with the off hand neither on the shaft nor reaching for it, the alt point; grip 3 the left hand's
// default point; else the default point. SeatFor is the earlier rule (grip 3 at the alt point, the reach ignored).
// With the hands switched the game places the weapon by Grip L Hand Off Hand (:25868); the rig seats it at the point and
// lets the hand switch's slide (ApplySwitch, made again with the new joint) move it there, so that is not part of this.
constexpr GripSeat SeatForBuild(const SeatInput& in) {
    GripSeat s = SeatFor(in.gripR, in.offhand, in.alt, in.mordhau, in.reverse, in.reverseType, in.master);
    if (in.mordhau) return s;
    const bool oneHandLong = (in.gripR == 13 || in.gripR == 14 || in.gripR == 16) && !in.offSearching && !in.offhand;
    s.point = (in.alt || oneHandLong) ? SeatPoint::Alt : in.gripR == 3 ? SeatPoint::LeftDefault : SeatPoint::Default;
    return s;
}

// The seat Event Master Stroke Switch makes when Master Stroke Grip turns on or off with the right hand holding
// (Willie_BP.cpp:15864-15873): the weapon's origin at the socket the weapon's default grip names (hammer for 5, 8 or the
// hands switched, shield_r_socket for 6, 7, else weapon_rSocket), turned by the reverse grip, else by the master stroke,
// with no attach point; the joint is attached at weapon_rSocket_Hammer and holds lowerarm_r in grip 6 or 7.
// weaponDefault: the weapon's Grip R Hand Default. With the hands switched the game re-makes the left hand's joint
// instead (:15847), which this seat does not cover.
constexpr GripSeat MasterSeatFor(uint8_t gripR, uint8_t weaponDefault, bool switched, bool master, bool reverse,
                                 uint8_t reverseType) {
    GripSeat s;
    s.point = SeatPoint::Socket;
    s.flip = reverse ? ReverseFlip(reverseType) : master ? SeatFlip::Yaw90 : SeatFlip::None;
    s.hammerSocket = weaponDefault == 5 || weaponDefault == 8 || switched;
    s.shieldSocket = !s.hammerSocket && (weaponDefault == 6 || weaponDefault == 7);
    s.lowerArm = gripR == 6 || gripR == 7;
    s.jointHammer = true;
    return s;
}

// A shield or lantern grip (5 Buckler, 6 Shield, 7 Shield_Heavy, 8 Shield_BarGrip, 9 Lantern): ABP_Willie poses these
// hands from fields besides the grip type (a buckler shares the bare hand's branch while its Thrust Alpha is 0;
// build 26568321 added two left-hand layers for 5 to 8), so the rig logs those fields while one is held (#3282).
constexpr bool ShieldHandGrip(uint8_t grip) { return grip >= 5 && grip <= 9; }

// The owner's toggles that make the game re-seat its weapon: alt grip, mordhau, reverse grip. A new joint starts at 0
// (the game makes it at the weapon's default seat).
constexpr uint8_t SeatKey(bool alt, bool mordhau, bool reverse) {
    return uint8_t((alt ? 1u : 0u) | (mordhau ? 2u : 0u) | (reverse ? 4u : 0u));
}

// The grips the game counts as two-handed (Willie_BP ReceiveTick, "R Two Handed Grip").
constexpr bool TwoHandedGrip(uint8_t gripR) { return gripR >= 10 && gripR <= 16; }

// The seat key for a right hand in grip gripR. The game re-seats for the alt grip only in a two-handed grip (its
// alt-grip switch needs R Two Handed Grip), but sets R Alt Grip in any grip while both attack buttons are held: with a
// sword beside a shield the flag comes and goes and the weapon stays where it is. A stand-in that re-seated on the
// flag alone moved that sword to the alt point, some of them turned over (#4322's logs: 88 times, 8 upside down).
// altAnyGrip: the earlier rule, the alt flag counts in every grip.
// master: Master Stroke Grip as the rig counts it (false with standin_master_seat=0): seat key bit 8.
constexpr uint8_t kSeatKeyMaster = 8;
constexpr uint8_t SeatKeyFor(uint8_t gripR, bool alt, bool mordhau, bool reverse, bool altAnyGrip, bool master = false) {
    return uint8_t(SeatKey(alt && (altAnyGrip || TwoHandedGrip(gripR)), mordhau, reverse) | (master ? kSeatKeyMaster : 0u));
}

// Which re-seat a seat key change is. Only the master stroke bit changed: Event Master Stroke Switch's (MasterSeatFor).
// Any other bit changed too: the alt / mordhau / reverse re-seat (SeatFor), which runs after the switch in the same tick
// (Willie_BP.cpp:25767-25771 come before :25855), so its seat is the one that stays.
constexpr bool MasterSeatChange(uint8_t oldKey, uint8_t newKey) {
    const unsigned diff = unsigned(oldKey ^ newKey);
    return diff == kSeatKeyMaster;
}

}  // namespace hsm::sync

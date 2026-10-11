// One owner's physical state of a fighter in one frame: the stream that drives its stand-ins elsewhere.
// Pure types, no engine headers. World space, Unreal units (uu, cm), degrees, right-handed Z-up as in UE.
//
// Bone list, parents and bodies follow Cyrex0/HalfSword-MP crates/hsmp-pose/src/posecodec_v2.rs
// (Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <array>
#include <cstdint>

namespace hsm::sync {

struct V3 { double x = 0, y = 0, z = 0; };
struct Q4 { double x = 0, y = 0, z = 0, w = 1; };   // unit quaternion (x, y, z, w), FQuat convention

constexpr int kPoseBones = 23;
// Parents first. spine_01 has no physics body (it links pelvis and spine_02); every other bone is a body.
constexpr std::array<const char*, kPoseBones> kPoseBoneNames = {
    "pelvis", "spine_01", "spine_02", "spine_03", "spine_04", "spine_05",
    "neck_01", "neck_02", "head",
    "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",
    "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
    "thigh_l", "calf_l", "foot_l",
    "thigh_r", "calf_r", "foot_r",
};
constexpr std::array<int, kPoseBones> kPoseParent = {0, 0, 1, 2, 3, 4, 5, 6, 7, 5, 9, 10, 11, 5, 13, 14, 15, 0, 17, 18, 0, 20, 21};
constexpr std::array<bool, kPoseBones> kPoseHasBody = {true, false, true, true, true, true, true, true, true, true, true, true,
                                                       true, true, true, true, true, true, true, true, true, true, true};
constexpr int kBonePelvis = 0, kBoneHead = 8, kBoneHandL = 12, kBoneHandR = 16, kBoneFootL = 19, kBoneFootR = 22;
constexpr bool IsLegBone(int b) { return b >= 17; }

struct BoneSample {
    V3 pos;          // bone origin, world
    Q4 rot;          // bone rotation, world
    V3 vel;          // linear velocity of the body at the bone origin, uu/s
    V3 angVelDeg;    // angular velocity, deg/s, world axes
};

constexpr int kPoseWeapons = 2;
struct WeaponSample {
    bool present = false;
    uint8_t hand = 0;        // 0 right, 1 left
    uint32_t classHash = 0;  // weapon class identity (FNV-1a of the class name); 0 unknown
    V3 pos;                  // actor root, world
    Q4 rot;
    V3 vel;                  // root body linear velocity at the root origin, uu/s
    V3 angVelDeg;
    V3 base, tip;            // blade base ("Root Scene") and tip ("TippyTipScene"), world
};

struct PoseFrame {
    uint32_t tsMs = 0;       // owner clock, ms, the physics state's time
    uint16_t stepUs = 0;     // the owner's physics step that produced this state, microseconds
    std::array<BoneSample, kPoseBones> bones{};
    std::array<WeaponSample, kPoseWeapons> weapons{};
};

}  // namespace hsm::sync

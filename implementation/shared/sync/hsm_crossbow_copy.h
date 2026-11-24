// A stand-in's crossbow copy (#3910). The owner's crossbow is a gift of the Spawn (Spawn.flags kSpawnCrossbow), and the
// owner moves it between hands (the string is pulled with the left). Two rules keep every machine's stand-in at exactly one
// crossbow, in the hand the owner's pose shows it in:
//   * GiftBook: the stand-in of a fighter spawned with the gift builds its copy as soon as the pose shows the crossbow
//     class, ignoring loose crossbows lying about (the last life's dropped copy, a copy another machine streamed). Before,
//     the hand waited sync::kSpawnAfterMs for "no copy of the class anywhere", took a stale copy from 600 uu away, or
//     stayed empty while one lay loose anywhere, and the first crossbow replay found "its stand-in holds no crossbow".
//   * MoveHeldCopy: when the class moves to the other hand, the copy the stand-in holds there is moved over. Before, the
//     hand built a second copy, the first was let go and streamed as a dropped weapon (11, then 6 bodies) that the other
//     stand-ins picked up.
// Pure, no engine headers.
#pragma once
#include "sync/hsm_pose_codec.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace hsm::sync {

constexpr uint32_t kCrossbowClass = WeaponClassHash("BP_Weapon_Ranged_Weapon_Crossbow_Light_C");
constexpr double kGiftWindowMs = 20000.0;   // the gift shows within about 3 s of the Spawn; a crossbow in a hand after this was picked up
constexpr uint8_t kGiftFlag = 1;            // protocol Spawn.flags kSpawnCrossbow

class GiftBook {
public:
    static constexpr size_t kEntities = 32;
    // A Spawn was heard (every machine hears it): a crossbow gift is now due for that entity, or no longer is.
    void Note(uint8_t entity, uint8_t spawnFlags, double nowMs) {
        if (entity >= kEntities) return;
        due_[entity] = (spawnFlags & kGiftFlag) != 0;
        atMs_[entity] = nowMs;
    }
    // The stand-in's hand shows poseClass: build the gift's own copy now. Only the crossbow class, only within the window.
    bool Due(uint8_t entity, uint32_t poseClass, double nowMs) const {
        return entity < kEntities && due_[entity] && poseClass == kCrossbowClass && nowMs >= atMs_[entity] &&
               nowMs - atMs_[entity] <= kGiftWindowMs;
    }
    double AgeMs(uint8_t entity, double nowMs) const { return entity < kEntities ? nowMs - atMs_[entity] : 0.0; }
    void Done(uint8_t entity) { if (entity < kEntities) due_[entity] = false; }   // its copy exists: a later one is a pick-up
    void Forget(uint8_t entity) { Done(entity); }
private:
    std::array<bool, kEntities> due_{};
    std::array<double, kEntities> atMs_{};
};

// The pose shows wantedClass in this hand. The other hand of the same stand-in holds a copy of that class of its own making
// (not a synced prop, which its stream carries) and the owner's pose no longer shows the class there: it is the same
// crossbow, moved over. wantedClass 0 (empty) and 1 (unknown) never match.
inline bool MoveHeldCopy(uint32_t wantedClass, bool otherHoldsWeapon, uint32_t otherHeldClass, bool otherIsProp, uint32_t otherPoseClass) {
    return wantedClass > 1 && otherHoldsWeapon && !otherIsProp && otherHeldClass == wantedClass && otherPoseClass != wantedClass;
}

}  // namespace hsm::sync

// Ground clear (lobby setting "Clear ground", and the Duel new-round / rematch clear): which weapon actors lying about are
// "dropped" and go. Pure rule; gameplay/adapter.cpp DestroyDroppedWeapons applies it to every weapon actor.
//
// The game's own "Last Parent" cannot tell a drop from the level's weapons: ModularWeaponBP clears it by itself once the weapon
// is 300 uu from its last holder (a 0.2 s poll) and on a re-grab, so almost every real drop (thrown, kicked, a holder that
// died and respawned) has none. The rule goes by what a weapon is now:
//   held / sheathed   a Parent Actor (Willie_BP "Weapon R/L" and the sheath slots all keep it) -> kept
//   fists, feet       the fighter's body weapons -> kept
//   level start       a weapon the level placed that still lies at its spot (the map reset's record) -> kept
//   anything else     a dropped weapon -> cleared. That includes a level weapon someone picked up and put down elsewhere;
//                     the map reset respawns it on its spot at the next round, and without a map reset it is a drop like any.
#pragma once
#include <cstdint>

namespace hsm {

// How far a level weapon may lie from its recorded spot (uu, 1.5 m) and still count as untouched: settling on a rack or a
// nudge by a passing body is not a drop; a kick or a throw is.
constexpr double kLevelWeaponRestUu = 150.0;

inline bool AtLevelStart(double distanceSquaredUu) {
    return distanceSquaredUu >= 0.0 && distanceSquaredUu <= kLevelWeaponRestUu * kLevelWeaponRestUu;
}

struct DropCandidate {
    bool hasParent = false;       // "Parent Actor" set: in a hand or sheathed on a body
    bool inHand = false;          // with hasParent: the parent's Weapon R / Weapon L is this weapon (else it is sheathed)
    bool bodyWeapon = false;      // fists or feet
    bool levelStart = false;      // a recorded level weapon still at its start spot
    bool beingDestroyed = false;  // already going
};

enum class DropVerdict : uint8_t { Clear, KeepHeld, KeepSheathed, KeepLevel, KeepOther };

inline DropVerdict ClearableDrop(const DropCandidate& c) {
    if (c.bodyWeapon || c.beingDestroyed) return DropVerdict::KeepOther;
    if (c.hasParent) return c.inHand ? DropVerdict::KeepHeld : DropVerdict::KeepSheathed;
    if (c.levelStart) return DropVerdict::KeepLevel;
    return DropVerdict::Clear;
}

}  // namespace hsm

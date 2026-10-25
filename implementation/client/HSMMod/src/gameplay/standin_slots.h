// A stand-in's sheathed weapons (the five "Weapon Slot ..." variables of its Willie_BP), kept with its body.
//
// The game spawns each slot weapon itself (Event Add Sheathed Weapon / Sheathe on Spawn, Willie_BP.cpp:6994): under the
// actor, then at the sheath socket of the Mesh, with a PhysicsConstraint constraining Weapon.BaseMesh to BoneCore (the
// waist slots) or to the Mesh (the back). The stand-in rig then switches BoneCore's collision off and moves the Mesh and
// the held weapons rigidly (standin_rig.cpp NeutraliseFull / Shift), which left the slot weapons standing at the
// stand-in's spawn point (docs/TWO_PLAYER_TEST_6.md section 3).
//
// This class logs what each slot looks like (at the first neutralise pass of a stand-in and again about 1 s and 3 s
// later) so the failing step is on record, and anchors the slots combat::SlotAnchorPlan names (a weapon in the slot with
// a live sheath constraint and in neither hand, the stand-in's own: "Parent Actor"): the waist slots' constraint is
// pointed at the Mesh's pelvis body instead of BoneCore (which has no collision), the weapon's BaseMesh gets the held
// weapons' body policy (gravity off, world geometry ignored), and Shift moves it with the body. A waist weapon more than
// 15 cm from its socket (it fell while BoneCore had no collision, before the game made its constraint) is teleported
// back to the socket first, since a new constraint keeps the offset. A slot whose constraint is dead is logged once and
// left alone. Game thread only.
#pragma once
#include "sync/hsm_pose.h"
#include "ue/engine.h"
#include <cstdint>

namespace hsm::game {

class StandInSlots {
public:
    // Once per full neutralise pass of the stand-in's rig: the diagnosis schedule, then Anchor. A different (or
    // respawned) Willie starts the schedule over.
    void Maintain(ue::UObject* willie);
    // The rig moved the stand-in rigidly by d (FighterRig::Shift): the anchored slot weapons go with it, at rest.
    void Shift(ue::UObject* willie, const sync::V3& d);
    // Anchors the stand-in's slot weapons (see above). Idempotent: a constraint is pointed at the Mesh once. Also after
    // EquipSync::Sheathe puts a weapon in a slot. logLost: a slot whose constraint is dead is logged (once per weapon).
    static void Anchor(ue::UObject* willie, bool logLost);

private:
    ue::UObject* willie_ = nullptr;
    int32_t index_ = -1;
    double firstMs_ = -1.0;
    int logged_ = 0;     // diagnosis lines written for this Willie
};

}  // namespace hsm::game

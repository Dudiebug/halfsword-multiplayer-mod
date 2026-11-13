// A stand-in's sheathed weapons (the five "Weapon Slot ..." variables of its Willie_BP), kept with its body.
//
// The game spawns each slot weapon itself (Event Add Sheathed Weapon / Sheathe on Spawn): under the actor, then at the
// sheath socket of the Mesh, with a PhysicsConstraint constraining Weapon.BaseMesh to the Mesh. Up to build 24185754 the
// waist slots were constrained to BoneCore (the back to the Mesh); since 26568321 Create Sheath Constraint R/L constrains
// every slot to the Mesh at the socket's own bone, with the hang point and Z limit from the weapon length (Willie_BP.cpp:3273,
// :3372, :9850) and BoneCore is gone. The stand-in rig moves the Mesh and the held weapons rigidly (standin_rig.cpp
// NeutraliseFull / Shift), which left the slot weapons standing at the stand-in's spawn point (docs/TWO_PLAYER_TEST_6.md
// section 3) while they hung from something the rig did not move.
//
// This class logs what each slot looks like (at the first neutralise pass of a stand-in and again about 1 s and 3 s
// later) so the failing step is on record, and anchors the slots combat::SlotAnchorPlan names (a weapon in the slot with
// a live sheath constraint and in neither hand, the stand-in's own: "Parent Actors"): the weapon's BaseMesh gets the held
// weapons' body policy (gravity off, world geometry ignored), and Shift moves it with the body. A constraint whose second
// component is not the Mesh already (GetConstrainedComponents; a build that anchors a waist slot to something else) is pointed
// at the Mesh's pelvis body, with the game's hang point and slack put back; one that already holds the Mesh is kept as the
// game made it and logged as such ("sheath constraint already holds the Mesh at bone ..."), once per constraint. A waist
// weapon more than 15 cm from its socket (it fell before the game made its constraint) is teleported back to the socket
// first, since a new constraint keeps the offset. A slot whose constraint is dead is logged once and left alone.
//
// A scabbard weapon (SheathSlot 0) hangs differently (Willie_BP.cpp:7114-7160): the weapon hangs from a physics Scabbard
// by the slot constraint, and the Scabbard from the waist by a joint that is the Scabbard's first child (26568321: already
// Mesh 'pelvis', Willie_BP.cpp:9942; it was BoneCore 'pelvis'). Where that joint is not on the Mesh it is the one pointed
// there; the slot constraint stays as the game made it (it is left at the spot the weapon
// was spawned, 3 m under the pelvis, and retargeting it hung the weapon there: #248). Weapon and Scabbard are put at the
// socket's own transform, as the game does, and Shift moves both.
//
// Watch (every kSlotWatchMs, combat::SlotDrift): an anchored weapon that stays more than combat::kSlotDriftCm from its
// socket is put back, and one more than 100 cm out is logged (the weapon, its visibility and parent), so a weapon that
// is far or hidden shows in a tester's log. Game thread only.
#pragma once
#include "combat/hsm_slotdrift.h"
#include "sync/hsm_pose.h"
#include "ue/engine.h"
#include <cstdint>

namespace hsm::game {

class StandInSlots {
public:
    // Once per full neutralise pass of the stand-in's rig: the diagnosis schedule, then Anchor. A different (or
    // respawned) Willie starts the schedule over.
    void Maintain(ue::UObject* willie);
    // Every frame of the stand-in's rig (cheap: it reads every combat::kSlotWatchMs): the drift watchdog and the far-weapon
    // line described above.
    void Watch(ue::UObject* willie);
    // The rig moved the stand-in rigidly by d (FighterRig::Shift): the anchored slot weapons go with it, at rest.
    void Shift(ue::UObject* willie, const sync::V3& d);
    // Anchors the stand-in's slot weapons (see above). Idempotent: a constraint is pointed at the Mesh once. Also after
    // EquipSync::Sheathe puts a weapon in a slot. logLost: a slot whose constraint is dead is logged (once per weapon).
    static void Anchor(ue::UObject* willie, bool logLost);

private:
    void Track(ue::UObject* willie, double now);   // a different (or respawned) Willie starts over
    ue::UObject* willie_ = nullptr;
    int32_t index_ = -1;
    double firstMs_ = -1.0;
    int logged_ = 0;     // diagnosis lines written for this Willie
    double lastWatchMs_ = -1e18;
    combat::SlotDrift drift_;
};

// scabbard_fix under [net] in HSMMod.ini (config.h): false is 0.8.0-pre18's anchoring (every slot constraint pointed at the
// Mesh, the once-only 15 cm snap, no watchdog); the far-weapon and joint log lines stay. Set once at start.
void SetStandInScabbardFix(bool on);

}  // namespace hsm::game

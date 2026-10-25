// One fighter's physics bodies bound for per-frame reading and driving: the 23 pose bones of its Mesh and the held
// weapons' root bodies, with every per-frame engine call resolved once (ue::PreparedCall). The owner side reads a
// fighter into the pose stream (hsm_pose.h); the stand-in side reads its own bodies for the servo (hsm_standin.h
// FighterNow), applies the servo's velocity commands, moves itself rigidly on a placement, and keeps the Willie's own
// drives switched off (Neutralise). Game thread only.
//
// The stand-in policy follows Cyrex0/HalfSword-MP mods/HSMPAvatars/Scripts/main.lua (neutralise_bp, the grip
// constraints, snap_mesh, drive_root, the weapon-root gravity/collision policy), Copyright (c) 2026 Cyrex0 and the
// HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md). Differences: the stand-in's own Willie ReceiveTick is
// blocked outright (session.cpp), so nothing re-arms what Neutralise switched off between passes; the pelvis-to-capsule
// root constraints are broken instead of having their drive powers zeroed (the powers only reach the constraint through
// the blocked tick); a freed grip is never restored (its template limits live in the native ConstraintInstance, which
// this layer does not read), so a held weapon that stops being driven after its grip was freed is held still instead.
#pragma once
#include "gameplay/standin_slots.h"
#include "sync/hsm_standin.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <vector>

namespace hsm::game {

class FighterRig {
public:
    // Binds the fighter's Mesh and bones (a different or respawned fighter rebinds everything). False when it has no
    // Mesh, a pose bone is missing or a per-frame call does not resolve; the rig is then unbound.
    bool Bind(ue::UObject* willie);
    void Unbind();
    bool Bound() const { return willie_ != nullptr; }
    ue::UObject* Willie() const { return willie_; }
    bool StillValid() const;                       // the bound Willie and its Mesh are still the live objects they were

    // Reads this frame's physics state. frame (optional): every bone with velocities and the held weapons (slot 0 =
    // "Weapon R", hand 0; slot 1 = "Weapon L", hand 1), as the owner streams them. now (optional): the bodies the
    // servo drives (bone transforms, weapon roots, centres of mass, the contact bodies' velocities), slot for slot.
    // False if the rig is not valid.
    bool Read(uint32_t tsMs, uint16_t stepUs, sync::PoseFrame* frame, sync::FighterNow* now);
    // Blade tip of the weapon in slot w as of the last Read (false when that slot was not read or has no tip).
    bool WeaponTip(int w, sync::V3& out) const;

    // Sets every commanded body's linear and angular velocity. Frees a hand's grip constraint once the weapon it holds
    // is driven (or the hand holds fists or nothing), as HSMP does; a held weapon whose grip is free but which is not
    // driven this frame is held still.
    void Drive(const sync::FighterCommand& cmd);
    // Weapon slots (bit w = slot w) another fighter holds with a stuck-blade joint (Severs::Hold): neither driven, held
    // still nor moved by Shift, so only the joint and the struck body move them. Set every frame before Shift and Drive.
    void SetPinnedWeapons(uint32_t slots) { pinned_ = slots; }
    // Moves the whole fighter rigidly so that the Mesh and the held weapon roots read this frame end where they were
    // read plus d (teleported, all velocities zeroed), whatever moved them since the Read. Call after PlaceCapsule.
    void Shift(const sync::V3& d);
    // Keeps the (collision-free) capsule with the body for gameplay queries (HSMP drive_root). Teleports only for jumps
    // over 150 uu, which may carry the Mesh along: on a placement call it before Shift, as HSMP does. yawDeg (optional)
    // also turns the capsule (pitch and roll 0). Unlike HSMP the turn never teleports, so it cannot swing the
    // simulating bodies around the capsule (Shift undoes a carried translation, not a rotation).
    void PlaceCapsule(const sync::V3& at, const double* yawDeg = nullptr);

    // Switches the Willie's own pulls off: tonus, muscles, pain reactions and damage every call (field writes); joint
    // motors, physical animation, physics handles, root constraints, gravity, world collision, BoneCore and capsule
    // collision on a schedule (every 0.5 s for the first 6 s after binding, then every 2 s, and after a weapon change).
    void Neutralise(double nowMs);

    // Stand-in: writes the owner's grip state into this Willie, so its fingers and animation follow the owner's hands (the
    // left hand of a two-handed grip never holds a Weapon L; the game marks it with L Hand In Offhand Attached). The two
    // grip types go through sync::GripToApply: a hand that holds a weapon here never gets 0, which would destroy the
    // weapon. Call every frame after Neutralise and Drive. Logs one line per change.
    void ApplyGrip(const sync::GripState& g);

private:
    sync::GripState ReadGrip() const;             // the bound Willie's own grip variables (the owner's side)
    struct Weapon {
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        ue::UObject* root = nullptr;           // the actor's root component: the simulating body
        int32_t rootIndex = -1;
        ue::UObject* base = nullptr;           // "Root Scene"
        ue::UObject* tip = nullptr;            // "TippyTipScene"
        bool fists = false;
        bool comMeasured = false;
        int comTries = 0;
        uint32_t classHash = 0;
        sync::V3 comLocal{};
        ue::PreparedCall toWorld, simulating, linVelAt, angVel, setLin, setAng, com, baseLoc, tipLoc;
        // this frame
        bool read = false, haveTip = false, driven = false;
        sync::V3 pos{}, basePos{}, tipPos{};
        sync::Q4 rot{};
    };
    ue::UObject* FieldAt(int32_t offset) const;   // UObject* field of the bound Willie at a cached offset
    void RefreshWeapon(int w);                    // re-resolves slot w when its field changed
    bool ReadWeapon(int w, const sync::V3& handPos);
    bool GripWanted(int hand) const;
    bool GripLoose(int hand) const;
    void FreeGrip(int hand);
    void HoldStill(Weapon& wp);
    bool Pinned(int w) const { return (pinned_ >> w) & 1u; }
    void NeutraliseFull();
    void SetBodyPolicy(ue::UObject* body);

    ue::UObject* willie_ = nullptr;
    int32_t willieIndex_ = -1;
    ue::UObject* mesh_ = nullptr;
    int32_t meshIndex_ = -1;
    std::array<ue::FName, sync::kPoseBones> bones_{};
    std::array<sync::V3, sync::kPoseBones> comLocal_{};
    bool comMeasured_ = false;
    int comTries_ = 0;
    ue::PreparedCall socket_, linVelAt_, linVel_, angVel_, setLin_, setAng_, com_;
    ue::PreparedCall placeActor_, actorLoc_, turnActor_;
    std::array<Weapon, sync::kPoseWeapons> weapons_{};
    int32_t weaponOffset_[sync::kPoseWeapons] = {-1, -1};   // "Weapon R" / "Weapon L" in the Willie
    int32_t gripOffset_[2] = {-1, -1};                       // "PhysicsConstraint R Hand" / "L Hand"
    ue::UObject* gripFreed_[2] = {nullptr, nullptr};         // freed per hand since the last full pass (null: free it again)
    ue::UObject* gripLoose_[2] = {nullptr, nullptr};         // freed at all (limits never restored), until a weapon change
    int32_t gripLooseIndex_[2] = {-1, -1};
    ue::UObject* gripLogged_[2] = {nullptr, nullptr};
    std::vector<int32_t> zeroDoubles_;                       // offsets of the zeroed double fields
    std::vector<ue::FProperty*> trueBools_, falseBools_;
    int32_t gripTypeOffset_[2] = {-1, -1};                   // "R_GripType_Current" / "L_GripType_Current" (ByteProperty)
    ue::FProperty* gripFlag_[5] = {};                        // offhand, alt, mordhau, switched, reverse (BoolProperty)
    sync::GripState gripStateLogged_{};                      // the grip state ApplyGrip last logged
    bool gripStateKnown_ = false;
    double boundAtMs_ = -1.0, nextFullMs_ = 0.0;
    bool weaponChanged_ = false;
    uint32_t pinned_ = 0;                                    // SetPinnedWeapons
    std::vector<ue::UObject*> brokenRoots_;                  // root constraints already broken
    bool typesLogged_ = false;
    sync::V3 pelvis_{};                                      // this frame's Read
    bool pelvisRead_ = false;
    ue::UObject* failLogged_ = nullptr;                      // the fighter whose bind failure was logged
    StandInSlots slots_;                                     // the sheathed weapons (standin_slots.h)
};

}  // namespace hsm::game

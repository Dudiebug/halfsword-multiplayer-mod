// Gore on stand-ins in owner sync: the severed parts and dislocated joints a fighter's owner reports in Vitals
// (VitalsMsg::severed / ::dislocated), shown on the stand-in Willie that mirrors it. Stand-ins are neutralised and
// tick-blocked, so none of the game's own gore detection runs on them; the owner's does, natively, and streams the
// result. The decisions (grace period, edge detection, respawn) are sync/hsm_gore.h's GoreTracker, one per entity; this
// class turns them into engine calls and keeps a record of what it did to each actor, so a repeated or reordered
// Vitals never repeats an action. Inert until Start (owner sync only). Game thread only.
//
// A new body. Vitals is unreliable and the old body's samples outlive it: in flight when the respawn notice lands, and
// still streaming through a level change while its owner loads (the feeds stay). Whenever an entity's stand-in is a
// new actor (first attach, a respawn, a new level, a rebuild) what was owed is dropped and the tracker waits for a whole
// sample (GoreTracker::AwaitWhole, at most kAwaitWholeMs after the first gory one), so the old body's gore never lands on
// it; Forget (the respawn notice) arms the same wait before the actor changes. Gore that is still reported after the
// wait (a stand-in rebuilt mid-fight, a join mid-fight) comes back through the tracker as usual, that much later.
//
// Severed parts. The game's cut (Willie_BP "Dismember Function Initiate/Delayed", Willie_BP.cpp:3973-4415,
// 4416+) spawns a separate limb actor at "Spawn Bone" (:3993), vertex-paints the cut on every "Dismember Parent Part"
// (the Mesh and its direct SkeletalMeshComponent children tagged 'Dismember Pass', and for a head or neck cut not those
// also tagged 'Head 2', :4452), hides "Hide Bone Local" (:3994, the bone one step below the cut) on those with
// HideBoneByName(.., PBO_Term) (:4128) and welds the limb back with a constraint. It does nothing for a Skeletron
// (:3975, :4417), so the fallback shows no cut on one either. It needs the striking weapon's live boxes, so the
// primary replays it from the reliable sever claim (NoteNativeSever tells this class it ran). This class is the fallback for a part the owner reports severed
// that no replay covered within kSeverGraceMs: the part's Spawn Bone (combat::SeverSpawnBone) is hidden on the Mesh
// and the same tagged children with PBO_Term (the flag the game uses), and the bodies below it are physics-disabled
// first (SetAllBodiesBelowPhysicsDisabled over the subtree, the exclusion Cyrex0/HalfSword-MP's body_injury.lua uses so
// a severed limb stops colliding and stops being servoed). For the head (parts 0 and 1) the hair is destroyed, as the
// game does for part 0 (Destroy Hair, :3742; for part 1 it moves the hair onto the thrown head, which does not exist
// here), or it would hang in the air. Not mirrored: the thrown limb actor, the vertex-painted
// cut surface and the blood and sound that go with them; the pelvis (part 2) is not hidden at all (hiding it hides the
// body). The look is a whole limb gone below the Spawn Bone, not the game's cut.
//
// Loose joints (dislocated or broken). The game dislocates from the owner's tick, which needs live physical-animation
// strain: Dislocate Bone (Willie_BP.cpp:3750-3905) fills a "<Part> Dislocation State" meter and at 1 sets X Dislocated
// (Back Broken for the spine) and puts the part's physics-asset joint on the 'Broken' constraint profile (:3801;
// shoulders UserConstraint_10/14, knees 0/18, neck 7, spine 4). A hit that breaks a bone (Get Damage, :5079-5161) does
// the same through Break Arm (the elbow, UserConstraint_11/15), Break Leg and Snap Neck. The joint stays whole: it goes
// loose, it does not come apart. The displaced bone is already in the owner's pose stream. What would hurt on the
// stand-in is its own joint still on the 'Motor' profile: it would fight the servo's velocity target for a bone the
// owner's loose joint lets hang. So Dislocate puts the same joint on the same profile, one joint per bit of the owner's
// Vitals (sync/hsm_gore.h). Healing runs the meter down and Restore Bone (:6860-6990) clears the flags and puts the
// profile back to 'Motor'; Relocate does the same on the stand-in (the bit falling) and forgets the joint as freed.
// Only a Skeletron still comes apart (Event Dislocate Arm/Leg/Neck, :15619+, called under `if (Skeletron)` at :12252 and
// on): every live constraint of the bone toward its parent (GetConstraintsFromBody(bone, parents, no children, no
// terminated ones)) is made breakable at threshold 0, linear and angular, and the next physics step breaks it.
// BreakJoint does that for a Skeletron stand-in. BreakConstraint(bone) would not do: the physics asset names its joints
// UserConstraint_N, not after their bones. A joint the engine has broken is terminated and has no call to bring it back.
// Not mirrored: the sound, the noise, health and pain (the Vitals carry the health values a bot reads).
//
// Once parts are gone the servo must not trust their bones: DeadBones(entity) gives the pose bones (sync/hsm_pose.h
// numbering) whose bodies the stand-in no longer has, for the session to clear in the FighterNow before it is used.
//
// The tagged-children hide, the subtree exclusion and the servo omission follow Cyrex0/HalfSword-MP mods/HSMPAvatars/
// Scripts/body_injury.lua, Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md).
#pragma once
#include "gameplay/adapter.h"
#include "sync/hsm_gore.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hsm::game {

class StandInGore {
public:
    struct Env {
        std::function<ue::UObject*(uint8_t)> standInActor;     // an entity's live stand-in Willie, nullptr if none (StandIns::ActorOf)
    };
    void Start(Adapter& adapter, Env env);
    void Stop();                                                // stand-ins die with the session: nothing is undone
    bool Active() const { return active_; }
    // One Vitals sample of a fighter owned elsewhere (any order, any repeat). nowMs: the session's steady clock.
    void OnVitals(uint8_t entity, uint32_t severed, uint32_t dislocated, double nowMs);
    // The game's own cut was replayed on this entity's stand-in for `part`: no fallback for it, and its bones are gone.
    void NoteNativeSever(uint8_t entity, uint8_t part);
    // ... but its Delayed half (limb spawn, bone hide) never ran: the part is owed to the fallback again.
    void NoteNativeLost(uint8_t entity, uint8_t part);
    void Tick(double nowMs);                                    // once per frame, after the stand-ins' Tick
    // Pose bones (bit per sync::kPoseBones index) the entity's current stand-in has lost; 0 when it is whole or has no actor.
    uint32_t DeadBones(uint8_t entity) const;
    void Forget(uint8_t entity);                                // left or respawned: state and record end, the next body waits whole
    void Clear();                                               // level change: actors are gone, nothing touched (Bind waits)
    std::string HealthLine();                                   // since the previous call

private:
    enum class Outcome { Done, Nothing, Failed };
    static constexpr size_t kActions = sync::kGoreSeveredBits + sync::kGoreJoints;   // retry slots: parts, then joints
    struct Record {                                             // what this machine did to one actor
        ue::UObject* willie = nullptr;
        int32_t index = -1;
        sync::GoreRecord did;                                   // parts hidden, cut natively or given up; joints freed or given up
        std::array<uint8_t, kActions> tries{};
        double retryAtMs = 0, checkAtMs = 0;
    };
    struct Entry {
        sync::GoreTracker tracker;
        sync::GoreOwed owed;                                    // fallbacks due and joints dislocated, until the tracker resets
        Record rec;
    };
    struct Counters { uint64_t vitals = 0, resets = 0, fallbacks = 0, nothing = 0, failed = 0, reasserted = 0, native = 0, nativeLost = 0,
                      dislocated = 0, relocated = 0, swaps = 0, actors = 0, headTwoSkipped = 0; };
    // The entity's live stand-in, with its record made current; a new actor drops what was owed and waits for a whole sample.
    ue::UObject* Bind(uint8_t entity, Entry& e);
    void Reconcile(uint8_t entity, Entry& e, double nowMs);
    Outcome ShowSeverFallback(ue::UObject* willie, uint8_t part);
    int32_t Dislocate(ue::UObject* willie, unsigned joint, bool skeletron);
    int32_t BreakJoint(ue::UObject* willie, unsigned joint);
    void Relocate(uint8_t entity, unsigned joint);
    void Reassert(ue::UObject* willie, uint32_t parts);
    bool HideBone(ue::UObject* component, ue::FName bone);
    bool BoneHidden(ue::UObject* mesh, ue::FName bone);
    std::vector<ue::UObject*> PassChildren(ue::UObject* mesh, bool head);  // the Mesh's direct children the game's cut also hides on
    ue::FName NameOf(const std::string& bone);

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    Env env_;
    std::array<Entry, kMaxWillies + 1> entries_{};
    std::map<std::string, ue::FName> names_;
    ue::UObject* constraintLib_ = nullptr;                      // ConstraintInstanceBlueprintLibrary's default object
    Counters n_, reported_;
};

}  // namespace hsm::game

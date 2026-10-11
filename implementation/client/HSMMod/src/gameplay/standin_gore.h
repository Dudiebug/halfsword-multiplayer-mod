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
// Severed parts. The game's cut (Willie_BP "Dismember Function Initiate/Delayed", Willie_BP.cpp:3249-3440,
// 3648+) spawns a separate limb actor at "Spawn Bone" (:3269), vertex-paints the cut on every "Dismember Parent Part"
// (the Mesh and its direct SkeletalMeshComponent children tagged 'Dismember Pass', :3678-3690), hides "Hide Bone
// Local" (:3270, the bone one step below the cut) on those with HideBoneByName(.., PBO_Term) (:3429) and welds the limb
// back with a constraint. It needs the striking weapon's live boxes, so the primary replays it from the reliable sever
// claim (NoteNativeSever tells this class it ran). This class is the fallback for a part the owner reports severed
// that no replay covered within kSeverGraceMs: the part's Spawn Bone (combat::SeverSpawnBone) is hidden on the Mesh
// and the same tagged children with PBO_Term (the flag the game uses), and the bodies below it are physics-disabled
// first (SetAllBodiesBelowPhysicsDisabled over the subtree, the exclusion Cyrex0/HalfSword-MP's body_injury.lua uses so
// a severed limb stops colliding and stops being servoed). For the head (parts 0 and 1) the hair is destroyed, as the
// game does for part 0 (:8504, Destroy Hair; for part 1 it moves the hair onto the thrown head, which does not exist
// here), or it would hang in the air. Not mirrored: the thrown limb actor, the vertex-painted
// cut surface and the blood and sound that go with them; the pelvis (part 2) is not hidden at all (hiding it hides the
// body). The look is a whole limb gone below the Spawn Bone, not the game's cut.
//
// Dislocations. The game sets X Dislocated from a tick-driven event that needs live physical-animation strain
// (Willie_BP.cpp:8875-9460), then plays a sound, makes every constraint of the bone breakable at threshold 0 so its
// original joint snaps, drags the bone out with a temporary sphere and constraint, and after a Delay 0 parks a loose
// constraint (bone to parent: upperarm-clavicle, calf-thigh, neck_02-neck_01, spine_04-spine_03; :12170-12300) with
// health 0 and pain 10; the neck also runs Snap Neck, the spine Break Back. The displaced bone is already in the
// owner's pose stream. What would hurt on the stand-in is its own original joint (linear limit zero, nothing drives
// it any more): it would fight the servo's velocity target for a bone 15 cm or more from where the joint puts it. So
// Dislocate frees that joint the game's own way (Willie_BP.cpp:8998-9012): every live constraint of the bone toward its
// parent (GetConstraintsFromBody(bone, parents, no children, no terminated ones)) is made breakable at threshold 0,
// linear and angular, and the next physics step breaks it under the servo's load. BreakConstraint(bone) would not do:
// the physics asset names its joints UserConstraint_N, not after their bones. Not mirrored: the sound, the noise,
// health and pain (Neutralise zeroes them), the replacement constraint (the servo carries the bone), Snap Neck and
// Break Back. Relocate has nothing to undo: a broken constraint is terminated and the engine has no call to bring it
// back, and the game never clears X Dislocated on a live body either (no assignment of false anywhere in the
// decompile) - a dislocation ends with the fighter's respawn, which is a new stand-in actor.
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
    struct Counters { uint64_t vitals = 0, resets = 0, fallbacks = 0, nothing = 0, failed = 0, reasserted = 0, native = 0,
                      dislocated = 0, relocated = 0, swaps = 0, actors = 0; };
    // The entity's live stand-in, with its record made current; a new actor drops what was owed and waits for a whole sample.
    ue::UObject* Bind(uint8_t entity, Entry& e);
    void Reconcile(uint8_t entity, Entry& e, double nowMs);
    Outcome ShowSeverFallback(ue::UObject* willie, uint8_t part);
    int32_t Dislocate(ue::UObject* willie, unsigned joint);
    void Relocate(uint8_t entity, unsigned joint);
    void Reassert(ue::UObject* willie, uint32_t parts);
    bool HideBone(ue::UObject* component, ue::FName bone);
    bool BoneHidden(ue::UObject* mesh, ue::FName bone);
    std::vector<ue::UObject*> PassChildren(ue::UObject* mesh);  // the Mesh's direct children the game's cut also hides on
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

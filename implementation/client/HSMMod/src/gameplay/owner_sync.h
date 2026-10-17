// Owner authority (protocol rev 9, session/hsm_authority.h). Every machine simulates the fighters it owns natively,
// never corrected, and streams them (OwnerStreams); every fighter owned elsewhere is a neutralised stand-in servoed to
// that stream (StandIns), the network form of the mirror bench (mirror.h). Game thread only.
//
// The stand-in flow follows Cyrex0/HalfSword-MP mods/HSMPAvatars/Scripts/main.lua (drive_root, snap_mesh, the 60 Hz
// pose stream), Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md).
#pragma once
#include "sync/hsm_vitals_feed.h"
#include "gameplay/adapter.h"
#include "gameplay/standin_rig.h"
#include "hsm_owner.h"
#include "sync/hsm_poseplay.h"
#include "sync/hsm_roottrack.h"
#include "sync/hsm_standin.h"
#include "sync/hsm_track.h"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace hsm::game {

double SteadyNowMs();

// The fighters this machine owns: pose at 60 Hz (bots 30 Hz) and vitals at 15 Hz, both unreliable. One clock for
// every pose and every hit claim this machine makes, so a receiver can relate a claim to the poses it has seen.
class OwnerStreams {
public:
    using Send = std::function<void(const std::vector<uint8_t>&)>;
    void Start(double nowMs);                         // a new network session: a new clock
    void Clear();
    bool Started() const { return startMs_ > 0.0; }
    uint32_t ClockMs(double nowMs) const;             // this machine's stream clock (also every claim's attackerTsMs)
    // Once per frame: reads and sends every owned fighter that is due. owned: (entity, its live Willie).
    void Tick(Adapter& adapter, double nowMs, float dt, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned, const Send& send);
    bool PelvisOf(uint8_t entity, sync::V3& out) const;   // as of the latest read (the snap clearance)
    uint64_t PosesSent() const { return posesSent_; }
    uint64_t VitalsSent() const { return vitalsSent_; }

private:
    struct Owned {
        FighterRig rig;
        ue::UObject* willie = nullptr;
        double nextPoseMs = 0, nextVitalsMs = 0;
        sync::V3 pelvis{};
        bool havePelvis = false, failLogged = false;
    };
    std::array<std::unique_ptr<Owned>, kMaxWillies + 1> owned_{};
    double startMs_ = 0, prevDtS_ = 0;
    uint64_t posesSent_ = 0, vitalsSent_ = 0, readFailed_ = 0;
};

// Fighters owned elsewhere. The feed (jitter buffer, root track, vitals) is kept per entity and survives the actor being
// replaced (respawn); the actor binding (rig, servo) is per stand-in actor.
class StandIns {
public:
    void OnPose(const PoseMsg& m, const sync::PoseFrame& frame, double nowMs);
    bool OnVitals(const VitalsMsg& v);              // false for stale/previous-body packets
    void OnDeath(uint8_t entity);                  // reliable host-confirmed death, latched until body replacement
    void Attach(uint8_t entity, ue::UObject* willie);   // the session spawned willie as entity's stand-in
    void Detach(uint8_t entity);                        // unbinds; the session destroys the actor
    void Forget(uint8_t entity);                        // the entity left: its feed goes too
    void Clear();

    bool IsStandIn(const ue::UObject* o) const;         // the Willie of a bound stand-in
    uint8_t EntityOf(const ue::UObject* willie) const;  // 0 if none
    ue::UObject* ActorOf(uint8_t entity) const;
    const VitalsMsg* Vitals(uint8_t entity) const;      // the owner's latest vitals, nullptr before any
    // The weapon class (sync::WeaponSample::classHash) the owner's newest pose shows in hand (0 right, 1 left), 0 when
    // that hand is empty. False before any pose, or when the hand holds a weapon of unknown class (prop_sync.cpp).
    bool PoseHand(uint8_t entity, int hand, uint32_t& classHash) const;
    // The owner's clock now (local time minus the minimum transit offset seen): how old a claim from that owner is.
    bool OwnerClockNow(uint8_t entity, double nowMs, int64_t& out) const;
    // The owner time the stand-in currently shows (its last sample), for the parry window.
    bool ShownOwnerTs(uint8_t entity, uint32_t& out) const;

    // Pose bones (bit per sync::kPoseBones index) an entity's stand-in has no bodies for any more (gore): never driven.
    void SetDeadBones(std::function<uint32_t(uint8_t)> deadBones) { deadBones_ = std::move(deadBones); }
    // Weapon slots (bit 0 "Weapon R", bit 1 "Weapon L") of an entity's stand-in that a stuck-blade joint holds to a local
    // fighter: the servo leaves them to the joint (FighterRig::SetPinnedWeapons).
    void SetHeldWeapons(std::function<uint32_t(uint8_t)> heldWeapons) { heldWeapons_ = std::move(heldWeapons); }
    // Once per frame, before the physics step. avoid: local pelvises a stand-in must never be snapped onto.
    void Tick(Adapter& adapter, double nowMs, float dt, const std::vector<sync::V3>& avoid);
    std::string HealthLine();                            // since the previous call; empty with no stand-in
    int Count() const;

private:
    struct Feed {
        sync::PosePlayer player;
        sync::RootTrack root;
        sync::VitalsFeed vitals;
        std::array<uint32_t, sync::kPoseWeapons> handClass{};   // newest pose's weapon class per hand (0 = empty, 1 = unknown)
        uint32_t handTs = 0;
        bool haveHands = false;
        uint64_t poses = 0, rejected = 0;
    };
    struct Actor {
        ue::UObject* willie = nullptr;
        int32_t index = -1;
        FighterRig rig;
        sync::StandInDriver driver;
        double bindDeadlineMs = 0, nextBindMs = 0;
        bool bindLogged = false, haveTarget = false;
        sync::V3 lastTargetPelvis{};
        uint32_t shownTs = 0;
        bool haveShown = false;
        bool vitalsLogged = false, deathLogged = false;
        uint32_t vitalsAppliedTs = 0;
    };
    struct Window {
        sync::ErrorStats pelvis, hands;
        uint64_t frames = 0, targeted = 0, extrapolated = 0, stale = 0, cuts = 0, snaps = 0, placed = 0, struck = 0;
        double delaySum = 0, j95Max = 0;
    };
    struct Slot {
        std::unique_ptr<Feed> feed;
        std::unique_ptr<Actor> actor;
        Window window;
    };
    void TickOne(Adapter& adapter, uint8_t entity, Slot& s, double nowMs, float dt, const std::vector<sync::V3>& avoid);
    void Record(Slot& s, const sync::DriverStep& step, const sync::FighterNow& now, const sync::PoseFrame& target);
    std::array<Slot, kMaxWillies + 1> slots_{};
    std::function<uint32_t(uint8_t)> deadBones_;
    std::function<uint32_t(uint8_t)> heldWeapons_;
    double costSumMs_ = 0, costMaxMs_ = 0;
    uint64_t ticks_ = 0;
};

}  // namespace hsm::game

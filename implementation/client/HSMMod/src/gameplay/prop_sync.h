// Dropped, thrown and disarmed weapons in owner sync (protocol rev 9, Phase 4b "props"). Every weapon a fighter
// simulated here lets go of becomes a prop with an id this machine mints (session/hsm_lease.h, MakeDropPropId); the
// copy every other machine has of that weapon is still in the hand of its stand-in of that fighter, so on hearing the
// drop's Touch Grant each of them makes its stand-in let go of it (the game's own "Set Up ... Hand Weapon" drop) once
// the owner's pose shows that hand empty. From then on one stream drives the prop everywhere: its lessee's (TOUCH:
// whoever touched or threw it last, HOLD: whoever holds it), or the host's while it is FREE (session/hsm_lease.h,
// LeaseTable, arbitrated by the host). Everyone else teleports its copy to that stream (world_sync's ApplyWorldObject).
// A weapon a fighter holds and has never let go of is carried by its Pose and never enters the lease table; a prop
// picked up again stays a prop (HOLD), streamed by the picker, never attached to the picker's stand-in elsewhere.
// Level props are not synced: no identity was found that every machine computes the same (see prop_sync.cpp).
// Game thread only; inert until Start; owner sync only (host sync keeps world_sync's WorldState untouched).
#pragma once
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "session/hsm_lease.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hsm::game {

class PropSync {
public:
    struct Env {
        bool host = false;
        std::function<uint8_t()> mySlot;                              // this machine's roster slot (kHostSlot on the host)
        std::function<uint8_t(const ue::UObject*)> ownedEntity;      // the entity of a fighter simulated here, 0 if none
        // Host: broadcast to every client. Client: to the host. Grants and lease requests go reliable, states not.
        std::function<void(const std::vector<uint8_t>&, bool reliable)> send;
    };
    void Start(Adapter& adapter, StandIns& standIns, Env env);
    void Stop();                                                     // everything, the drop sequences too
    void Clear();                                                    // a level change: every actor is gone
    bool Active() const { return active_; }

    // Once per frame, after StandIns::Tick. owned: (entity, its live Willie) for every fighter simulated here.
    void Tick(double nowMs, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned);
    // A hit event between two actors (the session's hit hook): a fighter simulated here, or a weapon one holds,
    // touching a known prop. Cheap when neither actor is a prop.
    void NoteContact(ue::UObject* a, ue::UObject* b);

    // Host: a PropState from the machine in fromSlot. True when it came from the prop's granted streamer and is newer:
    // relay it (RelayTargets). Client: a relayed PropState; the return value is unused.
    bool OnPropState(const uint8_t* d, size_t n, uint8_t fromSlot);
    // Host: a PropLease Request/Release from the machine in fromSlot. Client: the host's Grant.
    void OnPropLease(const uint8_t* d, size_t n, uint8_t fromSlot);
    void SlotLeft(uint8_t slot);                                     // host: every lease the slot held goes FREE
    std::vector<std::vector<uint8_t>> GrantsForJoiner() const;       // host: every current lease, as Grants
    void ForgetEntity(uint8_t entity);                               // the entity's fighter is gone (left, respawned, removed)

    std::string HealthLine();                                        // since the previous call

private:
    struct RootCalls {
        ue::PreparedCall toWorld, linVel, angVel, setAng, simulating;
    };
    struct Prop {
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        ue::UObject* root = nullptr;
        int32_t rootIndex = -1;
        LeaseClaim claim;
        double lastContactMs = -1e18;
        double lastSentMs = -1e18;
        int restoreTicks = 0;              // physics restore re-asserted after a stand-in let go of it
        bool dirty = false;                // a fresh state from the streamer was applied to the view since the last drive
        bool denied = false;               // held here while another machine holds it
        uint64_t readTick = 0;             // the tick `read` was taken in
        bool readOk = false;
        PropStateMsg read;
    };
    struct Hands {                         // what an owned fighter held as of the previous tick
        ue::UObject* willie = nullptr;
        std::array<ue::UObject*, 2> weapon{};
        std::array<int32_t, 2> index{{-1, -1}};
    };
    struct Counters {
        uint64_t drops = 0, minted = 0, mintFailed = 0, resolved = 0, unresolved = 0, releaseSkipped = 0, releaseFailed = 0,
                 requests = 0, releases = 0, grants = 0, lapsed = 0, statesSent = 0, statesApplied = 0, statesStale = 0,
                 statesRejected = 0, leasesRejected = 0, driven = 0, holdDenied = 0, contacts = 0, readFailed = 0, lost = 0;
    };

    uint8_t MySlot() const { return env_.mySlot ? env_.mySlot() : kHostSlot; }
    void DetectDrops(double nowMs, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned);
    void ResolvePending(double nowMs);
    bool ReleaseFromStandIn(uint64_t propId, ue::UObject* standIn, int hand, ue::UObject* copy);
    Prop* Track(uint64_t propId, ue::UObject* actor);
    void Claim(uint64_t propId, ClaimSend what, LeaseLevel level, double nowMs);
    void HostRequest(uint64_t propId, uint8_t slot, LeaseLevel level, double nowMs);
    void HostRelease(uint64_t propId, uint8_t slot);
    void Grant(uint64_t propId, uint8_t slot, LeaseLevel level);     // host: broadcast it and apply it here
    void OnGrant(uint64_t propId, uint8_t slot, LeaseLevel level);   // every machine: the view, and a drop to resolve
    bool Read(Prop& p, PropStateMsg& out);
    void Drive(Prop& p, const PropStateMsg& s);
    void RestorePhysics(Prop& p);
    RootCalls* CallsFor(ue::UObject* root);
    ue::UObject* ObjectField(ue::UObject* o, int which);             // 0 "Parent Actor", 1 "Weapon R", 2 "Weapon L"
    bool InUse(uint64_t propId, double nowMs) const;

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    Env env_;
    uint64_t tick_ = 0;
    LeaseTable table_;                                               // host only
    PropStateView view_;
    std::map<uint64_t, Prop> props_;                                 // every prop with an actor here
    std::unordered_map<const ue::UObject*, uint64_t> byActor_;
    std::map<uint64_t, double> pending_;                             // drops heard of, not found yet -> give up at
    std::unordered_map<uint64_t, double> heardMs_;                   // last PropState heard per id (minting skips live ids)
    std::array<Hands, kMaxWillies + 1> hands_{};
    std::array<uint8_t, kMaxWillies + 1> dropSeq_{};
    std::array<uint32_t, kMaxWillies + 1> dropEpoch_{};                // seeded per process at Start (MakeDropPropId)
    std::map<const ue::UClass*, std::unique_ptr<RootCalls>> calls_;
    std::array<std::unordered_map<const ue::UClass*, int32_t>, 3> fieldOff_{};   // ObjectField: offset per class, -1 none
    Counters n_, reported_;
};

}  // namespace hsm::game

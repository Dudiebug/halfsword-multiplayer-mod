// Dropped, thrown and disarmed weapons in owner sync (protocol rev 9, Phase 4b "props"). Every weapon a fighter
// simulated here lets go of becomes a prop with an id this machine mints (session/hsm_lease.h, MakeDropPropId); the
// copy every other machine has of that weapon is still in the hand of its stand-in of that fighter, so on hearing the
// drop's Touch Grant each of them makes its stand-in let go of it (the game's own "Set Up ... Hand Weapon" drop) once
// the owner's pose shows that hand empty. From then on one stream drives the prop everywhere: its lessee's (TOUCH:
// whoever touched or threw it last, HOLD: whoever holds it), or the host's while it is FREE (session/hsm_lease.h,
// LeaseTable, arbitrated by the host). Everyone else teleports its copy to that stream (world_sync's ApplyWorldObject).
// A weapon a fighter holds and has never let go of is carried by its Pose and never enters the lease table; a prop
// picked up again stays a prop (HOLD), streamed by the picker.
// Stand-in hands follow the owner's pose (sync/hsm_handmatch.h): a weapon the owner picked up (a prop, or one lying in
// the level) is taken into the stand-in's hand here too ("Set Up ... Hand Weapon" with it), and a weapon the stand-in
// still holds that the owner no longer does is let go. A weapon that leaves a stand-in's hand by itself is taken back
// while the pose still shows it there, and otherwise falls as a loose weapon would; one this machine let go of before
// the owner's drop was heard of becomes that drop's prop when it is.
// Level props (an actor that is no fighter and no weapon, attached to nothing, whose root simulates physics: barrels,
// stools, debris) are named by the host's census (session/hsm_prop_census.h): the host gives each an index and sends
// the list; a client finds its own actor for each index (same class, nearest the host's first-seen place). From then
// on a level prop is leased like a dropped weapon (FREE: the host streams it; TOUCH: whoever's fighter touched it),
// never HOLD. In the first census of a level a client destroys its own props tagged 'Random Delete' that matched
// nothing: the host's level manager deleted those, so both sides keep the host's set.
// Game thread only; inert until Start; owner sync only (host sync keeps world_sync's WorldState untouched).
#pragma once
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "session/hsm_lease.h"
#include "session/hsm_prop_census.h"
#include "sync/hsm_handmatch.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
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
        // Stand-in hand matching: the owner's Equip token for a hand (EquipSync::OwnerToken), whether the pose showing a
        // class in a hand may be a draw still on its way (EquipSync::MayBeDraw) or the pose no longer showing the held
        // weapon a sheathe (EquipSync::MayBeSheathe), and the pose bones a stand-in has lost (StandInGore::DeadBones).
        // Without all four no stand-in hand is matched.
        std::function<bool(uint8_t entity, int hand, uint8_t& token)> equipToken;
        std::function<bool(uint8_t entity, uint32_t classHash)> mayBeDraw;
        std::function<bool(uint8_t entity, int hand)> mayBeSheathe;
        std::function<uint32_t(uint8_t entity)> deadBones;
        // This code changed an entity's stand-in's hands (a drop let go of, a pickup, a leftover let go of, a weapon that
        // left by itself): Equip plans that stand-in again (EquipSync::Replan), as a draw it skipped for a full hand can
        // run now. Optional.
        std::function<void(uint8_t entity)> handsChanged;
        // Whether this machine is in a fight level, whose physics props the census covers (a client ticks props only
        // once its level gate is open). Without it no level prop is synced.
        std::function<bool()> fightLevel;
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
    // Level props. Host: the current census as PropCensus parts, for a machine that has just loaded the level (send them
    // before GrantsForJoiner: a client refuses a level prop's Grant whose index its census lacks); none before the first
    // walk. Client: one part of the host's census, matched once the census is complete; false when it is refused.
    std::vector<std::vector<uint8_t>> CensusForJoiner() const;
    bool OnPropCensus(const uint8_t* d, size_t n);

    // A prop's contact with a fighter (game::Claims; combat::DecidePropContact decides it): what struck, as seen here.
    struct Touch {
        uint64_t id = 0;
        bool streamedHere = false;         // this machine streams it now (its lessee, or the host while it is FREE)
        uint8_t toucher = 0;               // the fighter simulated here that held or touched it last, 0 if none
        sync::V3 velocity;                 // cm/s, read here when streamed here (zero otherwise or when unread)
    };
    // False for an actor that is no prop here, and for a prop a hand holds (a blow with it is its holder's).
    bool PropOf(ue::UObject* actor, Touch& out);
    ue::UObject* ActorOf(uint64_t propId) const;                     // a prop's live actor here, nullptr if none

    std::string HealthLine();                                        // since the previous call

private:
    struct RootCalls {
        ue::PreparedCall toWorld, linVel, angVel, setAng, setLin, simulating;
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
        bool level = false;                // a level prop (MakeLevelPropId), not a dropped weapon
        bool moving = false;               // its last read was not at rest (level props: streamed before those at rest)
        uint8_t toucher = 0;               // the fighter simulated here that held or touched it last (Touch)
        uint64_t readTick = 0;             // the tick `read` was taken in
        bool readOk = false;
        PropStateMsg read;
    };
    struct Hands {                         // what an owned fighter held as of the previous tick
        ue::UObject* willie = nullptr;
        std::array<ue::UObject*, 2> weapon{};
        std::array<int32_t, 2> index{{-1, -1}};
    };
    struct StandInHand {                   // one hand of a stand-in, as the matcher last left it
        sync::HandMatch match;
        ue::UObject* held = nullptr;       // the real weapon in it (not fists, for the left not the right's again)
        int32_t heldIndex = -1;
        uint32_t heldClass = 0;
        double episodeMs = -1;             // the pose's since-time this hand's attach retries belong to
        double retryMs = 0, backoffMs = 0;
        double reattachMs = -1e18;         // the last time a weapon that left by itself was taken back
        bool selfDropLogged = false;       // its first loss since the stand-in was bound (left by itself, or gone) is logged
        // A lease change from the owner let this hand go (NoticeDrop) while the owner's pose still showed that hand in the
        // episode that began at leaseLetGoPoseMs: until the pose moves on the hand takes nothing (it would take the weapon
        // just dropped).
        bool leaseLetGo = false;
        double leaseLetGoPoseMs = 0;
    };
    struct StandInHands {
        ue::UObject* willie = nullptr;
        int32_t index = -1;
        double boundMs = 0;                // when this stand-in was bound (a respawn binds a new one)
        std::array<StandInHand, 2> hand{};
    };
    struct PendingDrop {                   // a drop heard of (its Touch Grant), not resolved yet
        double heardMs = 0;
        double deadlineMs = 0;
        ue::UObject* witness = nullptr;    // the weapon that hand of the stand-in held when it was heard (sync::DropFacts)
        int32_t witnessIndex = -1;
    };
    struct Loose {                         // a weapon a stand-in let go of here that is not a prop (yet)
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        ue::UObject* root = nullptr;
        int32_t rootIndex = -1;
        uint8_t entity = 0;                // the stand-in's, 0 once that fighter is gone
        int hand = 0;
        uint32_t classHash = 0;
        double atMs = 0;
        int restoreTicks = 0;
    };
    struct Candidate {                     // a weapon actor the attach may take (WeaponActors, refreshed now and then)
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        uint32_t classHash = 0;
    };
    enum class DropResult : uint8_t { Dropped, Skipped, Failed };
    struct LevelActor {                    // a physics prop of the level as the census walk found it here
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        uint32_t classHash = 0;
        ue::FVector at;
    };
    using LevelKey = std::pair<const ue::UObject*, int32_t>;        // a level actor and its object-array slot
    struct Counters {
        uint64_t drops = 0, minted = 0, mintFailed = 0, resolved = 0, forced = 0, kept = 0, unresolved = 0, releaseSkipped = 0, releaseFailed = 0,
                 requests = 0, releases = 0, grants = 0, lapsed = 0, statesSent = 0, statesApplied = 0, statesStale = 0,
                 statesRejected = 0, leasesRejected = 0, driven = 0, holdDenied = 0, contacts = 0, islands = 0, readFailed = 0, lost = 0,
                 attached = 0, attachMiss = 0, matchReleased = 0, selfDrops = 0, reattached = 0, leaseReleased = 0, adopted = 0,
                 spawned = 0, spawnFailed = 0,
                 censuses = 0, censusRejected = 0, levelStreamed = 0, levelDriven = 0, levelDeferred = 0, censusPurged = 0;
    };

    uint8_t MySlot() const { return env_.mySlot ? env_.mySlot() : kHostSlot; }
    void DetectDrops(double nowMs, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned);
    void ResolvePending(double nowMs);
    void DeadlineRelease(uint64_t id, uint8_t owner, int hand, ue::UObject* standIn, ue::UObject* copy);
    void GiveUp(uint64_t id, uint8_t owner, int hand, const PendingDrop& d, ue::UObject* copy, double nowMs);
    ue::UObject* UntrackedInHand(ue::UObject* standIn, int hand);   // a real weapon in that hand that no prop names
    bool ReleaseFromStandIn(uint64_t propId, ue::UObject* standIn, int hand, ue::UObject* copy);
    DropResult DropFromHands(ue::UObject* standIn, int hand, ue::UObject* copy, const std::string& what);
    void MatchStandIns(double nowMs);
    void MatchHand(uint8_t entity, ue::UObject* standIn, int hand, std::array<ue::UObject*, 2>& cur, double nowMs);
    void SelfDropped(uint8_t entity, ue::UObject* standIn, int hand, StandInHand& sh, std::array<ue::UObject*, 2>& cur, double nowMs);
    bool Attach(uint8_t entity, ue::UObject* standIn, int hand, const StandIns::PoseHandView& pose, double nowMs);
    bool TakeIntoHand(ue::UObject* standIn, int hand, ue::UObject* weapon);
    bool SpawnIntoHand(uint8_t entity, ue::UObject* standIn, int hand, uint32_t classHash);   // no copy here: one of that class
    ue::UClass* WeaponClassByHash(uint32_t classHash, double nowMs);   // a loaded weapon class by sync::WeaponClassHash
    void LetLoose(uint8_t entity, int hand, ue::UObject* weapon, double nowMs);   // falls here: a prop again, or loose_
    void TickLoose(double nowMs);
    bool Adopt(uint64_t propId, uint8_t owner, int hand, const PendingDrop& d, double nowMs);
    void SetHeld(StandInHand& sh, ue::UObject* weapon);
    void ForgetHeld(uint8_t entity, ue::UObject* standIn);           // this code changed its hands: start from what they hold
    std::array<ue::UObject*, 2> HandsOf(ue::UObject* standIn);   // real weapons only, the right's not again on the left
    uint32_t ClassHashOf(ue::UObject* actor);
    Prop* Track(uint64_t propId, ue::UObject* actor);
    void Claim(uint64_t propId, ClaimSend what, LeaseLevel level, double nowMs);
    void HostRequest(uint64_t propId, uint8_t slot, LeaseLevel level, double nowMs);
    void HostRelease(uint64_t propId, uint8_t slot);
    void Grant(uint64_t propId, uint8_t slot, LeaseLevel level);     // host: broadcast it and apply it here
    void OnGrant(uint64_t propId, uint8_t slot, LeaseLevel level);   // every machine: the view, and a drop to resolve
    // The owner's lease on a prop a stand-in holds here changed (sync DropNoticed): the stand-in lets the copy go at once.
    void NoticeDrop(uint64_t propId, LeaseLevel prevLevel, uint8_t prevSlot, LeaseLevel level, uint8_t slot);
    bool Read(Prop& p, PropStateMsg& out);
    void StreamState(uint64_t propId, Prop& p, LeaseLevel level, uint8_t me, double nowMs);   // read, and send when due
    void StreamLevel(const std::vector<uint64_t>& due, uint64_t& cursor, size_t& budget, bool countDeferred, uint8_t me, double nowMs);
    bool Drive(Prop& p, const PropStateMsg& s);
    // Level props (session/hsm_prop_census.h).
    std::vector<LevelActor> WalkLevel(size_t& notSimulating);       // every physics prop of the level here
    void HostCensus(double nowMs);                                   // walk, index, track, and send when it changed
    void ClientCensus(double nowMs);                                 // bind the census's indices to actors here
    bool InCensus(uint64_t propId) const;                            // a level id of the current census
    bool TaggedRandomDelete(ue::UObject* actor);
    void RestorePhysics(ue::UObject* root, int32_t rootIndex);
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
    std::map<uint64_t, PendingDrop> pending_;                        // drops heard of, not found yet
    // Drops given up as unresolved -> when: a later Grant of the id does not open another window (test 5: one came back
    // with every Grant and fired its deadline 18 s after the drop); forgotten after kLooseKeepMs.
    std::unordered_map<uint64_t, double> givenUp_;
    std::unordered_map<uint64_t, double> heardMs_;                   // last PropState heard per id (minting skips live ids)
    std::array<Hands, kMaxWillies + 1> hands_{};
    std::array<uint8_t, kMaxWillies + 1> dropSeq_{};
    std::array<uint32_t, kMaxWillies + 1> dropEpoch_{};                // seeded per process at Start (MakeDropPropId)
    std::array<StandInHands, kMaxWillies + 1> standInHands_{};
    std::vector<Loose> loose_;
    std::vector<Candidate> weapons_;
    double weaponsMs_ = -1e18;                                       // when weapons_ was last refreshed
    std::unordered_map<uint32_t, std::pair<ue::UClass*, int32_t>> weaponClasses_;   // class hash -> loaded class, its slot
    double weaponClassesMs_ = -1e18;                                 // when weaponClasses_ was last walked
    std::map<const ue::UClass*, std::unique_ptr<RootCalls>> calls_;
    std::array<std::unordered_map<const ue::UClass*, int32_t>, 3> fieldOff_{};   // ObjectField: offset per class, -1 none
    // Level props. levelSinceMs_: when this level was first ticked in a fight level (-1: not yet); censusMs_: the next walk
    // (host) or match retry (client); censusIndex_: the indices of the current census, on both sides.
    double levelSinceMs_ = -1;
    double censusMs_ = 0;
    std::set<uint16_t> censusIndex_;
    uint64_t movingCursor_ = 0, idleCursor_ = 0;                     // StreamLevel's round robin
    // Host: the book that names its props, the census as sent, and its sequence (kept across levels: a client may hold
    // the previous level's census until its own level change clears it).
    CensusBook<LevelKey> book_;
    std::vector<CensusEntry> sent_;
    uint16_t censusSeq_ = 0;
    bool haveSent_ = false;
    // Client: the host's census, the seq it was last matched for, each bound index's entry as it was matched (a changed
    // entry is matched again), the misses of the last match, and whether this level's 'Random Delete' purge ran.
    CensusAssembler census_;
    bool matched_ = false;
    uint16_t matchedSeq_ = 0;
    std::map<uint16_t, CensusEntry> bound_;
    size_t misses_ = 0;
    bool purged_ = false;
    ue::FName randomDelete_{};
    bool randomDeleteKnown_ = false;
    std::unordered_map<const ue::UClass*, int32_t> rootOff_, mobilityOff_;   // WalkLevel: "RootComponent", "Mobility"
    std::unordered_map<const ue::UClass*, uint8_t> classKind_;      // WalkLevel: 0 not a candidate, 1 a candidate actor class
    Counters n_, reported_;
};

}  // namespace hsm::game

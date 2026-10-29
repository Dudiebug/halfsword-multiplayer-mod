// Objects in owner sync (protocol rev 26, plan M0): dropped weapons, level props and every other physics object that appears
// mid-match (a helmet knocked off, a snapped plank, a thrown barrel). An id names an OBJECT (session/hsm_object.h); each
// object has bodies (the actor's movable primitive components, the root first, BodyIndex) and one BodyState per simulating
// body travels from the object's streamer: its lessee (TOUCH: whoever's fighter touched it last, HOLD: whoever holds a
// weapon) or the host while it is FREE (session/hsm_lease.h, LeaseTable, arbitrated by the host). Everyone else drives the
// body to that stream (ApplyPropPose for a root, a component transform for the other bodies).
// Three id kinds: a weapon a fighter let go of (MakeDropPropId; the copy is found in a stand-in's hand, weapon_hands.cpp), a
// level prop named by the host's first census (session/hsm_prop_census.h), and a spawned object (MakeSpawnedId): anything
// the periodic re-check finds that nobody names yet. The host announces its own (ObjectAnnounce, canonical); a client
// proposes with a provisional id and streams nothing under it until the host's canonical announce re-keys it
// (ObjectReconciler); every other machine matches its own actor to a canonical announce by class and place, keeping a
// ghost (GhostTable) until it can. Events (ObjectEvent: a joint broke, a body detached or attached, an object destroyed) are
// polled on the machine that streams the object and replayed idempotently everywhere else (object_events.cpp).
// Stand-in hands follow the owner's pose (sync/hsm_handmatch.h, weapon_hands.cpp). Game thread only; inert until Start.
#pragma once
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "gameplay/shatter_sync.h"
#include "gameplay/weapon_hands.h"
#include "session/hsm_lease.h"
#include "session/hsm_map_reset.h"
#include "session/hsm_object.h"
#include "session/hsm_object_attach.h"
#include "session/hsm_object_event.h"
#include "session/hsm_object_skip.h"
#include "session/hsm_prop_census.h"
#include "session/hsm_walk_cache.h"
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

class ObjectSync {
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
    void SetIdleDespawn(bool on);                                    // dropped weapons go after a minute FREE (bot formats)
    void Clear();                                                    // a level change: every actor is gone
    bool Active() const { return active_; }

    // Once per frame, after StandIns::Tick. owned: (entity, its live Willie) for every fighter simulated here.
    void Tick(double nowMs, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned);
    // A hit event between two actors (the session's hit hook): a fighter simulated here, or a weapon one holds,
    // touching a known prop. Cheap when neither actor is a prop.
    void NoteContact(ue::UObject* a, ue::UObject* b);

    // Host: a BodyState from the machine in fromSlot. True when it came from the object's granted streamer and is newer:
    // relay it (RelayTargets). Client: a relayed BodyState; the return value is unused.
    bool OnBodyState(const uint8_t* d, size_t n, uint8_t fromSlot);
    // Host: a PropLease Request/Release from the machine in fromSlot. Client: the host's Grant.
    void OnPropLease(const uint8_t* d, size_t n, uint8_t fromSlot);
    // Host: a client's proposal (answered with the canonical announce, sent to everyone). Client: the host's canonical announce.
    void OnObjectAnnounce(const uint8_t* d, size_t n, uint8_t fromSlot);
    // Host: an ObjectEvent from fromSlot; true when it is valid and to be relayed. Client: replay it (idempotent).
    bool OnObjectEvent(const uint8_t* d, size_t n, uint8_t fromSlot);
    // Something spawned that should be looked for now (armour knocked off): the re-check is brought forward.
    void NoteSpawn();
    // Host: every announced object as canonical announces, then the event history, for a machine that has just joined.
    std::vector<std::vector<uint8_t>> ObjectsForJoiner() const;
    // Host migration (0.8.0-pre2). ExportKnown: every spawned object this machine knows by its canonical id, as announces (the
    // host's reconciler table; a client's bound objects and announces waiting for an actor). The machine that becomes the host
    // gives them to AdoptKnown after Start: its reconciler is seeded with them (so a late proposal matches an adopted id and
    // new ids never repeat one), they are announced again to everyone (AnnounceAll), and the host's census binds its own
    // actors to them by class and place instead of naming those objects anew.
    std::vector<ObjectAnnounceMsg> ExportKnown() const;
    void AdoptKnown(const std::vector<ObjectAnnounceMsg>& known);
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

    // Map reset (map_reset_sync.cpp, session/hsm_map_reset.h). MarkBaseline: the match has started in this level (every
    // frame it runs is fine; the first counts): level weapons are recorded, and objects spawned after this are the round's.
    // MapReset: put this machine's copy of the map back as it began, a few items a frame (StepReset, from Tick).
    void MarkBaseline();
    void MapReset(double nowMs, uint8_t flags);

private:
    struct RootCalls {
        ue::PreparedCall toWorld, linVel, angVel, setAng, setLin, simulating;
    };
    struct BodyRef {                       // one body of an object: a movable primitive component of its actor
        ue::UObject* comp = nullptr;
        int32_t index = -1;
        bool dirty = false;                // a fresh state from the streamer was applied to the view since the last drive
        bool detached = false;             // left the object (a Detached event): it is its own object now
        uint64_t readTick = 0;             // the tick `read` was taken in
        bool readOk = false;
        BodyStateMsg read;
    };
    struct ConstraintRef { ue::UObject* comp = nullptr; int32_t index = -1; };
    struct StartBody { bool ok = false; BodyStateMsg s; };       // a body's transform when the level began
    struct LevelWeapon {                   // a weapon the level placed, as the match began
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        uint32_t classHash = 0;
        ue::FVector at;
        ue::FRotator rot;
    };
    struct ResetItem { ResetAction action = ResetAction::Leave; uint64_t id = 0; size_t weapon = 0; bool isWeapon = false; };
    struct Prop {
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        ue::UObject* root = nullptr;       // bodies[0].comp
        int32_t rootIndex = -1;
        std::vector<BodyRef> bodies;       // BodyIndex order: the root body first, then the actor's components in their order
        std::vector<ConstraintRef> constraints;   // PhysicsConstraintComponents of the actor, by index
        uint64_t brokenMask = 0;           // constraints known broken (a bit per index)
        uint64_t attachedMask = 0;         // bodies (>= 1) that hung on a parent when tracked
        uint32_t classHash = 0;
        bool spawned = false;              // a spawned object (MakeSpawnedId), announced
        bool provisional = false;          // proposed here under a provisional id: nothing streams or claims until it is re-keyed
        bool rootBody = true;              // bodies[0] is the actor's root component (else every body is placed by its component)
        bool sub = false;                 // a detached body: its own object on its old actor (not in byActor_)
        bool attachedSent = false;         // attached to another tracked object: not streamed or driven any more
        uint64_t attachParent = 0;         // the object it is attached to (Detached on body 0 names it when it is let go of)
        double provisionalMs = 0;          // when it was proposed
        LeaseClaim claim;
        double lastContactMs = -1e18;
        double lastSentMs = -1e18;
        int restoreTicks = 0;              // physics restore re-asserted after a stand-in let go of it
        bool dirty = false;                // a fresh state from the streamer was applied to the view since the last drive
        bool denied = false;               // held here while another machine holds it
        bool level = false;                // a level prop (MakeLevelPropId), not a dropped weapon
        bool moving = false;               // its last read was not at rest (level props: streamed before those at rest)
        uint8_t toucher = 0;               // the fighter simulated here that held or touched it last (Touch)
        double bornMs = 0;                 // when it was first tracked here (a spawned object born in the round is destroyed by a map reset)
        bool startTried = false;           // a level prop: its start was read (once)
        std::vector<StartBody> start;      // a level prop: each body's transform as the level began (map reset)
    };
    using Hands = hands::Hands;
    using StandInHand = hands::StandInHand;
    using StandInHands = hands::StandInHands;
    using PendingDrop = hands::PendingDrop;
    using Loose = hands::Loose;
    using Candidate = hands::Candidate;
    using DropResult = hands::DropResult;
    struct LevelActor {                    // a physics prop of the level as the census walk found it here
        ue::UObject* actor = nullptr;
        int32_t index = -1;
        uint32_t classHash = 0;
        ue::FVector at;
        bool anySim = false;               // some movable primitive of it simulates physics
        ue::UObject* simComp = nullptr;    // the first one that does
        uint8_t bodies = 0;                // movable primitives it has (capped at kMaxObjectBodies)
        bool asleep = false;               // tracked here and at rest: its components' physics flags are kept longest (SimCache)
    };
    struct Announced {                     // a canonical announce with no actor here yet
        ObjectAnnounceMsg msg;
        double sinceMs = 0;
    };
    struct QueuedEvent { ObjectEventMsg msg; double atMs = 0; };
    using LevelKey = std::pair<const ue::UObject*, int32_t>;        // a level actor and its object-array slot
    struct Counters {
        uint64_t drops = 0, minted = 0, mintFailed = 0, resolved = 0, forced = 0, kept = 0, unresolved = 0, releaseSkipped = 0, releaseFailed = 0,
                 requests = 0, releases = 0, grants = 0, lapsed = 0, statesSent = 0, statesApplied = 0, statesStale = 0,
                 statesRejected = 0, leasesRejected = 0, driven = 0, holdDenied = 0, contacts = 0, islands = 0, readFailed = 0, lost = 0,
                 attached = 0, attachMiss = 0, resetUnmoved = 0, matchReleased = 0, selfDrops = 0, reattached = 0, leaseReleased = 0, adopted = 0,
                 spawned = 0, spawnFailed = 0,
                 censuses = 0, censusRejected = 0, levelStreamed = 0, levelDriven = 0, levelDeferred = 0, censusPurged = 0,
                 despawned = 0,
                 announced = 0, proposed = 0, refused = 0, rekeyed = 0, bound = 0, ghostsExpired = 0, announceRejected = 0,
                 eventsSent = 0, eventsApplied = 0, eventsQueued = 0, eventsRejected = 0, jointsBroken = 0, bodiesDetached = 0,
                 destroyedSent = 0, destroyedApplied = 0, rechecks = 0, bodyDriven = 0,
                 skippedClasses = 0, attachHeld = 0, letGoSent = 0, letGoApplied = 0, adoptKnown = 0, adoptBound = 0, adoptExpired = 0,
                 simAsked = 0, simCached = 0, simCapped = 0;
    };

    static constexpr double kCensusRefreshMs = 5000.0;  // the host walks again this often; a client retries its misses this often
    static constexpr double kProposalWaitMs = 5000.0;   // a provisional id waits this long for the host's canonical announce
    static constexpr double kRefusedKeepMs = 30000.0;   // an actor the host did not name is not proposed again for this long
    static constexpr double kEventPollMs = 250.0;       // joint, detach and attach checks of the objects streamed here
    static constexpr double kHintGapMs = 250.0;         // NoteSpawn pulls the re-check forward at most this often
    static constexpr double kEventKeepMs = 20000.0;     // an event for an object not found here yet is kept this long
    static constexpr size_t kMaxQueuedEvents = 128, kMaxHistory = 128, kProposalsPerPass = 4, kSpawnsPerPass = 8;
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
    bool Read(Prop& p, size_t body, BodyStateMsg& out);
    // Read every simulating body of an object I stream and send them when due; returns the bodies read (the tick's budget).
    size_t StreamState(uint64_t propId, Prop& p, LeaseLevel level, uint8_t me, double nowMs);
    void StreamLevel(const std::vector<uint64_t>& due, uint64_t& cursor, size_t& budget, bool countDeferred, uint8_t me, double nowMs);
    bool Drive(uint64_t id, Prop& p);                                // every body with a fresh state from the streamer
    bool DriveBody(Prop& p, size_t body, const BodyStateMsg& s);
    bool BuildBodies(Prop& p, bool multi);                           // BodyIndex over the actor's movable primitives
    bool LooseParts(ue::UObject* actor, ue::UObject* root);          // a simulating movable primitive besides the root (a flail's chain)
    std::vector<ue::UObject*> ComponentsOf(ue::UObject* actor, const ue::UClass* cls);
    bool MovableComp(ue::UObject* comp);
    bool ClassSkipped(const ue::UClass* actorClass);                 // an effect, gore or mod actor class (or a child of one)
    void BindAdopted(std::vector<LevelActor>& unnamed, double nowMs);   // host: bind the adopted announces to actors found
    // Level props (session/hsm_prop_census.h).
    std::vector<LevelActor> WalkLevel(size_t& notSimulating);       // every physics object of the level here
    void HostCensus(double nowMs);                                   // walk, index, track, and send when it changed
    void ClientCensus(double nowMs);                                 // bind the census's indices to actors here
    // Spawned objects and events (object_events.cpp).
    void Recheck(double nowMs);                                      // client: walk, bind announces, propose what nobody names
    void AfterWalk(std::vector<LevelActor>& free, double nowMs);     // free: tracked by nothing yet
    void HostSpawn(const LevelActor& a, double nowMs);               // host: a new object -> a canonical id and its announce
    void ProposeLocal(const LevelActor& a, double nowMs);            // client: provisional id and a proposal
    void HostPropose(const ObjectAnnounceMsg& m, uint8_t fromSlot, double nowMs);
    void ClientAnnounce(const ObjectAnnounceMsg& m, double nowMs);
    void BindAnnounced(std::vector<LevelActor>& free, double nowMs); // match pending announces to actors in free
    void Bound(uint64_t id, double nowMs);                           // an object got its actor here: queued events replay
    bool Rekey(uint64_t provisional, uint64_t canonical);
    uint64_t MintSpawned();
    void SendEvent(const ObjectEventMsg& m);
    void PollEvents(double nowMs);
    void Replay(const ObjectEventMsg& m, double nowMs);
    bool ReplayNow(const ObjectEventMsg& m, double nowMs);          // false: the object is not here (yet)
    Prop* TrackSub(uint64_t id, Prop& from, size_t body, double nowMs, bool mine);
    bool CompMoving(ue::UObject* comp);
    // Shattering objects (shatter_sync.cpp): geometry collections break on the machine that streams them and the break is
    // an ObjectEvent Fracture everywhere else.
    void ShatterPoll(double nowMs);                                  // every kEventPollMs: detect, set up, verify, log breakables
    bool ShatterReplay(const ObjectEventMsg& m, double nowMs);       // false: the object is not here (yet)
    ShatterObject& ShatterScan(uint64_t id, Prop& p);
    void ShatterStep(uint64_t id, ShatterBody& b, bool streams, double nowMs);
    void ShatterBreak(uint64_t id, ShatterBody& b, double nowMs);
    void LogBreakables();
    ue::UObject* ParentActorOf(ue::UObject* actor);
    bool InCensus(uint64_t propId) const;                            // a level id of the current census
    bool TaggedRandomDelete(ue::UObject* actor);
    void RestorePhysics(ue::UObject* actor, ue::UObject* root, int32_t rootIndex);   // every movable primitive of the actor
    RootCalls* CallsFor(ue::UObject* root);
    ue::UObject* ObjectField(ue::UObject* o, int which);             // 0 "Parent Actor", 1 "Weapon R", 2 "Weapon L"
    bool InUse(uint64_t propId, double nowMs) const;
    // Map reset (map_reset_sync.cpp).
    void RecordStart(Prop& p);                                       // a level prop: read each body's transform once
    void StepReset(double nowMs);                                    // a few queued items a frame
    void ResetProp(uint64_t id, Prop& p, ResetAction action, double nowMs);
    void ResetWeapon(size_t i, ResetAction action, double nowMs);
    void ReleaseLease(uint64_t id, Prop& p, double nowMs);                      // a reset prop is FREE again
    bool ShatterBroken(uint64_t id) const;                           // a geometry collection of it has broken
    // A dropped weapon FREE for a minute goes (session/hsm_lease.h IdleDespawnDue); every kIdleCheckMs, while SetIdleDespawn.
    void DespawnIdle(double nowMs);

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    Env env_;
    uint64_t tick_ = 0;
    LeaseTable table_;                                               // host only
    BodyStateView view_;
    std::map<uint64_t, Prop> props_;                                 // every prop with an actor here
    std::unordered_map<const ue::UObject*, uint64_t> byActor_;
    std::map<uint64_t, PendingDrop> pending_;                        // drops heard of, not found yet
    // Drops given up as unresolved -> when: a later Grant of the id does not open another window (test 5: one came back
    // with every Grant and fired its deadline 18 s after the drop); forgotten after kLooseKeepMs.
    std::unordered_map<uint64_t, double> givenUp_;
    std::unordered_map<uint64_t, double> heardMs_;                   // last BodyState heard per id (minting skips live ids)
    std::unordered_map<uint64_t, double> freeSince_;                 // when each lease went FREE here (its first FREE Grant)
    double idleCheckMs_ = 0;                                         // DespawnIdle: the next check
    bool idleDespawn_ = false;                                       // the bot formats (match/hsm_match.h DropClearFor)
    std::array<Hands, kMaxWillies + 1> hands_{};
    std::array<uint8_t, kMaxWillies + 1> dropSeq_{};
    std::array<uint32_t, kMaxWillies + 1> dropEpoch_{};                // seeded per process at Start (MakeDropPropId)
    std::array<StandInHands, kMaxWillies + 1> standInHands_{};
    std::vector<Loose> loose_;
    std::vector<Candidate> weapons_;
    std::vector<Candidate> idle_;          // the level's movable objects at rest at the last walk (no census prop): Attach's last resort (#88)
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
    std::unordered_map<const ue::UClass*, uint8_t> classKind_;      // WalkLevel: 1 a primitive component class
    std::unordered_map<const ue::UClass*, uint8_t> actorKind_;      // WalkLevel: 1 an actor class, 2 one never synced (ObjectClassSkipped)
    std::set<std::string> skippedLogged_;                           // class names skipped, logged once per level
    SimCache simCache_;                                             // WalkLevel: IsSimulatingPhysics per component, kept for a while
    double simPruneMs_ = 0;
    AttachLatch attach_;                                            // attached objects held against states in flight
    // Host after a migration: objects the old host announced (every machine heard them), not yet bound to an actor of the new
    // host's own. Kept across a level change; ones nothing matched by adoptUntilMs_ are given up.
    std::vector<ObjectAnnounceMsg> adopt_;
    double adoptUntilMs_ = 0;
    // Spawned objects (object_events.cpp). reconciler_: host only. minter_: this machine's provisional ids (client).
    ObjectReconciler reconciler_;
    SpawnedIdMinter minter_;
    GhostTable ghosts_;
    std::map<uint64_t, Announced> announced_;                        // canonical announces with no actor here yet
    std::unordered_map<const ue::UObject*, double> refused_;         // actors the host would not name -> when to try again
    std::unordered_map<const ue::UObject*, double> touched_;         // untracked actors a fighter simulated here touched
    std::set<const ue::UObject*> leftBefore_;                        // unnamed, simulating actors the previous re-check saw
    std::set<uint64_t> detached_;                                    // host: ids of detached bodies (valid spawned ids)
    std::set<uint64_t> destroyed_;                                   // spawned ids destroyed (a late announce is not bound)
    std::vector<QueuedEvent> queuedEvents_;                          // events for objects not found here yet
    std::vector<std::vector<uint8_t>> history_;                      // host: events relayed, for a joiner
    ShatterState shatter_;                                           // geometry collections (shatter_sync.cpp)
    // Map reset. baselineMs_: when the match began in this level (-1: not yet); levelWeapons_: the level's own weapons then;
    // resetQueue_: what a MapReset still has to do, resetPerFrame_ items a frame.
    double baselineMs_ = -1;
    std::vector<LevelWeapon> levelWeapons_;
    std::vector<ResetItem> resetQueue_;
    size_t resetPerFrame_ = 0;
    ResetTally resetTally_;
    double recheckMs_ = 0, eventMs_ = 0, hintMs_ = -1e18;
    ue::UClass* primitiveClass_ = nullptr;
    ue::UClass* constraintClass_ = nullptr;
    ue::PreparedCall components_;                                    // Actor::K2_GetComponentsByClass
    Counters n_, reported_;
};

}  // namespace hsm::game

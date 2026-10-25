#include "gameplay/prop_sync.h"
#include "gameplay/world_sync.h"
#include "log.h"
#include "sync/hsm_pose_codec.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

// Identity. A weapon's SheathSlot is a sheath type shared by many weapons and its "Last Parent" is cleared by the game,
// so no machine can name a dropped weapon from what it sees alone. The dropping fighter's machine mints the id
// (MakeDropPropId: owner entity, hand, a 7-bit sequence under a per-process drop epoch) and asks for a Touch on it;
// everyone else finds the weapon in the hand of its stand-in of that fighter (DropSignature, ResolveDropNow). An id
// is kept for the weapon's life: a pick-up (HOLD) and a later drop by anyone reuse it. A level prop has no name two
// machines share (the level manager's purge and respawn make per-machine actors), so the host names them: its census
// (session/hsm_prop_census.h) gives each an index, MakeLevelPropId(index), and each client binds its own actor to it.
//
// Level props on the wire. The host walks its level kCensusFirstMs into a fight and every kCensusRefreshMs after (at
// once when one is lost) and sends the census whenever its set changed; a client matches each complete census it has
// not matched yet, and retries the indices it could not bind every kCensusRefreshMs. A level prop is FREE (the host
// streams it) or TOUCH (the machine whose fighter touched it last), never HOLD. Most of them lie still, so they are
// read at most kLevelReadsPerTick a tick, the moving ones first.
//
// Driving. A machine that does not stream a prop teleports its copy to each new state from the prop's streamer
// (ApplyWorldObject, plus the angular velocity), and only while the copy is loose here: a copy in the hand of a
// fighter simulated here (a Hold the host denied) or of a stand-in is never moved by a stream. A request for a FREE
// prop is always granted, so local physics keeps it until that Grant arrives rather than fighting the host's stream.

namespace hsm::game {
using namespace hsm::ue;

namespace {
constexpr double kResolveWindowMs = 1500.0;   // a drop's Grant waits this long for the owner's pose to show the hand empty
constexpr double kHeardLiveMs = 3000.0;       // an id heard this recently is live somewhere (the host re-sends FREE ones every second)
constexpr int kRestoreTicks = 3;              // the rig's full neutralise may re-apply its body policy once before it lets go
constexpr double kRetryMs = 250.0;            // a stand-in hand's attach or release waits this long before it is tried again,
constexpr double kRetryMaxMs = 4000.0;        // doubling up to this, until the owner's pose for that hand changes
constexpr double kLooseKeepMs = 10000.0;      // a weapon a stand-in let go of here waits this long for its drop to be heard of
constexpr double kWeaponsRefreshMs = 10000.0; // the attach's list of weapon actors (WeaponActors walks every object) is read
                                              // again at most this often; props and loose_ are always looked at
constexpr uint64_t kLogFirst = 10;            // the first attaches, releases, self-drops and adoptions are logged in full
constexpr uint8_t kWorldStatic = 0, kWorldDynamic = 1, kBlock = 2;   // ECollisionChannel / ECollisionResponse
constexpr double kCensusFirstMs = 3000.0;     // a fight level's first census walk (and a client's first match) waits this
                                              // long: past the level manager's purge (0.1 s) and its first respawns (2.5 s)
constexpr double kCensusRefreshMs = 5000.0;   // the host walks again this often; a client retries its misses this often
constexpr size_t kLevelReadsPerTick = 16;     // level props read (and so streamed) per tick at most, the moving ones first
constexpr uint8_t kMovable = 2;               // EComponentMobility::Movable: only a movable root can simulate

// Where this process starts every entity's drop epoch: the wall clock, so a machine that reconnects (Stop zeroes the
// sequences) does not mint the ids its previous session gave weapons that may still lie in the level elsewhere.
uint32_t DropEpochSeed() {
    const uint64_t t = uint64_t(std::chrono::system_clock::now().time_since_epoch().count());
    return uint32_t(t ^ (t >> 32));
}

float Clamped(double v, float limit) {
    if (!std::isfinite(v)) return 0.f;
    const double l = double(limit) * 0.999;
    return float(std::clamp(v, -l, l));
}
}  // namespace

void PropSync::Start(Adapter& adapter, StandIns& standIns, Env env) {
    Stop();
    dropEpoch_.fill(DropEpochSeed());
    adapter_ = &adapter;
    standIns_ = &standIns;
    env_ = std::move(env);
    active_ = true;
    HSM_LOG("props: on (%s): dropped weapons are leased FREE/TOUCH/HOLD; level props by the host's census, FREE/TOUCH",
            env_.host ? "host arbitrates" : "client");
}

void PropSync::Stop() {
    if (active_) HSM_LOG("props: off (%s)", HealthLine().c_str());
    *this = PropSync{};
}

void PropSync::Clear() {
    if (!active_) return;
    table_ = LeaseTable{};
    view_.Clear();
    props_.clear();
    byActor_.clear();
    pending_.clear();
    givenUp_.clear();
    heardMs_.clear();
    hands_ = {};
    standInHands_ = {};
    loose_.clear();
    weapons_.clear();
    weaponsMs_ = -1e18;
    calls_.clear();
    fieldOff_ = {};
    // Level props: a new level, a new census. censusSeq_ goes on (a client may still hold the last level's census until
    // its own level change), and the 'Random Delete' name stays what it is.
    levelSinceMs_ = -1;
    censusMs_ = 0;
    censusIndex_.clear();
    movingCursor_ = idleCursor_ = 0;
    book_.Clear();
    sent_.clear();
    haveSent_ = false;
    census_.Clear();
    matched_ = false;
    matchedSeq_ = 0;
    bound_.clear();
    misses_ = 0;
    purged_ = false;
    rootOff_.clear();
    mobilityOff_.clear();
    classKind_.clear();
}

// ---------------------------------------------------------------- per frame

void PropSync::Tick(double nowMs, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
    if (!active_ || !adapter_) return;
    ++tick_;
    if (env_.host)
        for (uint64_t id : table_.Tick(nowMs)) { ++n_.lapsed; Grant(id, kHostSlot, LeaseLevel::Free); }
    DetectDrops(nowMs, owned);
    if (!pending_.empty()) ResolvePending(nowMs);
    MatchStandIns(nowMs);
    if (!loose_.empty()) TickLoose(nowMs);
    if (env_.fightLevel && env_.fightLevel()) {
        if (levelSinceMs_ < 0) { levelSinceMs_ = nowMs; censusMs_ = nowMs + kCensusFirstMs; }
        if (env_.host) {
            if (nowMs >= censusMs_) HostCensus(nowMs);
        } else if (census_.HaveCensus() && nowMs >= levelSinceMs_ + kCensusFirstMs &&
                   (!matched_ || census_.Seq() != matchedSeq_ || (bound_.size() < census_.Census().size() && nowMs >= censusMs_))) {
            ClientCensus(nowMs);
        }
    }
    const uint8_t me = MySlot();
    std::vector<uint64_t> dueMoving, dueIdle;   // level props due on the moving schedule, by their last read
    for (auto it = props_.begin(); it != props_.end();) {
        const uint64_t id = it->first;
        Prop& p = it->second;
        if (!adapter_->StillValid(p.actor, p.index)) {   // destroyed here (a respawn takes what its fighter held)
            ++n_.lost;
            if (p.claim.sent != LeaseLevel::Free) Claim(id, ClaimSend::Release, LeaseLevel::Free, nowMs);
            // A level prop: the host walks again at once (its census drops it); a client binds that index again when it can.
            if (p.level) { if (env_.host) censusMs_ = nowMs; else bound_.erase(LevelPropIndex(id)); }
            byActor_.erase(p.actor);
            it = props_.erase(it);
            continue;
        }
        UObject* parent = ObjectField(p.actor, 0);
        const uint8_t holder = parent && env_.ownedEntity ? env_.ownedEntity(parent) : 0;
        const bool holding = holder != 0;
        if (holding) p.toucher = holder;
        if (p.restoreTicks > 0) { --p.restoreTicks; if (!parent) RestorePhysics(p.root, p.rootIndex); }

        bool moving = false;   // only read when it decides anything: a let-go claim of mine waiting for rest
        if (!holding && p.claim.sent != LeaseLevel::Free && nowMs - p.lastContactMs >= kContactKeepMs) {
            PropStateMsg s;
            moving = Read(p, s) && !PropAtRest(s);
        }
        LeaseLevel want = LeaseLevel::Free;
        const ClaimSend cs = StepClaim(p.claim, holding, p.lastContactMs, moving, nowMs, want);
        if (cs != ClaimSend::None) Claim(id, cs, want, nowMs);

        const uint8_t streamer = view_.Streamer(id);
        const LeaseLevel level = view_.Level(id);
        const bool denied = holding && streamer != me && level == LeaseLevel::Hold;
        if (denied && !p.denied) ++n_.holdDenied;
        p.denied = denied;

        if (streamer == me) {
            p.dirty = false;
            // A lease of mine I have just released streams no more (the host would drop it until its FREE Grant lands).
            const bool streaming = level == LeaseLevel::Free || p.claim.sent != LeaseLevel::Free;
            if (streaming && PropStreamDue(level, p.lastSentMs, false, nowMs)) {   // due on the moving schedule: worth a read
                if (!p.level) StreamState(id, p, level, me, nowMs);
                else (p.moving ? dueMoving : dueIdle).push_back(id);
            }
        } else if (p.dirty) {
            p.dirty = false;
            const bool awaitingGrant = p.claim.sent != LeaseLevel::Free && level == LeaseLevel::Free;
            if (!parent && !awaitingGrant)
                if (const PropStateMsg* s = view_.Find(id))
                    if (Drive(p, *s) && p.level) ++n_.levelDriven;
        }
        ++it;
    }
    size_t budget = kLevelReadsPerTick;
    StreamLevel(dueMoving, movingCursor_, budget, true, me, nowMs);
    StreamLevel(dueIdle, idleCursor_, budget, false, me, nowMs);
}

// Read a prop I stream, and send its state when it is due by that read (at rest: the slow schedule).
void PropSync::StreamState(uint64_t propId, Prop& p, LeaseLevel level, uint8_t me, double nowMs) {
    PropStateMsg s;
    if (!Read(p, s)) return;
    const bool atRest = PropAtRest(s);
    p.moving = !atRest;
    if (!PropStreamDue(level, p.lastSentMs, atRest, nowMs)) return;
    s.propId = propId;
    s.lessee = me;
    s.senderTimeMs = uint32_t(uint64_t(nowMs));
    if (env_.send) env_.send(Encode(s), false);
    p.lastSentMs = nowMs;
    ++n_.statesSent;
    if (p.level) ++n_.levelStreamed;
}

// Level props due a read, in id order: as many as the budget allows, round robin from the one after cursor so each gets
// its turn however many there are. Those left over wait for a later tick; for props that were moving (countDeferred)
// that is a stream falling behind, counted in levelDeferred (one at rest only waits for its next once-a-second send).
void PropSync::StreamLevel(const std::vector<uint64_t>& due, uint64_t& cursor, size_t& budget, bool countDeferred, uint8_t me,
                           double nowMs) {
    if (due.empty()) return;
    const size_t start = size_t(std::upper_bound(due.begin(), due.end(), cursor) - due.begin());
    size_t done = 0;
    for (; done < due.size() && budget > 0; ++done, --budget) {
        const uint64_t id = due[(start + done) % due.size()];
        cursor = id;
        if (auto it = props_.find(id); it != props_.end()) StreamState(id, it->second, view_.Level(id), me, nowMs);
    }
    if (countDeferred) n_.levelDeferred += due.size() - done;
}

// A weapon leaves the hand of a fighter simulated here (dropped, thrown, disarmed): the hand field moves off it and
// the game clears its "Parent Actor" (a sheathed weapon keeps it, Willie_BP "Set Up Right Hand Weapon").
void PropSync::DetectDrops(double nowMs, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
    std::array<bool, kMaxWillies + 1> seen{};
    for (const auto& [e, w] : owned) {
        if (!ValidEntity(e) || !w) continue;
        seen[e] = true;
        Hands& h = hands_[e];
        std::array<UObject*, 2> cur{ObjectField(w, 1), ObjectField(w, 2)};
        if (cur[1] == cur[0]) cur[1] = nullptr;   // one weapon in both hands: the right hand has it (as the rig counts it)
        if (h.willie == w) {
            for (int hand = 0; hand < 2; ++hand) {
                UObject* prev = h.weapon[hand];
                if (!prev || prev == cur[0] || prev == cur[1]) continue;   // still held, maybe in the other hand
                if (!adapter_->StillValid(prev, h.index[hand]) || !adapter_->IsWeapon(prev) || adapter_->IsBodyWeapon(prev) ||
                    ObjectField(prev, 0))
                    continue;
                ++n_.drops;
                uint64_t id = 0;
                if (auto known = byActor_.find(prev); known != byActor_.end()) id = known->second;
                else {
                    id = MintDropPropId(e, uint8_t(hand), dropSeq_[e], dropEpoch_[e], [&](uint64_t c) { return InUse(c, nowMs); });
                    if (!id) { ++n_.mintFailed; continue; }
                    ++n_.minted;
                    if (!Track(id, prev)) continue;
                }
                if (auto p = props_.find(id); p != props_.end()) p->second.lastContactMs = nowMs;   // Touch now
            }
        }
        h = Hands{};   // a new fighter (respawn) starts clean: nothing it never held is a drop
        h.willie = w;
        for (int hand = 0; hand < 2; ++hand) {
            h.weapon[hand] = cur[hand];
            h.index[hand] = cur[hand] ? ObjectIndex(cur[hand]) : -1;
        }
    }
    for (uint8_t e = 1; e <= kMaxWillies; ++e)
        if (!seen[e] && hands_[e].willie) hands_[e] = Hands{};
}

// Drops heard of (a Touch Grant on an id this machine does not know, from the machine that owns the id's fighter). The
// weapon in that hand of the stand-in when the drop was heard is its witness (OnGrant): the drop's copy, let go of once
// the owner's pose shows that hand empty or another class, or at the deadline whatever the pose shows (no pose, or one
// that never caught up; DeadlineRelease). A witness the stand-in let go of already (it left by itself, or the hand
// matching was quicker) is adopted from loose_; with no witness (the hand was empty when the drop was heard) only a
// weapon let go of from that hand before can be the drop's (sync::AdoptCandidate). Anything else in that hand is never
// the drop's (test 5: a drop heard while the hand was empty forced out the secondary drawn 4 s later): at the deadline
// the drop is given up (GiveUp).
void PropSync::ResolvePending(double nowMs) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        const uint64_t id = it->first;
        const PendingDrop& d = it->second;
        bool done = props_.count(id) != 0;
        const uint8_t owner = PropOwnerEntity(id);
        const int hand = PropHand(id);
        if (!done) {
            UObject* standIn = standIns_->ActorOf(owner);
            UObject* copy = UntrackedInHand(standIn, hand);
            sync::DropFacts f;
            f.witnessed = d.witness != nullptr;
            f.holdsWitness = f.witnessed && copy == d.witness && adapter_->StillValid(copy, d.witnessIndex);
            f.deadline = nowMs >= d.deadlineMs;
            if (f.holdsWitness) {
                uint32_t poseClass = 0;
                const bool havePose = standIns_->PoseHand(owner, hand, poseClass);
                f.poseShowsLetGo = ResolveDropNow(havePose, poseClass, ClassHashOf(copy));
            }
            if (!f.holdsWitness && Adopt(id, owner, hand, d, nowMs)) {
                done = true;
            } else {
                switch (sync::StepDrop(f)) {
                case sync::DropAct::Wait: break;
                case sync::DropAct::Release:
                    done = true;
                    if (!f.poseShowsLetGo) DeadlineRelease(id, owner, hand, standIn, copy);
                    else if (ReleaseFromStandIn(id, standIn, hand, copy)) ++n_.resolved;
                    break;
                case sync::DropAct::GiveUp:
                    done = true;
                    GiveUp(id, owner, hand, d, copy, nowMs);
                    break;
                }
            }
        }
        it = done ? pending_.erase(it) : std::next(it);
    }
}

// A real weapon in that hand of the stand-in (its "Parent Actor" the stand-in) that no prop names; nullptr if none.
UObject* PropSync::UntrackedInHand(UObject* standIn, int hand) {
    UObject* w = standIn ? ObjectField(standIn, hand == 0 ? 1 : 2) : nullptr;
    if (!w || byActor_.count(w) || !adapter_->IsWeapon(w) || adapter_->IsBodyWeapon(w) || ObjectField(w, 0) != standIn) return nullptr;
    return w;
}

// A drop with no copy here: its witness is gone, or there was none and nothing that hand let go of before is the drop's.
// The weapon stays wherever it is here, and a later Grant of the id opens no other window (givenUp_).
void PropSync::GiveUp(uint64_t id, uint8_t owner, int hand, const PendingDrop& d, UObject* copy, double nowMs) {
    ++n_.unresolved;
    for (auto g = givenUp_.begin(); g != givenUp_.end();) g = nowMs - g->second >= kLooseKeepMs ? givenUp_.erase(g) : std::next(g);
    givenUp_[id] = nowMs;
    std::string heard = "that hand empty";
    if (d.witness)
        heard = adapter_->StillValid(d.witness, d.witnessIndex) ? ObjectName(d.witness) + " in it, not in that hand since"
                                                                 : std::string("a weapon in it, destroyed since");
    const std::string holds = copy ? ObjectName(copy) : std::string("nothing untracked");
    HSM_LOG("props: drop %016llx (entity %u, hand %d) not found on its stand-in (heard with %s; that hand holds %s now); that "
            "weapon stays local here", static_cast<unsigned long long>(id), owner, hand, heard.c_str(), holds.c_str());
}

// A drop whose deadline passed while the stand-in still holds its witness. Let go of (forced), unless the owner holds
// that prop again (a Hold Grant to the owner's slot), when the copy in hand is that weapon, held, and becomes the prop
// where it is (kept).
void PropSync::DeadlineRelease(uint64_t id, uint8_t owner, int hand, UObject* standIn, UObject* copy) {
    if (view_.Level(id) == LeaseLevel::Hold && view_.Streamer(id) == OwnerSlotOf(owner)) {
        Prop* p = Track(id, copy);
        if (!p) return;
        p->dirty = view_.Find(id) != nullptr;
        if (++n_.kept <= kLogFirst)
            HSM_LOG("props: drop %016llx (entity %u, hand %d): the owner holds it again; %s in that hand of its stand-in is that prop",
                    static_cast<unsigned long long>(id), owner, hand, ObjectName(copy).c_str());
        return;
    }
    const std::string name = ObjectName(copy);
    if (ReleaseFromStandIn(id, standIn, hand, copy) && ++n_.forced <= kLogFirst)
        HSM_LOG("props: drop %016llx (entity %u, hand %d): the owner's pose never showed that hand let go; %s let go of at the deadline",
                static_cast<unsigned long long>(id), owner, hand, name.c_str());
}

// The game's own drop on the stand-in: "Set Up ... Hand Weapon" with no class detaches the weapon in that hand
// ("Dropped With no Damage": no blow), clears the hand and the weapon's "Parent Actor". With the grip type at 0 the
// same function destroys the weapon instead, so that case is left alone. Not under ScriptHookBypass: the claims
// hooks keep guarding anything it might reach.
bool PropSync::ReleaseFromStandIn(uint64_t propId, UObject* standIn, int hand, UObject* copy) {
    char what[24];
    std::snprintf(what, sizeof what, "%016llx", static_cast<unsigned long long>(propId));
    const DropResult r = DropFromHands(standIn, hand, copy, what);
    ForgetHeld(PropOwnerEntity(propId), standIn);
    if (r != DropResult::Dropped) return false;
    if (env_.handsChanged) env_.handsChanged(PropOwnerEntity(propId));
    Prop* p = Track(propId, copy);
    if (!p) return false;
    p->restoreTicks = kRestoreTicks;
    RestorePhysics(p->root, p->rootIndex);
    p->dirty = view_.Find(propId) != nullptr;
    return true;
}

// The drop itself, for ReleaseFromStandIn and the hand matching: both hands when the stand-in holds copy in both.
// Dropped once neither hand holds it and its "Parent Actor" is clear; what names it in the log.
PropSync::DropResult PropSync::DropFromHands(UObject* standIn, int hand, UObject* copy, const std::string& what) {
    const int32_t copyIndex = ObjectIndex(copy);
    for (int pass = 0; pass < 2; ++pass) {
        const int h = pass == 0 ? hand : 1 - hand;
        if (pass == 1 && ObjectField(standIn, h == 0 ? 1 : 2) != copy) break;   // held in both hands: let the other go too
        uint8_t* grip = h == 0 ? Field<uint8_t>(standIn, "R_GripType_Current") : Field<uint8_t>(standIn, "L_GripType_Current");
        if (!grip || *grip == 0) {
            ++n_.releaseSkipped;
            HSM_LOG("props: %s hand %d grip type unreadable or 0: not dropping %s there (the game would destroy it)",
                    ObjectName(standIn).c_str(), h, what.c_str());
            return DropResult::Skipped;
        }
        bool ok = false;
        if (h == 0) {
            Call c(standIn, "Set Up Right Hand Weapon");
            c.set<bool>("Dropped With no Damage", true);
            ok = c.invoke();
        } else {
            Call c(standIn, "Set Up Left Hand Weapon");
            c.set<bool>("Dropped with No Damage", true);
            ok = c.invoke();
        }
        if (!ok) break;
    }
    if (!adapter_->StillValid(copy, copyIndex) || ObjectField(standIn, 1) == copy || ObjectField(standIn, 2) == copy ||
        ObjectField(copy, 0)) {
        ++n_.releaseFailed;
        HSM_LOG("props: %s did not let go of %s; that weapon stays with the stand-in here", ObjectName(standIn).c_str(), what.c_str());
        return DropResult::Failed;
    }
    return DropResult::Dropped;
}

PropSync::Prop* PropSync::Track(uint64_t propId, UObject* actor) {
    if (props_.count(propId) || byActor_.count(actor)) return nullptr;
    Call rc(actor, "K2_GetRootComponent");
    UObject* root = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
    if (!root || !CallsFor(root)) {
        ++n_.readFailed;
        HSM_LOG("props: %s has no physics root; %016llx is not synced", ObjectName(actor).c_str(), static_cast<unsigned long long>(propId));
        return nullptr;
    }
    Prop& p = props_[propId];
    p.actor = actor;
    p.index = ObjectIndex(actor);
    p.root = root;
    p.rootIndex = ObjectIndex(root);
    byActor_[actor] = propId;
    return &p;
}

// The stand-in rig kept its weapons gravity-free and blind to world geometry (FighterRig::SetBodyPolicy); a weapon it
// let go of falls and lands like any other (Block on WorldStatic and WorldDynamic, the weapon's default response).
void PropSync::RestorePhysics(UObject* root, int32_t rootIndex) {
    if (!adapter_->StillValid(root, rootIndex)) return;
    Call(root, "SetEnableGravity").set<bool>("bGravityEnabled", true).invoke();
    Call(root, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldStatic).set<uint8_t>("NewResponse", kBlock).invoke();
    Call(root, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldDynamic).set<uint8_t>("NewResponse", kBlock).invoke();
}

// ---------------------------------------------------------------- stand-in hands (sync/hsm_handmatch.h)

uint32_t PropSync::ClassHashOf(UObject* actor) {
    return actor ? sync::WeaponClassHash(ObjectName(reinterpret_cast<UObject*>(ClassOf(actor))).c_str()) : 0;
}

// What a stand-in holds as the pose counts it: real weapons only (no fists or feet), one weapon in both hands as the right's.
std::array<UObject*, 2> PropSync::HandsOf(UObject* standIn) {
    std::array<UObject*, 2> cur{ObjectField(standIn, 1), ObjectField(standIn, 2)};
    if (cur[1] == cur[0]) cur[1] = nullptr;
    for (UObject*& w : cur)
        if (w && (!adapter_->IsWeapon(w) || adapter_->IsBodyWeapon(w))) w = nullptr;
    return cur;
}

void PropSync::SetHeld(StandInHand& sh, UObject* weapon) {
    if (sh.held == weapon) return;
    sh.held = weapon;
    sh.heldIndex = weapon ? ObjectIndex(weapon) : -1;
    sh.heldClass = ClassHashOf(weapon);
    sh.match.Reset();   // a mismatch clock belongs to the weapon it was started for
}

// This code changed the stand-in's hands: what they hold now is the matcher's starting point, not a weapon that left.
void PropSync::ForgetHeld(uint8_t entity, UObject* standIn) {
    if (!ValidEntity(entity)) return;
    StandInHands& s = standInHands_[entity];
    if (s.willie != standIn) return;
    const std::array<UObject*, 2> cur = HandsOf(standIn);
    for (int h = 0; h < 2; ++h) SetHeld(s.hand[size_t(h)], cur[size_t(h)]);
}

// Every stand-in's hands against its owner's pose.
void PropSync::MatchStandIns(double nowMs) {
    if (!env_.equipToken || !env_.mayBeDraw || !env_.mayBeSheathe || !env_.deadBones) return;
    for (uint8_t e = 1; e <= kMaxWillies; ++e) {
        StandInHands& s = standInHands_[e];
        UObject* standIn = standIns_->ActorOf(e);
        if (!standIn) { if (s.willie) s = StandInHands{}; continue; }
        const int32_t index = ObjectIndex(standIn);
        std::array<UObject*, 2> cur = HandsOf(standIn);
        if (s.willie != standIn || s.index != index) {   // a new stand-in (bound, respawned): what it holds now is its start
            s = StandInHands{};
            s.willie = standIn;
            s.index = index;
            s.boundMs = nowMs;
            for (int h = 0; h < 2; ++h) SetHeld(s.hand[size_t(h)], cur[size_t(h)]);
            // Test 5: a respawned stand-in's hand went empty 330 ms after its weapon was given, with nothing logged.
            HSM_LOG("props: stand-in of e%u is %s; hand 0 holds %s, hand 1 %s", e, ObjectName(standIn).c_str(),
                    cur[0] ? ObjectName(cur[0]).c_str() : "nothing", cur[1] ? ObjectName(cur[1]).c_str() : "nothing");
        }
        for (int h = 0; h < 2; ++h) MatchHand(e, standIn, h, cur, nowMs);
    }
}

void PropSync::MatchHand(uint8_t e, UObject* standIn, int h, std::array<UObject*, 2>& cur, double nowMs) {
    StandInHand& sh = standInHands_[e].hand[size_t(h)];
    UObject* prev = sh.held;
    // Left both hands with nothing here letting it go (this code and Equip update or keep what they change; a sheathed
    // weapon keeps its "Parent Actor"): the stand-in dropped it by itself.
    if (prev && prev != cur[0] && prev != cur[1]) {
        if (!adapter_->StillValid(prev, sh.heldIndex)) {
            if (!sh.selfDropLogged)
                HSM_LOG("props: stand-in of e%u: the weapon in hand %d is gone, %.0f ms after the stand-in was bound", e, h,
                        nowMs - standInHands_[e].boundMs);
            sh.selfDropLogged = true;
        } else if (!ObjectField(prev, 0)) {
            SelfDropped(e, standIn, h, sh, cur, nowMs);
            if (env_.handsChanged) env_.handsChanged(e);
        }
    }
    SetHeld(sh, cur[size_t(h)]);

    StandIns::PoseHandView pose;
    sync::HandFacts f;
    f.havePose = standIns_->PoseHandInfo(e, h, pose);
    f.poseClass = pose.classHash;
    f.poseSinceMs = pose.sinceMs;
    f.tokenKnown = env_.equipToken(e, h, f.token);
    f.holding = sh.held != nullptr;
    f.heldClass = sh.heldClass;
    // A prop is let go of quickly (its stream carries it on), unless Equip may be about to sheathe it: the owner shows a
    // slot this hand reaches holding a weapon the stand-in's slot does not have yet (EquipSync's stale-sheathe re-plan
    // needs the pose settled first). Then it waits as long as any other weapon.
    f.heldIsProp = sh.held && byActor_.count(sh.held) && !env_.mayBeSheathe(e, h);
    for (const auto& kv : pending_)
        if (PropOwnerEntity(kv.first) == e && PropHand(kv.first) == h) { f.dropPending = true; break; }
    if (!f.holding && f.havePose && f.poseClass != sync::kHandEmpty && f.poseClass != sync::kHandUnknown) {
        const double age = nowMs - pose.sinceMs;   // only between the two graces does the answer change anything
        if (age >= sync::kAttachGraceMs && age < sync::kDrawGraceMs) f.mayBeDraw = env_.mayBeDraw(e, f.poseClass);
    }
    const sync::HandAct act = sh.match.Step(f, nowMs);
    // A hand a lease change let go of (NoticeDrop) takes nothing until the owner's pose for it moves on: the pose may still
    // show the weapon just dropped, and the hand would pick it straight up again from where it lies.
    if (sh.leaseLetGo) {
        if (!f.havePose || pose.sinceMs != sh.leaseLetGoPoseMs) sh.leaseLetGo = false;
        else if (act == sync::HandAct::Attach) return;
    }
    if (act == sync::HandAct::None) return;
    if (pose.sinceMs != sh.episodeMs) { sh.episodeMs = pose.sinceMs; sh.backoffMs = kRetryMs; sh.retryMs = 0; }
    if (nowMs < sh.retryMs) return;
    sh.retryMs = nowMs + sh.backoffMs;   // also after a success: a hand that keeps undoing it is not redone every frame
    sh.backoffMs = std::min(sh.backoffMs * 2, kRetryMaxMs);
    if (act == sync::HandAct::Attach) {
        if (env_.deadBones(e) & (1u << (h == 0 ? sync::kBoneHandR : sync::kBoneHandL))) return;   // that hand is cut off here
        if (!Attach(e, standIn, h, pose, nowMs)) { ++n_.attachMiss; return; }
        ++n_.attached;
        if (env_.handsChanged) env_.handsChanged(e);
    } else {
        UObject* w = sh.held;
        const DropResult r = DropFromHands(standIn, h, w, ObjectName(w));
        if (r == DropResult::Dropped) {
            if (++n_.matchReleased <= kLogFirst)
                HSM_LOG("props: stand-in of e%u let go of %s from hand %d: the owner's pose no longer shows it there", e,
                        ObjectName(w).c_str(), h);
            LetLoose(e, h, w, nowMs);
            if (env_.handsChanged) env_.handsChanged(e);
        }
    }
    cur = HandsOf(standIn);
    ForgetHeld(e, standIn);
}

// A weapon left the stand-in's hand by itself. While the owner's pose still shows its class in that hand, the hand takes
// it back (at most every sync::kReattachMinMs, never a cut-off hand); otherwise it falls like a dropped weapon.
void PropSync::SelfDropped(uint8_t e, UObject* standIn, int h, StandInHand& sh, std::array<UObject*, 2>& cur, double nowMs) {
    UObject* w = sh.held;
    // The first of every stand-in hand is logged, with how long after its binding (test 5: the respawn's empty hand).
    const bool firstOfHand = !sh.selfDropLogged;
    sh.selfDropLogged = true;
    const bool first = ++n_.selfDrops <= kLogFirst || firstOfHand;
    if (firstOfHand)
        HSM_LOG("props: stand-in of e%u: first weapon to leave hand %d by itself, %.0f ms after the stand-in was bound", e, h,
                nowMs - standInHands_[e].boundMs);
    StandIns::PoseHandView pose;
    const bool shown = sh.heldClass != 0 && standIns_->PoseHandInfo(e, h, pose) && pose.classHash == sh.heldClass;
    const bool handDead = env_.deadBones(e) & (1u << (h == 0 ? sync::kBoneHandR : sync::kBoneHandL));
    if (shown && !cur[size_t(h)] && !handDead && nowMs - sh.reattachMs >= sync::kReattachMinMs) {
        sh.reattachMs = nowMs;
        if (TakeIntoHand(standIn, h, w)) {
            ++n_.reattached;
            if (first) HSM_LOG("props: stand-in of e%u dropped %s from hand %d by itself; taken back", e, ObjectName(w).c_str(), h);
            cur = HandsOf(standIn);
            return;
        }
    }
    if (first)
        HSM_LOG("props: stand-in of e%u dropped %s from hand %d by itself; %s", e, ObjectName(w).c_str(), h,
                shown ? "not taken back: it falls" : "the owner no longer shows it there: it falls");
    LetLoose(e, h, w, nowMs);
}

// "Set Up Right/Left Hand Weapon" with that weapon, as the game's own pick-up does (Willie_BP.cpp:17357): the hand grips
// it and the grip constraint pulls it in; the rig drives it from the next pose on.
bool PropSync::TakeIntoHand(UObject* standIn, int h, UObject* weapon) {
    bool ok = false;
    if (h == 0) {
        Call c(standIn, "Set Up Right Hand Weapon");
        c.set<UObject*>("Weapon R", weapon);
        ok = c.valid() && c.invoke();
    } else {
        Call c(standIn, "Set Up Left Hand Weapon");
        c.set<UObject*>("Weapon L", weapon);
        ok = c.valid() && c.invoke();
    }
    return ok && ObjectField(standIn, h == 0 ? 1 : 2) == weapon && ObjectField(weapon, 0) == standIn;
}

// The owner picked a weapon up: the loose weapon of that class nearest the owner's (sync::PickAttach) goes into the
// stand-in's hand. Loose: no "Parent Actor" (held and sheathed weapons have one), not a body weapon, not being destroyed.
bool PropSync::Attach(uint8_t e, UObject* standIn, int h, const StandIns::PoseHandView& pose, double nowMs) {
    if (nowMs - weaponsMs_ >= kWeaponsRefreshMs) {
        weaponsMs_ = nowMs;
        weapons_.clear();
        for (UObject* w : adapter_->WeaponActors())
            if (!adapter_->IsBodyWeapon(w)) weapons_.push_back(Candidate{w, ObjectIndex(w), ClassHashOf(w)});
    }
    std::vector<sync::LooseWeapon> loose;
    std::vector<UObject*> actors;
    // cls 0: not known yet, read once the actor is known to be live
    auto consider = [&](UObject* a, int32_t index, uint32_t cls) {
        if (!a || (cls && cls != pose.classHash) || std::find(actors.begin(), actors.end(), a) != actors.end()) return;
        if (!adapter_->StillValid(a, index) || (!cls && ClassHashOf(a) != pose.classHash) || ObjectField(a, 0)) return;
        const FVector at = adapter_->ActorLocation(a);
        loose.push_back(sync::LooseWeapon{sync::V3{at.x, at.y, at.z}, pose.classHash});
        actors.push_back(a);
    };
    for (const Loose& l : loose_) consider(l.actor, l.index, 0);
    for (const auto& kv : props_) consider(kv.second.actor, kv.second.index, 0);
    for (const Candidate& c : weapons_) consider(c.actor, c.index, c.classHash);
    const int pick = sync::PickAttach(loose, pose.classHash, pose.pos, pose.firstPos);
    if (pick < 0) {
        if (n_.attachMiss < kLogFirst)
            HSM_LOG("props: stand-in of e%u hand %d: no loose weapon of the owner's class within %.0f uu here (%zu of that class loose)",
                    e, h, sync::kAttachRadiusUu, loose.size());
        return false;
    }
    UObject* w = actors[size_t(pick)];
    if (!TakeIntoHand(standIn, h, w)) {
        HSM_LOG("props: stand-in of e%u did not take %s into hand %d", e, ObjectName(w).c_str(), h);
        return false;
    }
    loose_.erase(std::remove_if(loose_.begin(), loose_.end(), [&](const Loose& l) { return l.actor == w; }), loose_.end());
    if (n_.attached < kLogFirst)
        HSM_LOG("props: stand-in of e%u took %s%s into hand %d: the owner's pose shows it picked one up", e, ObjectName(w).c_str(),
                byActor_.count(w) ? " (a prop)" : "", h);
    return true;
}

// A weapon a stand-in let go of here: it falls and lands (RestorePhysics, re-asserted for kRestoreTicks). A prop goes
// back to its stream; any other weapon waits in loose_ for a drop of that hand to be heard of (Adopt).
void PropSync::LetLoose(uint8_t e, int h, UObject* w, double nowMs) {
    if (auto known = byActor_.find(w); known != byActor_.end()) {
        if (auto p = props_.find(known->second); p != props_.end()) {
            p->second.restoreTicks = kRestoreTicks;
            RestorePhysics(p->second.root, p->second.rootIndex);
            p->second.dirty = view_.Find(known->second) != nullptr;
        }
        return;
    }
    Call rc(w, "K2_GetRootComponent");
    UObject* root = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
    loose_.erase(std::remove_if(loose_.begin(), loose_.end(), [&](const Loose& l) { return l.actor == w; }), loose_.end());
    Loose l;
    l.actor = w;
    l.index = ObjectIndex(w);
    l.root = root;
    l.rootIndex = root ? ObjectIndex(root) : -1;
    l.entity = e;
    l.hand = h;
    l.classHash = ClassHashOf(w);
    l.atMs = nowMs;
    l.restoreTicks = kRestoreTicks;
    RestorePhysics(l.root, l.rootIndex);
    loose_.push_back(l);
}

void PropSync::TickLoose(double nowMs) {
    for (auto it = loose_.begin(); it != loose_.end();) {
        Loose& l = *it;
        if (!adapter_->StillValid(l.actor, l.index) || nowMs - l.atMs >= kLooseKeepMs || byActor_.count(l.actor) || ObjectField(l.actor, 0)) {
            it = loose_.erase(it);   // gone, too old to be a drop on its way, a prop now, or held again
            continue;
        }
        if (l.restoreTicks > 0) { --l.restoreTicks; RestorePhysics(l.root, l.rootIndex); }
        ++it;
    }
}

// A drop whose copy the stand-in here let go of already. With a witness, that weapon if it is loose; without, the one
// let go of from that hand most recently before the drop was heard (and still loose), if it is of the class the
// owner's pose showed in that hand before it changed (a weapon this stand-in let go of earlier is not some other weapon
// the owner dropped since).
bool PropSync::Adopt(uint64_t propId, uint8_t owner, int hand, const PendingDrop& d, double nowMs) {
    const bool witnessed = d.witness != nullptr;
    StandIns::PoseHandView pose;
    if (!witnessed && !standIns_->PoseHandInfo(owner, hand, pose)) return false;
    auto best = loose_.end();
    for (auto it = loose_.begin(); it != loose_.end(); ++it)
        if (it->entity == owner && (witnessed || it->hand == hand) && nowMs - it->atMs < kLooseKeepMs &&
            sync::AdoptCandidate(witnessed, it->actor == d.witness && it->index == d.witnessIndex, it->atMs, d.heardMs) &&
            (best == loose_.end() || it->atMs > best->atMs))
            best = it;
    if (best == loose_.end()) return false;
    if (!witnessed && (best->classHash == 0 || best->classHash != pose.prevClassHash || best->classHash == pose.classHash))
        return false;
    if (!adapter_->StillValid(best->actor, best->index) || ObjectField(best->actor, 0)) return false;
    UObject* w = best->actor;
    const int restoreTicks = best->restoreTicks;
    Prop* p = Track(propId, w);
    if (!p) return false;
    loose_.erase(best);
    p->restoreTicks = restoreTicks;
    p->dirty = view_.Find(propId) != nullptr;
    if (++n_.adopted <= kLogFirst)
        HSM_LOG("props: drop %016llx (entity %u, hand %d) is %s, which the stand-in here had let go of already",
                static_cast<unsigned long long>(propId), owner, hand, ObjectName(w).c_str());
    return true;
}

// ---------------------------------------------------------------- leases

bool PropSync::InUse(uint64_t propId, double nowMs) const {
    if (props_.count(propId) || pending_.count(propId) || view_.Level(propId) != LeaseLevel::Free) return true;
    auto it = heardMs_.find(propId);
    return it != heardMs_.end() && nowMs - it->second < kHeardLiveMs;
}

void PropSync::Claim(uint64_t propId, ClaimSend what, LeaseLevel level, double nowMs) {
    const uint8_t me = MySlot();
    PropLeaseMsg m;
    m.propId = propId;
    m.slot = me;
    if (what == ClaimSend::Request) {
        ++n_.requests;
        if (env_.host) { HostRequest(propId, me, level, nowMs); return; }
        m.action = LeaseAction::Request;
        m.level = level;
    } else if (what == ClaimSend::Release) {
        ++n_.releases;
        if (env_.host) { HostRelease(propId, me); return; }
        m.action = LeaseAction::Release;
        m.level = LeaseLevel::Free;
    } else {
        return;
    }
    if (env_.send) env_.send(Encode(m), true);
}

void PropSync::HostRequest(uint64_t propId, uint8_t slot, LeaseLevel level, double nowMs) {
    const LeaseResult r = table_.Request(propId, slot, level, nowMs);
    if (r.changed) Grant(propId, r.slot, r.level);
}

void PropSync::HostRelease(uint64_t propId, uint8_t slot) {
    if (table_.Release(propId, slot).changed) Grant(propId, kHostSlot, LeaseLevel::Free);
}

void PropSync::Grant(uint64_t propId, uint8_t slot, LeaseLevel level) {
    PropLeaseMsg g;
    g.propId = propId;
    g.action = LeaseAction::Grant;
    g.level = level;
    g.slot = level == LeaseLevel::Free ? kHostSlot : slot;
    ++n_.grants;
    if (env_.send) env_.send(Encode(g), true);
    OnGrant(propId, g.slot, level);
}

// A lease change from the owner of a stand-in that holds this prop's copy is the owner letting go of it (DropNoticed):
// the copy leaves the stand-in's hand now, not once the owner's pose has shown the hand empty and the hand matching's
// holds are over (the last shown class for sync::kReattachMinMs, then the retry backoff -- test 6: seconds late, the copy
// glued to the hand and undriven meanwhile). MatchHand stays as the fallback for a drop with no lease change. The host
// answers changes only, so every change reaches OnGrant, on the host too (Grant applies its own), and a renewal never does.
// OnGrant calls it with the lease as the view had it before the Grant, ahead of its early return for tracked ids.
void PropSync::NoticeDrop(uint64_t propId, LeaseLevel prevLevel, uint8_t prevSlot, LeaseLevel level, uint8_t slot) {
    auto known = props_.find(propId);
    if (known == props_.end() || !standIns_) return;
    UObject* copy = known->second.actor;
    if (!adapter_->StillValid(copy, known->second.index)) return;
    for (uint8_t e = 1; e <= kMaxWillies; ++e) {
        UObject* standIn = standIns_->ActorOf(e);
        if (!standIn) continue;
        for (int h = 0; h < 2; ++h) {
            if (ObjectField(standIn, h == 0 ? 1 : 2) != copy) continue;   // held in both hands: the right's pass lets both go
            if (!DropNoticed(prevLevel, prevSlot, level, slot, OwnerSlotOf(e), true)) return;
            const double nowMs = SteadyNowMs();
            StandIns::PoseHandView pose;
            const bool havePose = standIns_->PoseHandInfo(e, h, pose);   // the episode this hand lets go in, before the pose moves on
            const DropResult r = DropFromHands(standIn, h, copy, ObjectName(copy));
            if (r == DropResult::Dropped) {
                ++n_.leaseReleased;
                HSM_LOG("props: stand-in of e%u let go of %s from hand %d: its owner's lease changed", e, ObjectName(copy).c_str(), h);
                LetLoose(e, h, copy, nowMs);
                StandInHand& sh = standInHands_[e].hand[size_t(h)];
                sh.leaseLetGo = havePose;
                sh.leaseLetGoPoseMs = pose.sinceMs;
                if (env_.handsChanged) env_.handsChanged(e);
                ForgetHeld(e, standIn);
            }
            return;
        }
    }
}

// A drop heard of opens its one resolve window, with what that hand of the stand-in holds now as its witness.
void PropSync::OnGrant(uint64_t propId, uint8_t slot, LeaseLevel level) {
    const LeaseLevel prevLevel = view_.Level(propId);
    const uint8_t prevSlot = view_.Streamer(propId);
    view_.OnGrant(propId, slot, level);
    NoticeDrop(propId, prevLevel, prevSlot, level, level == LeaseLevel::Free ? kHostSlot : slot);
    if (slot == MySlot() || props_.count(propId) || pending_.count(propId) || !DropSignature(propId, level, slot)) return;
    const double now = SteadyNowMs();
    if (auto g = givenUp_.find(propId); g != givenUp_.end()) {
        if (now - g->second < kLooseKeepMs) return;   // given up already: that drop had its window
        givenUp_.erase(g);
    }
    PendingDrop& d = pending_[propId];
    d.heardMs = now;
    d.deadlineMs = now + kResolveWindowMs;
    d.witness = UntrackedInHand(standIns_->ActorOf(PropOwnerEntity(propId)), PropHand(propId));
    d.witnessIndex = d.witness ? ObjectIndex(d.witness) : -1;
}

bool PropSync::OnPropState(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return false;
    PropStateMsg m;
    if (!Decode(d, n, m) || (IsLevelProp(m.propId) && !InCensus(m.propId))) { ++n_.statesRejected; return false; }
    if (env_.host && !table_.Accepts(m, fromSlot)) { ++n_.statesRejected; return false; }
    if (!env_.host && m.lessee == MySlot()) { ++n_.statesRejected; return false; }   // never mine back
    heardMs_[m.propId] = SteadyNowMs();
    if (!view_.Apply(m)) { ++n_.statesStale; return false; }
    ++n_.statesApplied;
    if (auto it = props_.find(m.propId); it != props_.end()) it->second.dirty = true;
    return true;
}

void PropSync::OnPropLease(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return;
    PropLeaseMsg m;
    // A level prop's index must be in the current census, and nobody holds one (HOLD is for weapons in hand).
    if (!Decode(d, n, m) || (IsLevelProp(m.propId) && (!InCensus(m.propId) || m.level == LeaseLevel::Hold))) {
        ++n_.leasesRejected;
        return;
    }
    if (env_.host) {
        if (!LeaseFromPeerOk(m, fromSlot)) { ++n_.leasesRejected; return; }
        if (m.action == LeaseAction::Request) HostRequest(m.propId, m.slot, m.level, SteadyNowMs());
        else HostRelease(m.propId, m.slot);
        return;
    }
    if (m.action != LeaseAction::Grant || fromSlot != kHostSlot) { ++n_.leasesRejected; return; }
    OnGrant(m.propId, m.slot, m.level);
}

void PropSync::SlotLeft(uint8_t slot) {
    if (!active_ || !env_.host) return;
    for (uint64_t id : table_.EntityLeft(slot)) Grant(id, kHostSlot, LeaseLevel::Free);
}

std::vector<std::vector<uint8_t>> PropSync::GrantsForJoiner() const {
    std::vector<std::vector<uint8_t>> out;
    if (!active_ || !env_.host) return out;
    for (const PropLeaseMsg& g : table_.Grants()) out.push_back(Encode(g));
    return out;
}

// ---------------------------------------------------------------- level props (session/hsm_prop_census.h)

std::vector<std::vector<uint8_t>> PropSync::CensusForJoiner() const {
    std::vector<std::vector<uint8_t>> out;
    if (!active_ || !env_.host || !haveSent_) return out;
    for (const PropCensusMsg& m : SplitCensus(censusSeq_, sent_)) out.push_back(Encode(m));
    return out;
}

bool PropSync::OnPropCensus(const uint8_t* d, size_t n) {
    if (!active_ || env_.host) return false;
    PropCensusMsg m;
    if (!Decode(d, n, m)) { ++n_.censusRejected; return false; }
    switch (census_.Add(m)) {
        case CensusAssembler::Result::Rejected: ++n_.censusRejected; return false;
        case CensusAssembler::Result::Complete:   // matched in Tick; from now on only its indices are heard
            ++n_.censuses;
            censusIndex_.clear();
            for (const CensusEntry& e : census_.Census()) censusIndex_.insert(e.index);
            return true;
        default: return true;                     // a part of one still coming, or of one already replaced
    }
}

bool PropSync::InCensus(uint64_t propId) const {
    return IsCanonicalLevelPropId(propId) && censusIndex_.count(LevelPropIndex(propId)) != 0;
}

// The level's physics props here: actors that are no fighter and no weapon, attached to nothing and not being destroyed,
// whose root is a movable primitive that simulates physics (one that does not is counted in notSimulating). One walk over
// every object (the WeaponActors pattern); what each class is, and where its root and mobility lie, is read once.
std::vector<PropSync::LevelActor> PropSync::WalkLevel(size_t& notSimulating) {
    std::vector<LevelActor> out;
    notSimulating = 0;
    static UClass* actorClass = nullptr;
    static UClass* primitiveClass = nullptr;
    if (!actorClass) actorClass = FindClass("Actor");
    if (!primitiveClass) primitiveClass = FindClass("PrimitiveComponent");
    if (!actorClass || !primitiveClass) return out;
    auto offsetOf = [](std::unordered_map<const UClass*, int32_t>& cache, const UClass* cls, int32_t size, auto find) {
        auto it = cache.find(cls);
        if (it == cache.end()) {
            FProperty* p = find();
            it = cache.emplace(cls, p && PropertySize(p) == size ? PropertyOffset(p) : -1).first;
        }
        return it->second;
    };
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o)) continue;
        const UClass* cls = ClassOf(o);
        auto kind = classKind_.find(cls);
        if (kind == classKind_.end())
            kind = classKind_.emplace(cls, uint8_t(IsChildOf(cls, actorClass) && !adapter_->IsWillie(o) && !adapter_->IsWeapon(o))).first;
        if (!kind->second) continue;
        const int32_t rootOff = offsetOf(rootOff_, cls, int32_t(sizeof(UObject*)), [&] { return FindProperty(ClassOf(o), "RootComponent"); });
        if (rootOff < 0) continue;
        UObject* root = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(o) + rootOff);
        if (!root || IsGarbage(root) || !IsA(root, primitiveClass)) continue;
        const int32_t mobilityOff = offsetOf(mobilityOff_, ClassOf(root), 1, [&] { return FindProperty(ClassOf(root), "Mobility"); });
        if (mobilityOff < 0 || *(reinterpret_cast<const uint8_t*>(root) + mobilityOff) != kMovable) continue;
        RootCalls* c = CallsFor(root);
        if (!c) continue;
        c->simulating.put(0, FName{});
        if (!c->simulating.invoke(root) || !c->simulating.take<bool>(1)) { ++notSimulating; continue; }
        if (Adapter::BeingDestroyed(o)) continue;
        Call parent(o, "GetAttachParentActor");
        if (!parent.invoke() || parent.get<UObject*>("ReturnValue")) continue;   // a fighter's armour, a weapon's part
        out.push_back(LevelActor{o, i, ClassHashOf(o), adapter_->ActorLocation(o)});
    }
    return out;
}

// Host: walk, let the book name what it found, track the new ones, forget the gone ones, and send the census when its set
// changed (and the first time, empty or not: a client must hear that too).
void PropSync::HostCensus(double nowMs) {
    censusMs_ = nowMs + kCensusRefreshMs;
    size_t notSimulating = 0;
    const std::vector<LevelActor> walk = WalkLevel(notSimulating);
    std::vector<CensusBook<LevelKey>::Seen> seen;
    seen.reserve(walk.size());
    for (const LevelActor& a : walk)
        seen.push_back({LevelKey{a.actor, a.index}, a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)});
    if (!book_.Update(seen) && haveSent_) return;
    const std::vector<CensusEntry> entries = book_.Entries();
    std::set<uint16_t> live;
    for (const CensusEntry& e : entries) live.insert(e.index);
    size_t gone = 0, added = 0;
    for (uint16_t index : censusIndex_) {
        if (live.count(index)) continue;
        ++gone;
        if (auto p = props_.find(MakeLevelPropId(index)); p != props_.end()) {
            byActor_.erase(p->second.actor);
            props_.erase(p);
        }
    }
    for (const LevelActor& a : walk) {
        uint16_t index = 0;
        if (!book_.Find(LevelKey{a.actor, a.index}, index) || censusIndex_.count(index)) continue;
        ++added;
        if (Prop* p = Track(MakeLevelPropId(index), a.actor)) p->level = true;
    }
    censusIndex_ = std::move(live);
    sent_ = entries;
    ++censusSeq_;
    haveSent_ = true;
    ++n_.censuses;
    for (const PropCensusMsg& m : SplitCensus(censusSeq_, sent_))
        if (env_.send) env_.send(Encode(m), true);
    if (n_.censuses <= kLogFirst)
        HSM_LOG("props: level census %zu physics props (%zu movable not simulating, %zu left out), seq %u (+%zu -%zu)",
                sent_.size(), notSimulating, book_.LeftOut(), censusSeq_, added, gone);
}

// Client: bind the census's indices to this machine's own physics props. A bound index stays bound while the census
// still has it as it was matched and its actor lives; the others are matched (MatchCensus) against the props here that
// are not synced yet. In the first match of a level, a prop here tagged 'Random Delete' that matched nothing is one the
// host's level manager purged (its RandomBool differs per machine): destroyed, so both sides keep the host's set.
void PropSync::ClientCensus(double nowMs) {
    const std::vector<CensusEntry>& census = census_.Census();
    const bool fresh = !matched_ || census_.Seq() != matchedSeq_;
    matched_ = true;
    matchedSeq_ = census_.Seq();
    censusMs_ = nowMs + kCensusRefreshMs;
    std::map<uint16_t, const CensusEntry*> want;
    for (const CensusEntry& e : census) want[e.index] = &e;
    for (auto it = bound_.begin(); it != bound_.end();) {
        const uint64_t id = MakeLevelPropId(it->first);
        const auto w = want.find(it->first);
        const bool same = w != want.end() && w->second->classHash == it->second.classHash && w->second->px == it->second.px &&
                          w->second->py == it->second.py && w->second->pz == it->second.pz;
        auto p = props_.find(id);
        if (same && p != props_.end()) { ++it; continue; }
        if (p != props_.end()) {
            if (w != want.end() && p->second.claim.sent != LeaseLevel::Free) Claim(id, ClaimSend::Release, LeaseLevel::Free, nowMs);
            byActor_.erase(p->second.actor);
            props_.erase(p);
        }
        it = bound_.erase(it);
    }
    size_t notSimulating = 0;
    std::vector<LevelActor> free;   // the physics props here that no census index has yet
    for (LevelActor& a : WalkLevel(notSimulating))
        if (!byActor_.count(a.actor)) free.push_back(a);
    std::vector<CensusEntry> open;
    for (const CensusEntry& e : census)
        if (!bound_.count(e.index)) open.push_back(e);
    std::vector<CensusCandidate> locals;
    locals.reserve(free.size());
    for (const LevelActor& a : free) locals.push_back(CensusCandidate{a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)});
    const std::vector<int> match = MatchCensus(open, locals);
    std::vector<bool> used(free.size(), false);
    size_t bound = 0;
    for (size_t i = 0; i < open.size(); ++i) {
        if (match[i] < 0) continue;
        used[size_t(match[i])] = true;
        const uint64_t id = MakeLevelPropId(open[i].index);
        Prop* p = Track(id, free[size_t(match[i])].actor);
        if (!p) continue;
        p->level = true;
        p->dirty = view_.Find(id) != nullptr;
        bound_[open[i].index] = open[i];
        ++bound;
    }
    misses_ = census.size() - bound_.size();
    const bool purge = !purged_;
    purged_ = true;
    size_t purged = 0, unmatched = 0;
    for (size_t j = 0; j < free.size(); ++j) {
        if (used[j]) continue;
        if (purge && TaggedRandomDelete(free[j].actor)) { adapter_->DestroyActor(free[j].actor); ++purged; }
        else ++unmatched;
    }
    n_.censusPurged += purged;
    if ((fresh && n_.censuses <= kLogFirst) || (!fresh && bound > 0))
        HSM_LOG("props: level census seq %u resolved %zu of %zu (%zu now), purged %zu, miss %zu (%zu here unmatched, %zu movable not simulating)",
                census_.Seq(), bound_.size(), census.size(), bound, purged, misses_, unmatched, notSimulating);
}

// Tagged 'Random Delete' (the tag BP_LevelManager's purge looks for): AActor::ActorHasTag. The FName is made once; if it
// cannot be made, nothing counts as tagged and nothing is purged.
bool PropSync::TaggedRandomDelete(UObject* actor) {
    if (!randomDeleteKnown_) {
        randomDeleteKnown_ = true;
        randomDelete_ = MakeName("Random Delete");
        if (randomDelete_.index == 0 && randomDelete_.number == 0) HSM_LOG("props: the 'Random Delete' tag could not be named; nothing is purged");
    }
    if (randomDelete_.index == 0 && randomDelete_.number == 0) return false;
    Call c(actor, "ActorHasTag");
    c.set<FName>("Tag", randomDelete_);
    return c.invoke() && c.get<bool>("ReturnValue");
}

void PropSync::ForgetEntity(uint8_t entity) {
    if (!active_ || !ValidEntity(entity)) return;
    hands_[entity] = Hands{};
    standInHands_[entity] = StandInHands{};
    for (Loose& l : loose_)
        if (l.entity == entity) l.entity = 0;   // still falls; no drop of that fighter's can be heard of any more
    for (auto it = pending_.begin(); it != pending_.end();)
        it = PropOwnerEntity(it->first) == entity ? pending_.erase(it) : std::next(it);
    for (auto it = givenUp_.begin(); it != givenUp_.end();)
        it = PropOwnerEntity(it->first) == entity ? givenUp_.erase(it) : std::next(it);
    for (auto& [id, p] : props_)
        if (p.toucher == entity) p.toucher = 0;
}

// ---------------------------------------------------------------- contacts

void PropSync::NoteContact(UObject* a, UObject* b) {
    if (!active_ || byActor_.empty() || !a || !b || !env_.ownedEntity) return;
    for (int side = 0; side < 2; ++side) {   // each side that is a prop, touched by the other (itself a prop, if picked up again)
        auto it = byActor_.find(side == 0 ? a : b);
        if (it == byActor_.end()) continue;
        UObject* other = side == 0 ? b : a;
        // What touched it (ContactClaimant): a fighter simulated here, or a weapon in its hand ("Parent Actor" only: a thrown
        // weapon's "Last Parent" is not a touch); a stand-in or its weapon claims nothing; and a prop this machine streams
        // claims what it pushes, credited to whoever touched that prop last -- the pile one hit disturbed has one simulator.
        UObject* fighter = adapter_->IsWillie(other) ? other : adapter_->IsWeapon(other) ? ObjectField(other, 0) : nullptr;
        OtherKind kind = OtherKind::StandIn;
        uint8_t entity = 0;
        bool streamedHere = false;
        if (fighter) {
            entity = env_.ownedEntity(fighter);
            if (entity) kind = adapter_->IsWillie(other) ? OtherKind::OwnedFighter : OtherKind::OwnedWeapon;
        } else if (auto pusher = byActor_.find(other); pusher != byActor_.end()) {
            kind = OtherKind::Prop;
            streamedHere = view_.Streamer(pusher->second) == MySlot();
            if (auto q = props_.find(pusher->second); q != props_.end()) entity = q->second.toucher;
            if (!entity) entity = uint8_t(MySlot() + 1);   // this machine's fighter: the roster slot's entity
        }
        entity = ContactClaimant(entity, kind, streamedHere);
        if (!entity) continue;
        auto p = props_.find(it->second);
        if (p == props_.end()) continue;
        p->second.lastContactMs = SteadyNowMs();
        p->second.toucher = entity;
        if (kind == OtherKind::Prop) ++n_.islands;
        else ++n_.contacts;
    }
}

// A prop's contact with a fighter: the hook of "Deal Complex Damage" asks what struck (game::Claims).
bool PropSync::PropOf(UObject* actor, Touch& out) {
    if (!active_ || !actor) return false;
    auto it = byActor_.find(actor);
    if (it == byActor_.end()) return false;
    auto p = props_.find(it->second);
    if (p == props_.end() || !adapter_->StillValid(p->second.actor, p->second.index) || ObjectField(actor, 0)) return false;
    out = Touch{};
    out.id = p->first;
    out.streamedHere = view_.Streamer(p->first) == MySlot();
    out.toucher = p->second.toucher;
    PropStateMsg s;
    if (out.streamedHere && Read(p->second, s)) out.velocity = sync::V3{s.vx, s.vy, s.vz};
    return true;
}

UObject* PropSync::ActorOf(uint64_t propId) const {
    auto it = props_.find(propId);
    return it != props_.end() && adapter_->StillValid(it->second.actor, it->second.index) ? it->second.actor : nullptr;
}

// ---------------------------------------------------------------- engine access

bool PropSync::Read(Prop& p, PropStateMsg& out) {
    if (p.readTick == tick_) { out = p.read; return p.readOk; }
    p.readTick = tick_;
    p.readOk = false;
    RootCalls* c = adapter_->StillValid(p.root, p.rootIndex) ? CallsFor(p.root) : nullptr;
    if (!c || !c->toWorld.invoke(p.root)) { ++n_.readFailed; return false; }
    const FTransform t = c->toWorld.take<FTransform>(0);
    c->linVel.put(0, FName{});
    const FVector v = c->linVel.invoke(p.root) ? c->linVel.take<FVector>(1) : FVector{};
    c->angVel.put(0, FName{});
    const FVector w = c->angVel.invoke(p.root) ? c->angVel.take<FVector>(1) : FVector{};
    for (double x : {t.translation.x, t.translation.y, t.translation.z})
        if (!std::isfinite(x) || std::fabs(x) >= double(kMaxPropCoord)) { ++n_.readFailed; return false; }
    PropStateMsg s;
    s.px = float(t.translation.x);
    s.py = float(t.translation.y);
    s.pz = float(t.translation.z);
    s.rot = PackQuat(float(t.rotation.x), float(t.rotation.y), float(t.rotation.z), float(t.rotation.w));
    s.vx = Clamped(v.x, kMaxPropSpeed);
    s.vy = Clamped(v.y, kMaxPropSpeed);
    s.vz = Clamped(v.z, kMaxPropSpeed);
    s.wx = Clamped(w.x, kMaxPropSpin);
    s.wy = Clamped(w.y, kMaxPropSpin);
    s.wz = Clamped(w.z, kMaxPropSpin);
    p.read = s;
    p.readOk = true;
    out = s;
    return true;
}

bool PropSync::Drive(Prop& p, const PropStateMsg& s) {
    WorldObject o;
    o.px = s.px; o.py = s.py; o.pz = s.pz;
    o.rot = s.rot;
    o.vx = s.vx; o.vy = s.vy; o.vz = s.vz;
    if (!ApplyWorldObject(*adapter_, p.actor, o)) return false;   // teleport + a weapon's linear velocity (world_sync)
    ++n_.driven;
    RootCalls* c = adapter_->StillValid(p.root, p.rootIndex) ? CallsFor(p.root) : nullptr;
    if (!c) return true;
    c->simulating.put(0, FName{});
    if (c->simulating.invoke(p.root) && c->simulating.take<bool>(1)) {
        c->setAng.put(0, FVector{double(s.wx), double(s.wy), double(s.wz)}).put(1, false).put(2, FName{}).invoke(p.root);
        // ApplyWorldObject sets the linear velocity on a weapon's "BaseMesh"; a level prop's body is its root.
        if (p.level) c->setLin.put(0, FVector{double(s.vx), double(s.vy), double(s.vz)}).put(1, false).put(2, FName{}).invoke(p.root);
    }
    return true;
}

PropSync::RootCalls* PropSync::CallsFor(UObject* root) {
    const UClass* cls = ClassOf(root);
    if (auto it = calls_.find(cls); it != calls_.end()) return it->second.get();
    auto c = std::make_unique<RootCalls>();
    c->toWorld = PreparedCall(cls, "K2_GetComponentToWorld", {"ReturnValue"});
    c->linVel = PreparedCall(cls, "GetPhysicsLinearVelocity", {"BoneName", "ReturnValue"});
    c->angVel = PreparedCall(cls, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    c->setAng = PreparedCall(cls, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    c->setLin = PreparedCall(cls, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    c->simulating = PreparedCall(cls, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
    if (!c->toWorld.valid() || !c->linVel.valid() || !c->angVel.valid() || !c->setAng.valid() || !c->setLin.valid() ||
        !c->simulating.valid()) {
        HSM_LOG("props: %s is not a physics body; props rooted in it are not synced", ObjectName(reinterpret_cast<const UObject*>(cls)).c_str());
        c.reset();
    }
    return (calls_[cls] = std::move(c)).get();
}

UObject* PropSync::ObjectField(UObject* o, int which) {
    if (!o || which < 0 || which > 2) return nullptr;
    std::unordered_map<const UClass*, int32_t>& offs = fieldOff_[size_t(which)];   // per class: props of mixed classes interleave
    const UClass* cls = ClassOf(o);
    auto it = offs.find(cls);
    if (it == offs.end()) {
        FProperty* p = nullptr;
        if (which == 0) p = FindProperty(ClassOf(o), "Parent Actor");
        else if (which == 1) p = FindProperty(ClassOf(o), "Weapon R");
        else p = FindProperty(ClassOf(o), "Weapon L");
        it = offs.emplace(cls, p && PropertySize(p) == int32_t(sizeof(UObject*)) ? PropertyOffset(p) : -1).first;
    }
    const int32_t off = it->second;
    return off >= 0 ? *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(o) + off) : nullptr;
}

// ---------------------------------------------------------------- health

std::string PropSync::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    size_t level = 0;
    for (const auto& kv : props_) level += kv.second.level ? 1 : 0;
    char line[1536];
    std::snprintf(line, sizeof line,
                  "props: live %zu pending %zu loose %zu | drops %llu minted %llu mintFailed %llu | resolved %llu forced %llu kept %llu "
                  "unresolved %llu adopted %llu releaseSkipped %llu releaseFailed %llu | requests %llu releases %llu holdDenied %llu contacts %llu islands %llu | "
                  "grants %llu lapsed %llu leasesRejected %llu | statesSent %llu statesApplied %llu statesStale %llu "
                  "statesRejected %llu driven %llu readFailed %llu lost %llu | hands: attached %llu attachMiss %llu "
                  "released %llu selfDrops %llu reattached %llu leaseReleased %llu | level %zu of %zu streamed %llu driven %llu deferred %llu miss %zu "
                  "leftOut %zu purged %llu censuses %llu censusRejected %llu",
                  props_.size(), pending_.size(), loose_.size(), d(a.drops, b.drops), d(a.minted, b.minted), d(a.mintFailed, b.mintFailed),
                  d(a.resolved, b.resolved), d(a.forced, b.forced), d(a.kept, b.kept), d(a.unresolved, b.unresolved), d(a.adopted, b.adopted), d(a.releaseSkipped, b.releaseSkipped),
                  d(a.releaseFailed, b.releaseFailed), d(a.requests, b.requests), d(a.releases, b.releases),
                  d(a.holdDenied, b.holdDenied), d(a.contacts, b.contacts), d(a.islands, b.islands), d(a.grants, b.grants), d(a.lapsed, b.lapsed),
                  d(a.leasesRejected, b.leasesRejected), d(a.statesSent, b.statesSent), d(a.statesApplied, b.statesApplied),
                  d(a.statesStale, b.statesStale), d(a.statesRejected, b.statesRejected), d(a.driven, b.driven),
                  d(a.readFailed, b.readFailed), d(a.lost, b.lost), d(a.attached, b.attached), d(a.attachMiss, b.attachMiss),
                  d(a.matchReleased, b.matchReleased), d(a.selfDrops, b.selfDrops), d(a.reattached, b.reattached), d(a.leaseReleased, b.leaseReleased), level,
                  censusIndex_.size(), d(a.levelStreamed, b.levelStreamed), d(a.levelDriven, b.levelDriven),
                  d(a.levelDeferred, b.levelDeferred), misses_, book_.LeftOut(), d(a.censusPurged, b.censusPurged),
                  d(a.censuses, b.censuses), d(a.censusRejected, b.censusRejected));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game

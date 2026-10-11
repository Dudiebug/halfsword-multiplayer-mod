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
// is kept for the weapon's life: a pick-up (HOLD) and a later drop by anyone reuse it. Level props are left out on
// purpose: the only candidate identity is the actor's name, and nothing verified that a level-baked prop has the same name on every machine (the
// game spawns most loose items at runtime, whose names carry per-process counters), so they stay local physics.
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
constexpr uint8_t kWorldStatic = 0, kWorldDynamic = 1, kBlock = 2;   // ECollisionChannel / ECollisionResponse

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
    HSM_LOG("props: on (%s): dropped weapons are leased FREE/TOUCH/HOLD; level props are not synced",
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
    heardMs_.clear();
    hands_ = {};
    calls_.clear();
    fieldOff_ = {};
}

// ---------------------------------------------------------------- per frame

void PropSync::Tick(double nowMs, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
    if (!active_ || !adapter_) return;
    ++tick_;
    if (env_.host)
        for (uint64_t id : table_.Tick(nowMs)) { ++n_.lapsed; Grant(id, kHostSlot, LeaseLevel::Free); }
    DetectDrops(nowMs, owned);
    if (!pending_.empty()) ResolvePending(nowMs);
    const uint8_t me = MySlot();
    for (auto it = props_.begin(); it != props_.end();) {
        const uint64_t id = it->first;
        Prop& p = it->second;
        if (!adapter_->StillValid(p.actor, p.index)) {   // destroyed here (a respawn takes what its fighter held)
            ++n_.lost;
            if (p.claim.sent != LeaseLevel::Free) Claim(id, ClaimSend::Release, LeaseLevel::Free, nowMs);
            byActor_.erase(p.actor);
            it = props_.erase(it);
            continue;
        }
        UObject* parent = ObjectField(p.actor, 0);
        const bool holding = parent && env_.ownedEntity && env_.ownedEntity(parent) != 0;
        if (p.restoreTicks > 0) { --p.restoreTicks; if (!parent) RestorePhysics(p); }

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
                PropStateMsg s;
                if (Read(p, s) && PropStreamDue(level, p.lastSentMs, PropAtRest(s), nowMs)) {
                    s.propId = id;
                    s.lessee = me;
                    s.senderTimeMs = uint32_t(uint64_t(nowMs));
                    if (env_.send) env_.send(Encode(s), false);
                    p.lastSentMs = nowMs;
                    ++n_.statesSent;
                }
            }
        } else if (p.dirty) {
            p.dirty = false;
            const bool awaitingGrant = p.claim.sent != LeaseLevel::Free && level == LeaseLevel::Free;
            if (!parent && !awaitingGrant)
                if (const PropStateMsg* s = view_.Find(id)) Drive(p, *s);
        }
        ++it;
    }
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
                if (!adapter_->StillValid(prev, h.index[hand]) || !adapter_->IsWeapon(prev) || ObjectField(prev, 0)) continue;
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

// Drops heard of (a Touch Grant on an id this machine does not know, from the machine that owns the id's fighter):
// the weapon is still in the hand of the stand-in here, until the owner's pose shows that hand empty.
void PropSync::ResolvePending(double nowMs) {
    for (auto it = pending_.begin(); it != pending_.end();) {
        const uint64_t id = it->first;
        bool done = props_.count(id) != 0;
        const uint8_t owner = PropOwnerEntity(id);
        const int hand = PropHand(id);
        if (UObject* standIn = done ? nullptr : standIns_->ActorOf(owner)) {
            UObject* copy = ObjectField(standIn, hand == 0 ? 1 : 2);
            if (copy && !byActor_.count(copy) && adapter_->IsWeapon(copy) && ObjectField(copy, 0) == standIn) {
                uint32_t poseClass = 0;
                const bool havePose = standIns_->PoseHand(owner, hand, poseClass);
                const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(copy)));
                if (ResolveDropNow(havePose, poseClass, sync::WeaponClassHash(cls.c_str()))) {
                    done = true;
                    if (ReleaseFromStandIn(id, standIn, hand, copy)) ++n_.resolved;
                }
            }
        }
        if (!done && nowMs >= it->second) {
            done = true;
            ++n_.unresolved;
            HSM_LOG("props: drop %016llx (entity %u, hand %d) not found on its stand-in; that weapon stays local here",
                    static_cast<unsigned long long>(id), owner, hand);
        }
        it = done ? pending_.erase(it) : std::next(it);
    }
}

// The game's own drop on the stand-in: "Set Up ... Hand Weapon" with no class detaches the weapon in that hand
// ("Dropped With no Damage": no blow), clears the hand and the weapon's "Parent Actor". With the grip type at 0 the
// same function destroys the weapon instead, so that case is left alone. Not under ScriptHookBypass: the claims
// hooks keep guarding anything it might reach.
bool PropSync::ReleaseFromStandIn(uint64_t propId, UObject* standIn, int hand, UObject* copy) {
    const int32_t copyIndex = ObjectIndex(copy);
    for (int pass = 0; pass < 2; ++pass) {
        const int h = pass == 0 ? hand : 1 - hand;
        if (pass == 1 && ObjectField(standIn, h == 0 ? 1 : 2) != copy) break;   // held in both hands: let the other go too
        uint8_t* grip = h == 0 ? Field<uint8_t>(standIn, "R_GripType_Current") : Field<uint8_t>(standIn, "L_GripType_Current");
        if (!grip || *grip == 0) {
            ++n_.releaseSkipped;
            HSM_LOG("props: %s hand %d grip type unreadable or 0: not dropping %016llx there (the game would destroy it)",
                    ObjectName(standIn).c_str(), h, static_cast<unsigned long long>(propId));
            return false;
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
        HSM_LOG("props: %s did not let go of %016llx; that weapon stays with the stand-in here", ObjectName(standIn).c_str(),
                static_cast<unsigned long long>(propId));
        return false;
    }
    Prop* p = Track(propId, copy);
    if (!p) return false;
    p->restoreTicks = kRestoreTicks;
    RestorePhysics(*p);
    p->dirty = view_.Find(propId) != nullptr;
    return true;
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
void PropSync::RestorePhysics(Prop& p) {
    if (!adapter_->StillValid(p.root, p.rootIndex)) return;
    Call(p.root, "SetEnableGravity").set<bool>("bGravityEnabled", true).invoke();
    Call(p.root, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldStatic).set<uint8_t>("NewResponse", kBlock).invoke();
    Call(p.root, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldDynamic).set<uint8_t>("NewResponse", kBlock).invoke();
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

void PropSync::OnGrant(uint64_t propId, uint8_t slot, LeaseLevel level) {
    view_.OnGrant(propId, slot, level);
    if (slot != MySlot() && !props_.count(propId) && !pending_.count(propId) && DropSignature(propId, level, slot))
        pending_[propId] = SteadyNowMs() + kResolveWindowMs;
}

bool PropSync::OnPropState(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return false;
    PropStateMsg m;
    if (!Decode(d, n, m) || IsLevelProp(m.propId)) { ++n_.statesRejected; return false; }
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
    if (!Decode(d, n, m) || IsLevelProp(m.propId)) { ++n_.leasesRejected; return; }
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

void PropSync::ForgetEntity(uint8_t entity) {
    if (!active_ || !ValidEntity(entity)) return;
    hands_[entity] = Hands{};
    for (auto it = pending_.begin(); it != pending_.end();)
        it = PropOwnerEntity(it->first) == entity ? pending_.erase(it) : std::next(it);
}

// ---------------------------------------------------------------- contacts

void PropSync::NoteContact(UObject* a, UObject* b) {
    if (!active_ || byActor_.empty() || !a || !b || !env_.ownedEntity) return;
    for (int side = 0; side < 2; ++side) {   // each side that is a prop, touched by the other (itself a prop, if picked up again)
        auto it = byActor_.find(side == 0 ? a : b);
        if (it == byActor_.end()) continue;
        UObject* other = side == 0 ? b : a;
        // A fighter simulated here, or a weapon in its hand ("Parent Actor" only: a thrown weapon's "Last Parent" is not a touch).
        UObject* fighter = adapter_->IsWillie(other) ? other : adapter_->IsWeapon(other) ? ObjectField(other, 0) : nullptr;
        if (!fighter || !env_.ownedEntity(fighter)) continue;
        auto p = props_.find(it->second);
        if (p == props_.end()) continue;
        p->second.lastContactMs = SteadyNowMs();
        ++n_.contacts;
    }
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

void PropSync::Drive(Prop& p, const PropStateMsg& s) {
    WorldObject o;
    o.px = s.px; o.py = s.py; o.pz = s.pz;
    o.rot = s.rot;
    o.vx = s.vx; o.vy = s.vy; o.vz = s.vz;
    if (!ApplyWorldObject(*adapter_, p.actor, o)) return;   // teleport + linear velocity (world_sync)
    ++n_.driven;
    RootCalls* c = adapter_->StillValid(p.root, p.rootIndex) ? CallsFor(p.root) : nullptr;
    if (!c) return;
    c->simulating.put(0, FName{});
    if (c->simulating.invoke(p.root) && c->simulating.take<bool>(1))
        c->setAng.put(0, FVector{double(s.wx), double(s.wy), double(s.wz)}).put(1, false).put(2, FName{}).invoke(p.root);
}

PropSync::RootCalls* PropSync::CallsFor(UObject* root) {
    const UClass* cls = ClassOf(root);
    if (auto it = calls_.find(cls); it != calls_.end()) return it->second.get();
    auto c = std::make_unique<RootCalls>();
    c->toWorld = PreparedCall(cls, "K2_GetComponentToWorld", {"ReturnValue"});
    c->linVel = PreparedCall(cls, "GetPhysicsLinearVelocity", {"BoneName", "ReturnValue"});
    c->angVel = PreparedCall(cls, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    c->setAng = PreparedCall(cls, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    c->simulating = PreparedCall(cls, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
    if (!c->toWorld.valid() || !c->linVel.valid() || !c->angVel.valid() || !c->setAng.valid() || !c->simulating.valid()) {
        HSM_LOG("props: %s is not a physics body; weapons rooted in it are not synced", ObjectName(reinterpret_cast<const UObject*>(cls)).c_str());
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
    char line[768];
    std::snprintf(line, sizeof line,
                  "props: live %zu pending %zu | drops %llu minted %llu mintFailed %llu | resolved %llu unresolved %llu "
                  "releaseSkipped %llu releaseFailed %llu | requests %llu releases %llu holdDenied %llu contacts %llu | "
                  "grants %llu lapsed %llu leasesRejected %llu | statesSent %llu statesApplied %llu statesStale %llu "
                  "statesRejected %llu driven %llu readFailed %llu lost %llu",
                  props_.size(), pending_.size(), d(a.drops, b.drops), d(a.minted, b.minted), d(a.mintFailed, b.mintFailed),
                  d(a.resolved, b.resolved), d(a.unresolved, b.unresolved), d(a.releaseSkipped, b.releaseSkipped),
                  d(a.releaseFailed, b.releaseFailed), d(a.requests, b.requests), d(a.releases, b.releases),
                  d(a.holdDenied, b.holdDenied), d(a.contacts, b.contacts), d(a.grants, b.grants), d(a.lapsed, b.lapsed),
                  d(a.leasesRejected, b.leasesRejected), d(a.statesSent, b.statesSent), d(a.statesApplied, b.statesApplied),
                  d(a.statesStale, b.statesStale), d(a.statesRejected, b.statesRejected), d(a.driven, b.driven),
                  d(a.readFailed, b.readFailed), d(a.lost, b.lost));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game

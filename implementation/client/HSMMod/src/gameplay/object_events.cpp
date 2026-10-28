#include "gameplay/object_sync.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

// Spawned objects and object events (plan M0; session/hsm_object.h, hsm_object_event.h). The members of the object class that
// name what appears mid-match and replay what happens to it. The shape:
//   The host walks (HostCensus, every kCensusRefreshMs and sooner on NoteSpawn) and gives an object nobody names a canonical
//   id (ObjectReconciler::SpawnLocal) and announces it. A client walks the same way (Recheck) and PROPOSES what it finds
//   moving that nothing names, under a provisional id it mints; nothing streams or claims under that id. The host answers
//   with the canonical announce, which re-keys the proposer's entry (Rekey) and lets every other machine bind its own actor
//   by class and place (BindAnnounced), or keep a ghost until it can.
//   Whoever streams an object checks its joints, its detached bodies and its parent every kEventPollMs (PollEvents) and sends
//   an ObjectEvent; every machine replays it by index (ReplayNow), idempotently, queuing an event for an object not found yet.

namespace hsm::game {
using namespace hsm::ue;
using namespace hsm::game::hands;

namespace {
bool PlaceOk(const FVector& at) {
    return CensusPlaceOk(float(at.x), float(at.y), float(at.z));
}
}  // namespace

void ObjectSync::NoteSpawn() {
    if (!active_) return;
    const double now = SteadyNowMs();
    if (now - hintMs_ < kHintGapMs) return;
    hintMs_ = now;
    if (env_.host) { if (haveSent_) censusMs_ = std::min(censusMs_, now + kHintGapMs); }
    else if (matched_) recheckMs_ = std::min(recheckMs_, now + kHintGapMs);
}

uint64_t ObjectSync::MintSpawned() {
    if (minter_.Slot() != MySlot()) minter_ = SpawnedIdMinter(MySlot());
    return minter_.Mint();
}

bool ObjectSync::CompMoving(UObject* comp) {
    RootCalls* c = comp && !IsGarbage(comp) ? CallsFor(comp) : nullptr;
    if (!c) return false;
    c->linVel.put(0, FName{});
    if (!c->linVel.invoke(comp)) return false;
    const FVector v = c->linVel.take<FVector>(1);
    return v.x * v.x + v.y * v.y + v.z * v.z > double(kRestSpeed) * double(kRestSpeed);
}

// ---------------------------------------------------------------- the re-check

// Client: the same pass the host's census makes, for what is not named. Runs on the census cadence, brought forward by
// NoteSpawn and by a canonical announce with no actor here yet.
void ObjectSync::Recheck(double nowMs) {
    recheckMs_ = nowMs + kCensusRefreshMs;
    ++n_.rechecks;
    size_t notSimulating = 0;
    std::vector<LevelActor> free;
    for (LevelActor& a : WalkLevel(notSimulating))
        if (!byActor_.count(a.actor)) free.push_back(a);
    AfterWalk(free, nowMs);
}

// Client: free are the physics objects here that nothing names yet. Pending announces bind to them first; what is left and
// is on the move, touched by a fighter simulated here, or hinted (armour off, a spawn this machine caused) is proposed.
void ObjectSync::AfterWalk(std::vector<LevelActor>& free, double nowMs) {
    if (env_.host) return;
    BindAnnounced(free, nowMs);
    for (auto it = refused_.begin(); it != refused_.end();) it = it->second <= nowMs ? refused_.erase(it) : std::next(it);
    for (auto it = touched_.begin(); it != touched_.end();) it = (it->second < 0 ? touched_.size() > 128 : nowMs - it->second > 60000.0) ? touched_.erase(it) : std::next(it);
    const bool hinted = nowMs - hintMs_ < 2000.0;
    std::set<const UObject*> left;
    size_t proposals = 0;
    for (LevelActor& a : free) {
        if (byActor_.count(a.actor)) continue;
        left.insert(a.actor);
        if (proposals >= kProposalsPerPass) continue;
        if (auto rf = refused_.find(a.actor); rf != refused_.end() && nowMs < rf->second) continue;
        const auto t = touched_.find(a.actor);
        const bool touchedNow = t != touched_.end() && t->second > 0 && nowMs - t->second < 15000.0;
        if (!touchedNow && !((hinted || leftBefore_.count(a.actor)) && CompMoving(a.simComp))) continue;
        ProposeLocal(a, nowMs);
        ++proposals;
    }
    leftBefore_ = std::move(left);
}

void ObjectSync::ProposeLocal(const LevelActor& a, double nowMs) {
    if (!a.classHash || !PlaceOk(a.at)) return;
    const uint64_t prov = MintSpawned();
    if (!prov) return;
    Prop* p = Track(prov, a.actor);
    if (!p) { refused_[a.actor] = nowMs + kRefusedKeepMs; return; }
    p->provisional = true;
    p->spawned = true;
    p->provisionalMs = nowMs;
    if (auto t = touched_.find(a.actor); t != touched_.end() && t->second > 0) {
        p->lastContactMs = t->second;
        p->toucher = uint8_t(MySlot() + 1);   // this machine's fighter: the roster slot's entity
    }
    ObjectAnnounceMsg m;
    m.objId = prov;
    m.flags = kAnnounceProposal;
    m.classHash = a.classHash;
    m.px = float(a.at.x);
    m.py = float(a.at.y);
    m.pz = float(a.at.z);
    m.bodyCount = uint8_t(std::clamp<size_t>(p->bodies.size(), 1, kMaxAnnounceBodies));
    if (env_.send) env_.send(Encode(m), true);
    ++n_.proposed;
    if (n_.proposed <= kLogFirst)
        HSM_LOG("props: proposed %s as %016llx (%u bodies)", ObjectName(a.actor).c_str(), static_cast<unsigned long long>(prov), unsigned(m.bodyCount));
}

// Host: an object nobody names yet is new. It gets a canonical id in the host's slot and everyone hears of it.
void ObjectSync::HostSpawn(const LevelActor& a, double nowMs) {
    if (!a.classHash || !PlaceOk(a.at)) return;
    const uint64_t id = reconciler_.SpawnLocal(a.classHash, float(a.at.x), float(a.at.y), float(a.at.z), std::max<uint8_t>(1, a.bodies));
    if (!id) return;
    Prop* p = Track(id, a.actor);
    if (!p) { reconciler_.Remove(id); return; }
    p->spawned = true;
    if (auto t = touched_.find(a.actor); t != touched_.end() && t->second > 0) { p->lastContactMs = t->second; p->toucher = uint8_t(kHostSlot + 1); }
    ObjectAnnounceMsg m;
    if (reconciler_.AnnounceOf(id, m) && env_.send) env_.send(Encode(m), true);
    ++n_.announced;
    if (n_.announced <= kLogFirst)
        HSM_LOG("props: announced %s as %016llx (%zu bodies)", ObjectName(a.actor).c_str(), static_cast<unsigned long long>(id), p->bodies.size());
    (void)nowMs;
}

// ---------------------------------------------------------------- announces

void ObjectSync::OnObjectAnnounce(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return;
    ObjectAnnounceMsg m;
    if (!Decode(d, n, m)) { ++n_.announceRejected; return; }
    const double now = SteadyNowMs();
    if (env_.host) {
        if (m.flags != kAnnounceProposal || fromSlot == kHostSlot || fromSlot >= kMaxFighters) { ++n_.announceRejected; return; }
        HostPropose(m, fromSlot, now);
    } else {
        if (m.flags != kAnnounceCanonical || fromSlot != kHostSlot) { ++n_.announceRejected; return; }
        ClientAnnounce(m, now);
    }
}

// Host: a proposal. An object the host already tracks here (a census prop, or a spawned object) of that class near that
// place is the same object and answers it; otherwise the reconciler names it, and the host looks for its own actor.
void ObjectSync::HostPropose(const ObjectAnnounceMsg& m, uint8_t fromSlot, double nowMs) {
    if (!ValidSpawnedId(m.objId) || SpawnedIdSlot(m.objId) != fromSlot) { ++n_.announceRejected; return; }
    uint64_t existing = 0;
    double best = double(kCensusMatchUu) * double(kCensusMatchUu);
    for (const auto& [id, p] : props_) {
        if (IsWeaponId(id) || p.provisional || p.sub || p.classHash != m.classHash || !adapter_->StillValid(p.actor, p.index)) continue;
        const FVector at = adapter_->ActorLocation(p.actor);
        const double dx = at.x - m.px, dy = at.y - m.py, dz = at.z - m.pz;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= best) { best = d2; existing = id; }
    }
    ObjectAnnounceMsg out;
    if (existing) {
        out = m;
        out.objId = existing;
        out.flags = kAnnounceCanonical;
        out.proposalId = m.objId;
        const FVector at = adapter_->ActorLocation(props_[existing].actor);
        out.px = float(at.x);
        out.py = float(at.y);
        out.pz = float(at.z);
    } else {
        const ObjectReconciler::Result r = reconciler_.Propose(m, fromSlot);
        if (!r.objId) { ++n_.announceRejected; return; }
        out = r.announce;
        if (r.created) {   // the host's own actor for it, if it has one; else a ghost until it does
            announced_[r.objId] = Announced{out, nowMs};
            size_t ns = 0;
            std::vector<LevelActor> free;
            for (LevelActor& a : WalkLevel(ns))
                if (!byActor_.count(a.actor)) free.push_back(a);
            BindAnnounced(free, nowMs);
        }
    }
    if (env_.send) env_.send(Encode(out), true);
    ++n_.announced;
}

// Client: the host's canonical announce. It answers one of my proposals (re-key), or names an object I bind to my own actor
// of that class and place, now or on a later re-check.
void ObjectSync::ClientAnnounce(const ObjectAnnounceMsg& m, double nowMs) {
    minter_.Observe(m.objId);
    minter_.Observe(m.proposalId);
    if (destroyed_.count(m.objId)) return;
    if (m.proposalId && Rekey(m.proposalId, m.objId)) { Bound(m.objId, nowMs); return; }
    if (props_.count(m.objId) || announced_.count(m.objId)) return;
    if (announced_.size() >= kMaxKnownObjects) { ++n_.announceRejected; return; }
    announced_[m.objId] = Announced{m, nowMs};
    ++n_.announced;
    if (matched_) recheckMs_ = std::min(recheckMs_, nowMs + kHintGapMs);   // look for the actor now
}

// My provisional entry becomes the canonical object. If the canonical id is already bound here to another actor the
// provisional entry is dropped (and its actor is not proposed again for a while).
bool ObjectSync::Rekey(uint64_t provisional, uint64_t canonical) {
    auto it = props_.find(provisional);
    if (it == props_.end() || !it->second.provisional) return false;
    if (props_.count(canonical)) {
        refused_[it->second.actor] = SteadyNowMs() + kRefusedKeepMs;
        byActor_.erase(it->second.actor);
        props_.erase(it);
        return true;
    }
    auto node = props_.extract(it);
    node.key() = canonical;
    Prop& p = props_.insert(std::move(node)).position->second;
    p.provisional = false;
    p.spawned = IsSpawnedId(canonical);
    p.level = IsLevelProp(canonical);
    p.claim = LeaseClaim{};
    p.lastSentMs = -1e18;
    byActor_[p.actor] = canonical;
    ++n_.rekeyed;
    if (n_.rekeyed <= kLogFirst)
        HSM_LOG("props: %016llx is now %016llx", static_cast<unsigned long long>(provisional), static_cast<unsigned long long>(canonical));
    return true;
}

// Match the pending announces to the actors in free (class and place; where the object was last heard of if it has been
// streamed since). Bound actors leave free. An announce nothing matched keeps a ghost (GhostTable) until it expires.
void ObjectSync::BindAnnounced(std::vector<LevelActor>& free, double nowMs) {
    for (uint64_t id : ghosts_.Expire(nowMs))
        if (announced_.erase(id)) {
            ++n_.ghostsExpired;
            HSM_LOG("props: no actor here for announced object %016llx after %.0f s; it is not synced here", static_cast<unsigned long long>(id), kGhostTtlMs / 1000.0);
        }
    if (announced_.empty() || free.empty()) {
        for (auto& [id, a] : announced_)
            if (!ghosts_.IsGhost(id)) ghosts_.Note(id, nowMs);
        return;
    }
    std::vector<uint64_t> ids;
    std::vector<CensusEntry> open;
    for (const auto& [id, a] : announced_) {
        CensusEntry e;
        e.index = uint16_t(ids.size());
        e.classHash = a.msg.classHash;
        e.px = a.msg.px;
        e.py = a.msg.py;
        e.pz = a.msg.pz;
        if (const BodyStateMsg* s = view_.Find(id, 0)) { e.px = s->px; e.py = s->py; e.pz = s->pz; }   // where it is now, not where it appeared
        ids.push_back(id);
        open.push_back(e);
    }
    std::vector<CensusCandidate> locals;
    locals.reserve(free.size());
    for (const LevelActor& a : free) locals.push_back(CensusCandidate{a.classHash, float(a.at.x), float(a.at.y), float(a.at.z)});
    const std::vector<int> match = MatchCensus(open, locals);
    std::vector<bool> used(free.size(), false);
    for (size_t i = 0; i < open.size(); ++i) {
        const uint64_t id = ids[i];
        if (match[i] < 0) {
            if (!ghosts_.IsGhost(id)) ghosts_.Note(id, nowMs);
            continue;
        }
        Prop* p = Track(id, free[size_t(match[i])].actor);
        if (!p) continue;
        used[size_t(match[i])] = true;
        p->spawned = IsSpawnedId(id);
        ghosts_.Resolve(id);
        announced_.erase(id);
        ++n_.bound;
        Bound(id, nowMs);
    }
    std::vector<LevelActor> rest;
    for (size_t j = 0; j < free.size(); ++j)
        if (!used[j]) rest.push_back(free[j]);
    free = std::move(rest);
}

// An object got its actor here: the state heard of it is driven once, and the events that waited for it replay.
void ObjectSync::Bound(uint64_t id, double nowMs) {
    auto it = props_.find(id);
    if (it == props_.end()) return;
    it->second.dirty = view_.Find(id) != nullptr;
    for (size_t i = 0; i < queuedEvents_.size();) {
        if (queuedEvents_[i].msg.objId == id && ReplayNow(queuedEvents_[i].msg, nowMs)) queuedEvents_.erase(queuedEvents_.begin() + long(i));
        else ++i;
    }
}

std::vector<std::vector<uint8_t>> ObjectSync::ObjectsForJoiner() const {
    std::vector<std::vector<uint8_t>> out;
    if (!active_ || !env_.host) return out;
    for (const ObjectAnnounceMsg& a : reconciler_.AnnounceAll()) out.push_back(Encode(a));
    for (const std::vector<uint8_t>& h : history_) out.push_back(h);
    return out;
}

// ---------------------------------------------------------------- events

void ObjectSync::SendEvent(const ObjectEventMsg& m) {
    std::vector<uint8_t> bytes = Encode(m);
    if (env_.send) env_.send(bytes, true);
    ++n_.eventsSent;
    if (env_.host && m.kind != ObjectEventKind::Custom) {
        if (history_.size() >= kMaxHistory) history_.erase(history_.begin());
        history_.push_back(std::move(bytes));
    }
}

// The objects I stream: a joint that broke, a body that left its parent, an object that was attached to another or worn.
void ObjectSync::PollEvents(double nowMs) {
    for (auto it = queuedEvents_.begin(); it != queuedEvents_.end();)
        it = nowMs - it->atMs > kEventKeepMs ? queuedEvents_.erase(it) : std::next(it);
    const uint8_t me = MySlot();
    std::vector<uint64_t> gone;
    for (auto& [id, p] : props_) {
        if (IsWeaponId(id) || p.provisional || p.sub || !adapter_->StillValid(p.actor, p.index)) continue;
        if (view_.Streamer(id) != me) continue;
        for (size_t ci = 0; ci < p.constraints.size(); ++ci) {
            const uint64_t bit = uint64_t(1) << ci;
            const ConstraintRef& c = p.constraints[ci];
            if ((p.brokenMask & bit) || !adapter_->StillValid(c.comp, c.index)) continue;
            Call ib(c.comp, "IsBroken");
            if (!ib.invoke() || !ib.get<bool>("ReturnValue")) continue;
            p.brokenMask |= bit;
            ObjectEventMsg e;
            e.objId = id;
            e.kind = ObjectEventKind::JointBroke;
            e.constraint = uint8_t(ci);
            SendEvent(e);
        }
        for (size_t b = 1; b < p.bodies.size(); ++b) {
            const uint64_t bit = uint64_t(1) << b;
            BodyRef& r = p.bodies[b];
            if (!(p.attachedMask & bit) || r.detached || !adapter_->StillValid(r.comp, r.index)) continue;
            Call ap(r.comp, "GetAttachParent");
            if (!ap.invoke() || ap.get<UObject*>("ReturnValue")) continue;
            const uint64_t other = MintSpawned();
            if (!other) continue;
            p.attachedMask &= ~bit;
            r.detached = true;
            ObjectEventMsg e;
            e.objId = id;
            e.body = uint8_t(b);
            e.kind = ObjectEventKind::Detached;
            e.otherId = other;
            if (env_.host) detached_.insert(other);
            SendEvent(e);
            TrackSub(other, p, b, nowMs, true);
            ++n_.bodiesDetached;
        }
        if (p.attachedSent || !(p.spawned || p.level)) continue;
        UObject* parent = ParentActorOf(p.actor);
        if (!parent) continue;
        if (auto q = byActor_.find(parent); q != byActor_.end() && q->second != id) {   // put on another tracked object
            p.attachedSent = true;
            ObjectEventMsg e;
            e.objId = id;
            e.kind = ObjectEventKind::Attached;
            e.otherId = q->second;
            SendEvent(e);
        } else if (p.spawned && adapter_->IsWillie(parent)) {   // picked up and worn (armour): gone from the floor everywhere
            ObjectEventMsg e;
            e.objId = id;
            e.kind = ObjectEventKind::Destroyed;
            SendEvent(e);
            ++n_.destroyedSent;
            gone.push_back(id);
        }
    }
    for (uint64_t id : gone) {
        destroyed_.insert(id);
        if (env_.host) reconciler_.Remove(id);
        if (auto it = props_.find(id); it != props_.end()) { byActor_.erase(it->second.actor); props_.erase(it); }
    }
}

bool ObjectSync::OnObjectEvent(const uint8_t* d, size_t n, uint8_t fromSlot) {
    if (!active_) return false;
    ObjectEventMsg m;
    if (!Decode(d, n, m) || (IsLevelProp(m.objId) && !InCensus(m.objId))) { ++n_.eventsRejected; return false; }
    const double now = SteadyNowMs();
    if (env_.host) {
        if (fromSlot == kHostSlot || fromSlot >= kMaxFighters) { ++n_.eventsRejected; return false; }
        // Only the object's streamer speaks for it, and only about objects the host knows.
        if (m.kind != ObjectEventKind::Custom && table_.Streamer(m.objId) != fromSlot) { ++n_.eventsRejected; return false; }
        if (IsSpawnedId(m.objId) && !reconciler_.Knows(m.objId) && !detached_.count(m.objId)) { ++n_.eventsRejected; return false; }
        if (m.kind == ObjectEventKind::Detached) {
            if (!ValidSpawnedId(m.otherId) || SpawnedIdSlot(m.otherId) != fromSlot || detached_.size() >= kMaxKnownObjects) { ++n_.eventsRejected; return false; }
            detached_.insert(m.otherId);
        }
        if (m.kind != ObjectEventKind::Custom) {
            if (history_.size() >= kMaxHistory) history_.erase(history_.begin());
            history_.emplace_back(d, d + n);
        }
        if (m.kind == ObjectEventKind::Destroyed && IsSpawnedId(m.objId)) reconciler_.Remove(m.objId);
    } else if (fromSlot != kHostSlot && view_.Streamer(m.objId) != fromSlot) {   // mesh: straight from the object's streamer
        ++n_.eventsRejected;
        return false;
    }
    Replay(m, now);
    return true;
}

void ObjectSync::Replay(const ObjectEventMsg& m, double nowMs) {
    if (ReplayNow(m, nowMs)) return;
    if (queuedEvents_.size() >= kMaxQueuedEvents) queuedEvents_.erase(queuedEvents_.begin());
    queuedEvents_.push_back(QueuedEvent{m, nowMs});
    ++n_.eventsQueued;
}

// false: the object (or the parent it names) is not here yet, so the event waits.
bool ObjectSync::ReplayNow(const ObjectEventMsg& m, double nowMs) {
    auto it = props_.find(m.objId);
    switch (m.kind) {
        case ObjectEventKind::Destroyed: {
            if (!IsSpawnedId(m.objId)) return true;
            if (destroyed_.size() < 4 * kMaxKnownObjects) destroyed_.insert(m.objId);
            announced_.erase(m.objId);
            ghosts_.Resolve(m.objId);
            if (it == props_.end()) return true;
            Prop& p = it->second;
            // A copy held in a hand or worn is not taken away (the owner's pose and Armor events decide those).
            if (adapter_->StillValid(p.actor, p.index) && !ObjectField(p.actor, 0) && !ParentActorOf(p.actor)) adapter_->DestroyActor(p.actor);
            if (!p.sub) byActor_.erase(p.actor);
            props_.erase(it);
            ++n_.destroyedApplied;
            return true;
        }
        case ObjectEventKind::JointBroke: {
            if (it == props_.end()) return false;
            Prop& p = it->second;
            if (m.constraint >= p.constraints.size()) return true;
            const ConstraintRef& c = p.constraints[m.constraint];
            p.brokenMask |= uint64_t(1) << m.constraint;   // set first: breaking it here is not an event of mine
            if (!adapter_->StillValid(c.comp, c.index)) return true;
            Call ib(c.comp, "IsBroken");
            if (ib.invoke() && ib.get<bool>("ReturnValue")) return true;
            Call(c.comp, "BreakConstraint").invoke();
            ++n_.jointsBroken;
            ++n_.eventsApplied;
            return true;
        }
        case ObjectEventKind::Detached: {
            if (it == props_.end()) return false;
            Prop& p = it->second;
            if (m.body == 0 || m.body >= p.bodies.size()) return true;
            BodyRef& r = p.bodies[m.body];
            if (r.detached) return true;
            r.detached = true;
            p.attachedMask &= ~(uint64_t(1) << m.body);
            if (adapter_->StillValid(r.comp, r.index))
                Call(r.comp, "K2_DetachFromComponent")
                    .set<uint8_t>("LocationRule", 1).set<uint8_t>("RotationRule", 1).set<uint8_t>("ScaleRule", 1).set<bool>("bCallModify", false)
                    .invoke();
            if (!props_.count(m.otherId)) TrackSub(m.otherId, p, m.body, nowMs, false);
            ++n_.bodiesDetached;
            ++n_.eventsApplied;
            return true;
        }
        case ObjectEventKind::Attached: {
            if (it == props_.end()) return false;
            auto parent = props_.find(m.otherId);
            if (parent == props_.end()) return false;
            Prop& p = it->second;
            if (p.attachedSent) return true;
            p.attachedSent = true;
            if (adapter_->StillValid(p.actor, p.index) && adapter_->StillValid(parent->second.actor, parent->second.index))
                Call(p.actor, "K2_AttachToActor")
                    .set<UObject*>("ParentActor", parent->second.actor).set<FName>("SocketName", FName{})
                    .set<uint8_t>("LocationRule", 1).set<uint8_t>("RotationRule", 1).set<uint8_t>("ScaleRule", 1)
                    .set<bool>("bWeldSimulatedBodies", false).invoke();
            ++n_.eventsApplied;
            return true;
        }
        case ObjectEventKind::Custom:
            return true;   // a mod's own event (M5): relayed, nothing replays it here
    }
    return true;
}

// A body that left its object is an object of its own on the same actor: one body, no census, no byActor_ entry.
ObjectSync::Prop* ObjectSync::TrackSub(uint64_t id, Prop& from, size_t body, double nowMs, bool mine) {
    if (props_.count(id) || body >= from.bodies.size()) return nullptr;
    Prop& s = props_[id];
    s.actor = from.actor;
    s.index = from.index;
    s.sub = true;
    s.spawned = true;
    s.rootBody = false;
    s.classHash = from.classHash;
    BodyRef r;
    r.comp = from.bodies[body].comp;
    r.index = from.bodies[body].index;
    s.bodies.assign(1, r);
    s.root = r.comp;
    s.rootIndex = r.index;
    if (mine) { s.lastContactMs = nowMs; s.toucher = uint8_t(MySlot() + 1); }   // the machine that saw it come off leases it
    return &s;
}

}  // namespace hsm::game

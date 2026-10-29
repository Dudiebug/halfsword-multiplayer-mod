#include "gameplay/object_sync.h"
#include "gameplay/weapon_hands.h"
#include "log.h"
#include <algorithm>
#include <cstdio>

// Stand-in hands: drop detection, the hand matching and seating (sync/hsm_handmatch.h), the drops heard of and the weapons a
// stand-in let go of. Moved unchanged out of prop_sync.cpp; the functions stay members of the prop class.

namespace hsm::game {
using namespace hsm::ue;
using namespace hsm::game::hands;

// A weapon leaves the hand of a fighter simulated here (dropped, thrown, disarmed): the hand field moves off it and
// the game clears its "Parent Actor" (a sheathed weapon keeps it, Willie_BP "Set Up Right Hand Weapon").
void ObjectSync::DetectDrops(double nowMs, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
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
void ObjectSync::ResolvePending(double nowMs) {
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
UObject* ObjectSync::UntrackedInHand(UObject* standIn, int hand) {
    UObject* w = standIn ? ObjectField(standIn, hand == 0 ? 1 : 2) : nullptr;
    if (!w || byActor_.count(w) || !adapter_->IsWeapon(w) || adapter_->IsBodyWeapon(w) || ObjectField(w, 0) != standIn) return nullptr;
    return w;
}

// A drop with no copy here: its witness is gone, or there was none and nothing that hand let go of before is the drop's.
// The weapon stays wherever it is here, and a later Grant of the id opens no other window (givenUp_).
void ObjectSync::GiveUp(uint64_t id, uint8_t owner, int hand, const PendingDrop& d, UObject* copy, double nowMs) {
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
void ObjectSync::DeadlineRelease(uint64_t id, uint8_t owner, int hand, UObject* standIn, UObject* copy) {
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
bool ObjectSync::ReleaseFromStandIn(uint64_t propId, UObject* standIn, int hand, UObject* copy) {
    char what[24];
    std::snprintf(what, sizeof what, "%016llx", static_cast<unsigned long long>(propId));
    const DropResult r = DropFromHands(standIn, hand, copy, what);
    ForgetHeld(PropOwnerEntity(propId), standIn);
    if (r != DropResult::Dropped) return false;
    if (env_.handsChanged) env_.handsChanged(PropOwnerEntity(propId));
    Prop* p = Track(propId, copy);
    if (!p) return false;
    p->restoreTicks = kRestoreTicks;
    RestorePhysics(p->actor, p->root, p->rootIndex);
    p->dirty = view_.Find(propId) != nullptr;
    return true;
}

// The drop itself, for ReleaseFromStandIn and the hand matching: both hands when the stand-in holds copy in both.
// Dropped once neither hand holds it and its "Parent Actor" is clear; what names it in the log.
ObjectSync::DropResult ObjectSync::DropFromHands(UObject* standIn, int hand, UObject* copy, const std::string& what) {
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

// ---------------------------------------------------------------- stand-in hands (sync/hsm_handmatch.h)

uint32_t ObjectSync::ClassHashOf(UObject* actor) {
    return actor ? sync::WeaponClassHash(ObjectName(reinterpret_cast<UObject*>(ClassOf(actor))).c_str()) : 0;
}

// What a stand-in holds as the pose counts it: real weapons only (no fists or feet), one weapon in both hands as the right's.
std::array<UObject*, 2> ObjectSync::HandsOf(UObject* standIn) {
    std::array<UObject*, 2> cur{ObjectField(standIn, 1), ObjectField(standIn, 2)};
    if (cur[1] == cur[0]) cur[1] = nullptr;
    for (UObject*& w : cur)
        if (w && (!adapter_->IsWeapon(w) || adapter_->IsBodyWeapon(w))) w = nullptr;
    return cur;
}

void ObjectSync::SetHeld(StandInHand& sh, UObject* weapon) {
    if (sh.held == weapon) return;
    sh.held = weapon;
    sh.heldIndex = weapon ? ObjectIndex(weapon) : -1;
    sh.heldClass = ClassHashOf(weapon);
    sh.match.Reset();   // a mismatch clock belongs to the weapon it was started for
}

// This code changed the stand-in's hands: what they hold now is the matcher's starting point, not a weapon that left.
void ObjectSync::ForgetHeld(uint8_t entity, UObject* standIn) {
    if (!ValidEntity(entity)) return;
    StandInHands& s = standInHands_[entity];
    if (s.willie != standIn) return;
    const std::array<UObject*, 2> cur = HandsOf(standIn);
    for (int h = 0; h < 2; ++h) SetHeld(s.hand[size_t(h)], cur[size_t(h)]);
}

// Every stand-in's hands against its owner's pose.
void ObjectSync::MatchStandIns(double nowMs) {
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

void ObjectSync::MatchHand(uint8_t e, UObject* standIn, int h, std::array<UObject*, 2>& cur, double nowMs) {
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
    f.boundMs = standInHands_[e].boundMs;
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
void ObjectSync::SelfDropped(uint8_t e, UObject* standIn, int h, StandInHand& sh, std::array<UObject*, 2>& cur, double nowMs) {
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
bool ObjectSync::TakeIntoHand(UObject* standIn, int h, UObject* weapon) {
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
bool ObjectSync::Attach(uint8_t e, UObject* standIn, int h, const StandIns::PoseHandView& pose, double nowMs) {
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
    // A level object that was at rest when the census walked (not a census prop, not a weapon): the owner may have
    // picked it up, and this machine's copy of it still lies there (0.8.0-pre7, #88 "holding an invisible rock").
    for (const Candidate& c : idle_) consider(c.actor, c.index, c.classHash);
    int pick = sync::PickAttach(loose, pose.classHash, pose.pos, pose.firstPos);
    if (pick < 0 && (pick = sync::PickAttachFar(loose, pose.classHash, pose.pos, pose.firstPos)) >= 0)
        HSM_LOG("props: stand-in of e%u hand %d: the owner's weapon matched one of its class within %.0f uu here (drifted)", e, h,
                sync::kAttachFarRadiusUu);
    // No copy of that class lies anywhere here (a prop the levels disagree on, a weapon this machine never spawned): once
    // the pose has shown it long enough that it is no round-start loadout still on its way, the hand gets one of its own.
    if (pick < 0 && loose.empty() && nowMs - pose.sinceMs >= kSpawnAfterMs && WeaponClassByHash(pose.classHash, nowMs))
        return SpawnIntoHand(e, standIn, h, pose.classHash);
    if (pick < 0) {
        if (n_.attachMiss < kLogFirst)
            HSM_LOG("props: stand-in of e%u hand %d: no loose weapon of the owner's class %08x within %.0f uu here (%zu of that class loose)",
                    e, h, pose.classHash, sync::kAttachRadiusUu, loose.size());
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

// Every loaded weapon class has a default object, so a walk over the object array's default objects finds them all
// (ue::IsDefaultObject, then the class chain; no names are made but the weapon classes'). Walked again on a miss at most
// every kClassWalkMs; an entry whose slot no longer holds its class (a level change unloaded it) counts as a miss.
UClass* ObjectSync::WeaponClassByHash(uint32_t classHash, double nowMs) {
    auto known = weaponClasses_.find(classHash);
    if (known != weaponClasses_.end() && ObjectAt(known->second.second) == reinterpret_cast<UObject*>(known->second.first))
        return known->second.first;
    if (nowMs - weaponClassesMs_ < kClassWalkMs) return nullptr;
    weaponClassesMs_ = nowMs;
    weaponClasses_.clear();
    UClass* base = FindClass("ModularWeaponBP_C");
    if (!base) return nullptr;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || !IsDefaultObject(o)) continue;
        UClass* c = ClassOf(o);
        if (c == base || !IsChildOf(c, base)) continue;   // the bare base has no look of its own (its passport gives it one)
        weaponClasses_[sync::WeaponClassHash(ObjectName(reinterpret_cast<UObject*>(c)).c_str())] = {c, ObjectIndex(reinterpret_cast<UObject*>(c))};
    }
    known = weaponClasses_.find(classHash);
    return known != weaponClasses_.end() ? known->second.first : nullptr;
}

// The game's own spawn: "Set Up Right/Left Hand Weapon" with no weapon and a class spawns one of that class at the hand's
// socket, gives it the (here empty) Weapon Passport and takes it into the hand (Willie_BP.cpp:6044). An empty passport
// leaves a built weapon's own modules (only the bare ModularWeaponBP_C, never spawned here, takes its look from it).
bool ObjectSync::SpawnIntoHand(uint8_t e, UObject* standIn, int h, uint32_t classHash) {
    UClass* cls = weaponClasses_.count(classHash) ? weaponClasses_[classHash].first : nullptr;
    bool ok = false;
    if (cls) {
        Call c(standIn, h == 0 ? "Set Up Right Hand Weapon" : "Set Up Left Hand Weapon");
        c.set<UClass*>("Weapon Class", cls);
        ok = c.valid() && c.invoke();
    }
    UObject* w = ok ? ObjectField(standIn, h == 0 ? 1 : 2) : nullptr;
    ok = w && ClassHashOf(w) == classHash && ObjectField(w, 0) == standIn;
    if (!ok) {
        if (++n_.spawnFailed <= kLogFirst) HSM_LOG("props: stand-in of e%u hand %d: spawning its own %s failed", e, h,
                                                    cls ? ObjectName(reinterpret_cast<UObject*>(cls)).c_str() : "weapon");
        return false;
    }
    if (++n_.spawned <= kLogFirst)
        HSM_LOG("props: stand-in of e%u hand %d: no %s lies here, so it got one of its own (%s)", e, h,
                ObjectName(reinterpret_cast<UObject*>(cls)).c_str(), ObjectName(w).c_str());
    return true;
}

// A weapon a stand-in let go of here: it falls and lands (RestorePhysics, re-asserted for kRestoreTicks). A prop goes
// back to its stream; any other weapon waits in loose_ for a drop of that hand to be heard of (Adopt).
void ObjectSync::LetLoose(uint8_t e, int h, UObject* w, double nowMs) {
    if (auto known = byActor_.find(w); known != byActor_.end()) {
        if (auto p = props_.find(known->second); p != props_.end()) {
            p->second.restoreTicks = kRestoreTicks;
            RestorePhysics(p->second.actor, p->second.root, p->second.rootIndex);
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
    RestorePhysics(l.actor, l.root, l.rootIndex);
    loose_.push_back(l);
}

void ObjectSync::TickLoose(double nowMs) {
    for (auto it = loose_.begin(); it != loose_.end();) {
        Loose& l = *it;
        if (!adapter_->StillValid(l.actor, l.index) || nowMs - l.atMs >= kLooseKeepMs || byActor_.count(l.actor) || ObjectField(l.actor, 0)) {
            it = loose_.erase(it);   // gone, too old to be a drop on its way, a prop now, or held again
            continue;
        }
        if (l.restoreTicks > 0) { --l.restoreTicks; RestorePhysics(l.actor, l.root, l.rootIndex); }
        ++it;
    }
}

// A drop whose copy the stand-in here let go of already. With a witness, that weapon if it is loose; without, the one
// let go of from that hand most recently before the drop was heard (and still loose), if it is of the class the
// owner's pose showed in that hand before it changed (a weapon this stand-in let go of earlier is not some other weapon
// the owner dropped since).
bool ObjectSync::Adopt(uint64_t propId, uint8_t owner, int hand, const PendingDrop& d, double nowMs) {
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

}  // namespace hsm::game

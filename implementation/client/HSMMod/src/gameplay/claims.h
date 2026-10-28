// Phase 3 combat through the game's own damage code (owner authority). Every strike lands natively on the machine
// that sees it happen, against the stand-in there; the game's "Deal Complex Damage" on the stand-in is caught by a
// script-VM pre-hook (ue/script_hook.h) and becomes a HitClaim in the struck bone's frame (combat/hsm_claim.h). The
// stand-in still shows the strike (cuts, paint: it is Invulnerable, so Get Damage does nothing). The victim's owner
// replays the claim through its own "Deal Complex Damage" on its own fighter, so armour, wounds, bleeding,
// dismemberment, knock-outs and death happen there exactly as in a solo fight. Everything else that would let a
// stand-in take or deal damage natively is blocked before its body runs, outputs zeroed. Game thread only.
//
// The claim fields, the bone-frame rule, the replay's reset of "Last Complex Damage Impulse" (the claim already passed
// that gate on the attacker's screen) and the per-bone limit follow
// Cyrex0/HalfSword-MP's HSMPCombat design, Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see
// THIRD_PARTY_NOTICES.md); the hooks themselves are ours (pre-hooks that block, where HSMP undid after the fact).
// "Last Damage Taken"/"Last Damaged Bone" are left to the victim's own 0.2 s "Reset Last Damage Taken", as in a solo
// fight. Ours as well: one blow's contacts coalesce into one claim on the attacker (combat::BlowCoalescer), and the
// victim replays arriving claims from a per-victim queue, at most kReplaysPerTick per victim per frame.
// Props (test 5: rocks did no damage): a prop's contact with a fighter is decided where the prop is streamed
// (combat::DecidePropContact). A stand-in struck by a prop streamed here is a claim struck by that prop (its attacker
// the fighter simulated here that held or touched it last, else this machine's player), replayed with the victim's own
// copy of the prop as the striker; my fighter struck by a copy driven from elsewhere is blocked, the streamer's claim
// brings that blow.
#pragma once
#include "combat/hsm_claim.h"
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "gameplay/object_sync.h"
#include "session/hsm_authority.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace hsm::ue { class ScriptCall; }

namespace hsm::game {

class Claims {
public:
    struct Env {
        std::function<uint8_t(const ue::UObject*)> ownedEntity;     // the entity of a fighter simulated here, 0 if none
        std::function<ue::UObject*(uint8_t)> ownedActor;            // the live Willie of an entity simulated here
        std::function<void(const combat::HitClaim&)> send;          // a claim made here (the session routes it)
        std::function<bool(uint8_t attacker, uint8_t victim)> allowed;   // the friendly-fire rule
        // Props: what a prop actor is here (ObjectSync::PropOf), a prop's actor here (ObjectSync::ActorOf), and this machine's
        // player entity (a prop claim's attacker when no fighter simulated here touched the prop). Without the first two
        // a prop's contact with a stand-in is blocked and one with my fighter runs natively, as before rev 13.
        std::function<bool(ue::UObject* actor, ObjectSync::Touch& out)> prop;
        std::function<ue::UObject*(uint64_t propId)> propActor;
        std::function<uint8_t()> myEntity;
    };
    // Installs the hooks (owner "claims"; they resolve on every level load). False when the script VM hook is
    // unavailable: nothing is guarded then (the host does not offer owner sync without it).
    bool Start(Adapter& adapter, StandIns& standIns, OwnerStreams& streams, Env env);
    void Stop();
    bool Active() const { return active_; }
    void Tick(double nowMs);                                         // once per frame: per-bone limit, queued replays

    void Replay(const combat::HitClaim& c, double nowMs);            // a claim against a fighter simulated here: queued
    void Effect(const combat::HitClaim& c);                          // someone else's claim: shown on the stand-in
    void NoteClash(uint8_t attacker, ue::UObject* theirWeapon, double nowMs);   // my weapon met that weapon of attacker's stand-in
    uint8_t RecentAttacker(uint8_t victim, double nowMs) const;      // last replayed attacker within 10 s, else 0
    void Forget(uint8_t entity) {                                    // left, respawned or rejoined: its ids may restart
        dedupe_.Forget(entity);
        coalescer_.Forget(entity);
        stuckLimiter_.Forget(entity);
        queue_.Forget(entity);                                       // blows queued against its old body are dropped
        parry_.Forget(entity);
        if (entity <= kMaxWillies) { lastClashMs_[entity] = {}; recent_[entity] = {}; }   // K4: a new body has no recent attacker
    }
    std::string HealthLine();                                        // since the previous call

private:
    struct Box { ue::UObject* pawn = nullptr; int32_t pawnIndex = -1; ue::UObject* box = nullptr; int32_t boxIndex = -1; };
    struct Recent { uint8_t attacker = 0; double atMs = -1e18; };
    struct Counters {
        uint64_t sent = 0, gated = 0, limited = 0, friendly = 0, coalesced = 0, zeroImpulse = 0, capped = 0, replayed = 0,
                 queued = 0, dropped = 0, dupes = 0, stale = 0, ageUnknown = 0, parried = 0, noVictim = 0, noBone = 0,
                 noStriker = 0, failed = 0, blockedStandIn = 0, blockedOnMine = 0, getDamageBlocked = 0, effects = 0,
                 effectsSkipped = 0, clashes = 0, clashUnmatched = 0, propSent = 0, propReplayed = 0, blockedPropCopy = 0, badBone = 0,
                 stuckSent = 0, stuckLimited = 0, stuckReplayed = 0;
    };
    bool OnDealComplexDamage(ue::ScriptCall& c);
    bool OnGetDamage(ue::ScriptCall& c);
    void OnStuckDamage(ue::ScriptCall& c, ue::UObject* standIn, uint8_t victim);   // a lodged blade of mine, on a stand-in
    void AfterDealComplexDamage(ue::ScriptCall& c);
    void SendPending(ue::ScriptCall& c);                             // the claim made before the body ran, coalesced by its damage
    void MakeClaim(ue::ScriptCall& c, ue::UObject* standIn, uint8_t victim, uint8_t attacker, ue::UObject* attackerWillie, ue::UObject* comp,
                   const ObjectSync::Touch* prop);
    void ReplayNow(const combat::HitClaim& c, double nowMs);         // a queued claim, through my fighter's own DCD
    void ReplayStuck(const combat::HitClaim& c, double nowMs);       // a lodged blade's damage, through my fighter's own Get Damage
    bool BoneXform(ue::UObject* mesh, ue::FName bone, combat::Xform& out) const;
    bool PeerBoneOk(const combat::HitClaim& c);   // M7: a peer's hitBone may be interned (MakeName) only if ValidBoneName
    ue::UObject* Striker(const combat::HitClaim& c) const;                          // the claim's striking component here
    ue::UObject* StrikerOn(ue::UObject* fighter, const combat::HitClaim& c) const;   // the claim's striking component on fighter
    ue::UObject* ComponentNamed(ue::UObject* actor, const std::string& name) const;  // a primitive component of actor by name
    ue::UObject* HitBox(ue::UObject* victim, const combat::HitClaim& c, const combat::DcdInputs& in);
    bool CallGetDamage(ue::UObject* victim, ue::UObject* mesh, ue::UObject* striker, ue::FName bone, const combat::DcdInputs& in,
                       double rawDamage, ue::UObject* hitBox, double* damageOut);
    bool CallDealComplexDamage(ue::UObject* victim, ue::UObject* mesh, ue::UObject* striker, ue::FName bone, const combat::DcdInputs& in,
                               ue::UObject* hitBox, double* damageOut);

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    OwnerStreams* streams_ = nullptr;
    Env env_;
    mutable ue::PreparedCall components_;                            // Actor.K2_GetComponentsByClass (frees its last array)
    ue::UClass* primitive_ = nullptr;
    ue::UClass* boxClass_ = nullptr;
    bool boxFailLogged_ = false;
    combat::ClaimLimiter limiter_;
    combat::ClaimDedupe dedupe_;
    combat::BlowCoalescer coalescer_;
    combat::StuckDamageLimiter stuckLimiter_;
    combat::ReplayQueue queue_;
    combat::ParryWindow parry_;
    std::array<uint32_t, kMaxWillies + 1> nextClaimId_{};
    // The claim just made (no id yet), until its stand-in's own run of the blow returns: its Damage Out decides whether
    // the claim is sent (combat::BlowCoalescer) and ranks it.
    struct Pending { ue::UObject* standIn = nullptr; combat::HitClaim claim; combat::BlowKey key; bool prop = false; };
    Pending pending_;
    std::array<Recent, kMaxWillies + 1> recent_{};
    std::array<std::array<double, 3>, kMaxWillies + 1> lastClashMs_{};   // per attacker and StrikeSource
    std::map<int, Box> boxes_;                                       // victim entity -> its private Hit Box
    Counters n_, reported_;
};

}  // namespace hsm::game

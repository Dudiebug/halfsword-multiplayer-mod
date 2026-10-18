// Phase 3 combat through the game's own damage code (owner authority). Every strike lands natively on the machine
// that sees it happen, against the stand-in there; the game's "Deal Complex Damage" on the stand-in is caught by a
// script-VM pre-hook (ue/script_hook.h) and becomes a HitClaim in the struck bone's frame (combat/hsm_claim.h). The
// stand-in still shows the strike (cuts, paint: it is Invulnerable, so Get Damage does nothing). The victim's owner
// replays the claim through its own "Deal Complex Damage" on its own fighter, so armour, wounds, bleeding,
// dismemberment, knock-outs and death happen there exactly as in a solo fight. Everything else that would let a
// stand-in take or deal damage natively is blocked before its body runs, outputs zeroed. Game thread only.
//
// The claim fields, the bone-frame rule, the replay gate (clear "Last Complex Damage Impulse"; keep "Last Damage
// Taken"/"Last Damaged Bone" only for the same attacker and bone within 0.2 s) and the per-bone limit follow
// Cyrex0/HalfSword-MP's HSMPCombat design, Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see
// THIRD_PARTY_NOTICES.md); the hooks themselves are ours (pre-hooks that block, where HSMP undid after the fact).
#pragma once
#include "combat/hsm_claim.h"
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
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
    };
    // Installs the hooks (owner "claims"; they resolve on every level load). False when the script VM hook is
    // unavailable: nothing is guarded then (the host does not offer owner sync without it).
    bool Start(Adapter& adapter, StandIns& standIns, OwnerStreams& streams, Env env);
    void Stop();
    bool Active() const { return active_; }
    void Tick();                                                     // once per frame (the per-bone-per-tick limit)

    void Replay(const combat::HitClaim& c, double nowMs);            // a claim against a fighter simulated here
    void Effect(const combat::HitClaim& c);                          // someone else's claim: shown on the stand-in
    void NoteClash(uint8_t attacker, double nowMs);                  // my weapon met attacker's stand-in weapon
    uint8_t RecentAttacker(uint8_t victim, double nowMs) const;      // last replayed attacker within 10 s, else 0
    void Forget(uint8_t entity) { dedupe_.Forget(entity); }          // left, respawned or rejoined: its ids may restart
    std::string HealthLine();                                        // since the previous call

private:
    struct Box { ue::UObject* pawn = nullptr; int32_t pawnIndex = -1; ue::UObject* box = nullptr; int32_t boxIndex = -1; };
    struct Recent { uint8_t attacker = 0; double atMs = -1e18; };
    struct Counters {
        uint64_t sent = 0, gated = 0, limited = 0, friendly = 0, replayed = 0, dupes = 0, stale = 0, ageUnknown = 0, parried = 0,
                 noVictim = 0, noBone = 0, noStriker = 0, failed = 0, blockedStandIn = 0, blockedOnMine = 0, getDamageBlocked = 0,
                 effects = 0, effectsSkipped = 0, clashes = 0;
    };
    bool OnDealComplexDamage(ue::ScriptCall& c);
    bool OnGetDamage(ue::ScriptCall& c);
    void AfterDealComplexDamage(ue::ScriptCall& c);
    void MakeClaim(ue::ScriptCall& c, ue::UObject* standIn, uint8_t victim, uint8_t attacker, ue::UObject* attackerWillie, ue::UObject* comp);
    bool BoneXform(ue::UObject* mesh, ue::FName bone, combat::Xform& out) const;
    ue::UObject* StrikerOn(ue::UObject* fighter, const combat::HitClaim& c) const;   // the claim's striking component on fighter
    ue::UObject* ComponentNamed(ue::UObject* actor, const std::string& name) const;  // a primitive component of actor by name
    ue::UObject* HitBox(ue::UObject* victim, const combat::HitClaim& c, const combat::DcdInputs& in);
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
    combat::ReplayGate gate_;
    combat::ParryWindow parry_;
    std::array<uint32_t, kMaxWillies + 1> nextClaimId_{};
    struct Pending { ue::UObject* standIn = nullptr; uint32_t claimId = 0; uint8_t attacker = 0; };
    Pending pending_;   // the claim just made, until its stand-in's own body returns (its damage is logged next to it)
    std::array<Recent, kMaxWillies + 1> recent_{};
    std::array<double, kMaxWillies + 1> lastClashMs_{};
    std::map<int, Box> boxes_;                                       // victim entity -> its private Hit Box
    Counters n_, reported_;
};

}  // namespace hsm::game

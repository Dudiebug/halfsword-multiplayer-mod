#include "gameplay/claims.h"
#include "log.h"
#include "ue/script_hook.h"
#include <cmath>
#include <cstdio>

namespace hsm::game {

using namespace hsm::ue;
using combat::DcdInputs;
using combat::HitClaim;
using combat::StrikeSource;
using combat::Xform;

namespace {

constexpr const char* kHookOwner = "claims";
constexpr double kClashEveryMs = 100;         // ParryWindow keeps 64 clashes: one per 100 ms per attacker
constexpr double kRecentAttackerMs = 10000;   // a death this long after the last replayed blow has no killer
constexpr uint64_t kLogFirst = 20;            // the first claims and replays are logged in full (test evidence)

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };

FVector ToF(const sync::V3& v) { return FVector{v.x, v.y, v.z}; }
sync::V3 ToV(const FVector& v) { return sync::V3{v.x, v.y, v.z}; }
FQuat ToF(const sync::Q4& q) { return FQuat{q.x, q.y, q.z, q.w}; }

Xform ToXform(const FTransform& t) {
    Xform x;
    x.pos = ToV(t.translation);
    x.rot = sync::Q4{t.rotation.x, t.rotation.y, t.rotation.z, t.rotation.w};
    x.scale = ToV(t.scale);
    return x;
}

bool SameName(FName a, FName b) { return a.index == b.index && a.number == b.number; }

const char* SourceName(StrikeSource s) {
    switch (s) {
    case StrikeSource::WeaponRight: return "weapon R";
    case StrikeSource::WeaponLeft: return "weapon L";
    default: return "body";
    }
}

}  // namespace

bool Claims::Start(Adapter& adapter, StandIns& standIns, OwnerStreams& streams, Env env) {
    Stop();
    adapter_ = &adapter;
    standIns_ = &standIns;
    streams_ = &streams;
    env_ = std::move(env);
    if (UClass* actor = FindClass("Actor"))
        components_ = PreparedCall(actor, "K2_GetComponentsByClass", {"ComponentClass", "ReturnValue"});
    primitive_ = FindClass("PrimitiveComponent");
    boxClass_ = FindClass("BoxComponent");
    int n = HookScript(kHookOwner, "Willie_BP_C", "Deal Complex Damage", [this](ScriptCall& c) { return OnDealComplexDamage(c); });
    n += HookScript(kHookOwner, "Willie_BP_C", "Get Damage", [this](ScriptCall& c) { return OnGetDamage(c); });
    // The rules stay registered: at the menu Willie_BP_C may not be loaded yet (n == 0); every level load resolves them.
    active_ = ScriptHookActive();
    HSM_LOG("claims: %s (%d function(s) hooked now; components %s, box class %s)",
            active_ ? "on: hits on stand-ins become claims, replayed by the victim's owner"
                    : "NOT ACTIVE (script VM hook unavailable): stand-ins neither take nor deal damage, no claims",
            n, components_.valid() ? "ok" : "MISSING", boxClass_ ? "ok" : "MISSING");
    return active_;
}

void Claims::Stop() {
    if (adapter_) {
        ClearScriptHooks(kHookOwner);
        if (active_) HSM_LOG("claims: off (%s)", HealthLine().c_str());
        for (auto& [entity, b] : boxes_)
            if (b.box && adapter_->StillValid(b.box, b.boxIndex) && adapter_->StillValid(b.pawn, b.pawnIndex))
                Call(b.box, "K2_DestroyComponent").set<UObject*>("Object", b.pawn).invoke();   // Object: the owner
    }
    *this = Claims{};
}

void Claims::Tick() {
    if (active_) limiter_.NewTick();
}

// ---------------------------------------------------------------- the hooks (game thread, inside the VM)

// self is the fighter struck; "Collided Component" is what struck it. A stand-in struck by a fighter simulated here:
// the claim (when the native gate would let the call through), then the body runs for its cosmetics only (the stand-in
// is Invulnerable, so its Get Damage does nothing). Anything else that would let a stand-in take or deal damage is
// blocked, outputs zeroed. Fighters simulated here struck by fighters simulated here run natively.
bool Claims::OnDealComplexDamage(ScriptCall& c) {
    if (!active_) return false;
    UObject* self = c.self();
    UObject* comp = nullptr;
    c.get("Collided Component", comp);
    UObject* striker = comp ? adapter_->FighterOf(OuterOf(comp)) : nullptr;
    if (const uint8_t victim = standIns_->EntityOf(self)) {
        const uint8_t attacker = striker ? env_.ownedEntity(striker) : 0;
        if (!attacker || attacker == victim) {
            c.ZeroOutputs();
            ++n_.blockedStandIn;
            return true;
        }
        if (env_.allowed && !env_.allowed(attacker, victim)) {
            c.ZeroOutputs();
            ++n_.friendly;
            return true;
        }
        // Willie_BP.cpp:2994, read from the stand-in's own fields exactly as its body is about to.
        FVector impulse{};
        double cutting = 0;
        FName bone{};
        c.get("Hit Impulse", impulse);
        c.get("Cutting Power", cutting);
        c.get("Hit Bone", bone);
        const double* lastImpulse = Field<double>(self, "Last Complex Damage Impulse");
        const FName* lastBone = Field<FName>(self, "Last Complex Damage Bone");
        const double len = std::sqrt(impulse.x * impulse.x + impulse.y * impulse.y + impulse.z * impulse.z);
        if (lastImpulse && lastBone && !combat::DcdGatePasses(len, cutting, *lastImpulse, SameName(*lastBone, bone))) {
            ++n_.gated;
            return false;   // the body returns at its own gate: nothing happens natively either
        }
        if (!limiter_.Allow(victim, NameToString(bone))) {
            c.ZeroOutputs();
            ++n_.limited;
            return true;
        }
        MakeClaim(c, self, victim, attacker, striker, comp);
        return false;
    }
    if (striker && standIns_->IsStandIn(striker) && env_.ownedEntity(self)) {
        c.ZeroOutputs();
        ++n_.blockedOnMine;
        return true;
    }
    return false;
}

// Get Damage reached on a fighter simulated here from a stand-in's component by any path other than our own replay
// (which runs under ScriptHookBypass): e.g. a stand-in's blade left stuck in the body. Blocked, outputs zeroed.
bool Claims::OnGetDamage(ScriptCall& c) {
    if (!active_ || !env_.ownedEntity(c.self())) return false;
    UObject* comp = nullptr;
    if (!c.get("Hit By Component", comp) || !comp) return false;
    if (!standIns_->IsStandIn(adapter_->FighterOf(OuterOf(comp)))) return false;
    c.ZeroOutputs();
    ++n_.getDamageBlocked;
    return true;
}

// ---------------------------------------------------------------- attacker side

void Claims::MakeClaim(ScriptCall& c, UObject* standIn, uint8_t victim, uint8_t attacker, UObject* attackerWillie, UObject* comp) {
    DcdInputs in;
    FName bone{};
    c.get("Hit Bone", bone);
    in.hitBone = NameToString(bone);
    FVector v{};
    if (c.get("Location", v)) in.location = ToV(v);
    if (c.get("Normal", v)) in.normal = ToV(v);
    if (c.get("Hit Velocity", v)) in.hitVelocity = ToV(v);
    if (c.get("Hit Impulse", v)) in.hitImpulse = ToV(v);
    c.get("Cutting Power", in.cuttingPower);
    c.get("Stab Rate", in.stabRate);
    c.get("Rigidity", in.rigidity);
    c.get("Kick Power", in.kickPower);
    c.get("Draw Cut", in.drawCut);
    c.get("Blunt Destruction Int", in.bluntDestruction);
    c.getBool("Lower Threshold In", in.lowerThreshold);
    c.getBool("Damage Parent Bone?", in.damageParentBone);
    c.getBool("Extra High Velocity", in.extraHighVelocity);
    UObject* box = nullptr;
    if (c.get("Hit Box", box) && box) {
        Call world(box, "K2_GetComponentToWorld");
        Call extent(box, "GetUnscaledBoxExtent");
        if (world.invoke() && extent.invoke()) {
            in.hasHitBox = true;
            in.hitBoxWorld = ToXform(world.get<FTransform>("ReturnValue"));
            in.hitBoxExtent = ToV(extent.get<FVector>("ReturnValue"));
        }
    }
    Xform boneWorld;
    if (!BoneXform(adapter_->Mesh(standIn), bone, boneWorld)) {
        ++n_.noBone;
        if (n_.noBone <= 3) HSM_LOG("claims: no bone transform for '%s' on the stand-in of e%u; no claim", in.hitBone.c_str(), victim);
        return;
    }
    StrikeSource source = StrikeSource::Body;
    UObject* weapon = OuterOf(comp);
    UObject** right = Field<UObject*>(attackerWillie, "Weapon R");
    UObject** left = Field<UObject*>(attackerWillie, "Weapon L");
    if (weapon && right && weapon == *right) source = StrikeSource::WeaponRight;
    else if (weapon && left && weapon == *left) source = StrikeSource::WeaponLeft;
    const uint32_t ts = streams_->ClockMs(SteadyNowMs());
    const HitClaim claim = combat::MakeClaim(in, boneWorld, ++nextClaimId_[attacker], attacker, victim, ts, source,
                                             ObjectName(comp), std::string{});
    env_.send(claim);
    ++n_.sent;
    if (n_.sent <= kLogFirst)
        HSM_LOG("claims: sent #%u e%u -> e%u bone %s by %s '%s' |impulse| %.0f cut %.2f stab %.2f box %d at t=%u",
                claim.claimId, attacker, victim, claim.hitBone.c_str(), SourceName(source), claim.strikeComponent.c_str(),
                std::sqrt(in.hitImpulse.x * in.hitImpulse.x + in.hitImpulse.y * in.hitImpulse.y + in.hitImpulse.z * in.hitImpulse.z),
                in.cuttingPower, in.stabRate, in.hasHitBox ? 1 : 0, ts);
}

bool Claims::BoneXform(UObject* mesh, FName bone, Xform& out) const {
    if (!mesh) return false;
    Call socket(mesh, "GetSocketTransform");
    socket.setName("InSocketName", bone);   // TransformSpace stays 0 in the zeroed buffer: RTS_World
    if (!socket.invoke()) return false;
    const FTransform t = socket.get<FTransform>("ReturnValue");
    if (!std::isfinite(t.translation.x) || !std::isfinite(t.rotation.w)) return false;
    out = ToXform(t);
    return true;
}

// ---------------------------------------------------------------- victim side

void Claims::Replay(const HitClaim& c, double nowMs) {
    if (!active_) return;
    if (!dedupe_.Accept(c.attacker, c.claimId)) { ++n_.dupes; return; }
    int64_t ownerNow = 0;
    if (standIns_->OwnerClockNow(c.attacker, nowMs, ownerNow)) {
        const int64_t age = ownerNow - int64_t(c.attackerTsMs);
        if (!combat::ClaimFresh(age)) {
            ++n_.stale;
            if (n_.stale <= 5) HSM_LOG("claims: dropped #%u from e%u: %lld ms old", c.claimId, c.attacker, static_cast<long long>(age));
            return;
        }
    } else {
        ++n_.ageUnknown;   // no pose from that attacker yet: its clock cannot be estimated; accepted
    }
    if (parry_.Cancels(c)) {
        ++n_.parried;
        HSM_LOG("claims: #%u from e%u parried (my weapon met theirs around t=%u)", c.claimId, c.attacker, c.attackerTsMs);
        return;
    }
    UObject* victim = env_.ownedActor ? env_.ownedActor(c.victim) : nullptr;
    UObject* mesh = victim ? adapter_->Mesh(victim) : nullptr;
    if (!mesh) { ++n_.noVictim; return; }   // between lives: the blow has nothing to land on
    const FName bone = c.hitBone.empty() ? FName{} : MakeName(c.hitBone);
    Call index(mesh, "GetBoneIndex");
    index.setName("BoneName", bone).invoke();
    Xform boneWorld;
    if (c.hitBone.empty() || index.get<int32_t>("ReturnValue") < 0 || !BoneXform(mesh, bone, boneWorld)) {
        ++n_.noBone;
        if (n_.noBone <= 3) HSM_LOG("claims: #%u from e%u names bone '%s', which my fighter lacks", c.claimId, c.attacker, c.hitBone.c_str());
        return;
    }
    const DcdInputs in = combat::ReplayInputs(c, boneWorld);
    const combat::ReplayGate::Decision d = gate_.Before(c);
    if (d.clearComplexImpulse)
        if (double* p = Field<double>(victim, "Last Complex Damage Impulse")) *p = 0.0;
    if (!d.keepLastDamage) {
        if (double* p = Field<double>(victim, "Last Damage Taken")) *p = 0.0;
        if (FName* p = Field<FName>(victim, "Last Damaged Bone")) *p = FName{};
    }
    UObject* striker = StrikerOn(standIns_->ActorOf(c.attacker), c);
    if (!striker) ++n_.noStriker;   // the blow still lands; blood is painted on nothing
    UObject* box = in.hasHitBox ? HitBox(victim, c, in) : nullptr;
    double damage = 0;
    if (!CallDealComplexDamage(victim, mesh, striker, bone, in, box, &damage)) {
        ++n_.failed;
        if (n_.failed <= 3) HSM_LOG("claims: #%u from e%u: Deal Complex Damage could not be called", c.claimId, c.attacker);
        return;
    }
    gate_.Applied(c);
    recent_[c.victim] = Recent{c.attacker, nowMs};
    ++n_.replayed;
    if (n_.replayed <= kLogFirst)
        HSM_LOG("claims: replayed #%u e%u -> e%u bone %s by %s '%s'%s box %d: damage out %.2f%s", c.claimId, c.attacker,
                c.victim, c.hitBone.c_str(), SourceName(c.source), c.strikeComponent.c_str(), striker ? "" : " (no striker)",
                box ? 1 : 0, damage, d.keepLastDamage ? " (same blow continued)" : "");
}

// The claim's striking component on fighter (the attacker's stand-in here): the named component on its weapon or body,
// else the weapon's root or the body's Mesh. nullptr when fighter or the weapon is gone.
UObject* Claims::StrikerOn(UObject* fighter, const HitClaim& c) const {
    if (!fighter) return nullptr;
    UObject* owner = fighter;
    if (c.source != StrikeSource::Body) {
        UObject** w = Field<UObject*>(fighter, c.source == StrikeSource::WeaponRight ? "Weapon R" : "Weapon L");
        owner = w ? *w : nullptr;
        if (!owner) return nullptr;
    }
    if (UObject* named = ComponentNamed(owner, c.strikeComponent)) return named;
    if (c.source == StrikeSource::Body) return adapter_->Mesh(fighter);
    Call root(owner, "K2_GetRootComponent");
    return root.invoke() ? root.get<UObject*>("ReturnValue") : nullptr;
}

UObject* Claims::ComponentNamed(UObject* actor, const std::string& name) const {
    if (name.empty() || !primitive_ || !components_.valid()) return nullptr;
    if (!components_.put(0, primitive_).invoke(actor)) return nullptr;
    const FObjArray arr = components_.take<FObjArray>(1);
    if (!arr.data || arr.num <= 0 || arr.num > 512) return nullptr;
    for (int32_t i = 0; i < arr.num; ++i)
        if (arr.data[i] && ObjectName(arr.data[i]) == name) return arr.data[i];
    return nullptr;
}

// The claim's Hit Box, re-made on the victim: a private BoxComponent on the victim fighter, collision off, placed at the
// claim's frame and extent just before each replay. Deal Complex Damage and Get Damage use the Hit Box only for its
// geometry (extent, rotation, "is within" tests), so a box that touches nothing serves. One per victim fighter.
UObject* Claims::HitBox(UObject* victim, const HitClaim& c, const DcdInputs& in) {
    if (!boxClass_) return nullptr;
    Box& b = boxes_[c.victim];
    const bool live = b.box && b.pawn == victim && adapter_->StillValid(b.pawn, b.pawnIndex) && adapter_->StillValid(b.box, b.boxIndex);
    if (!live) {
        b = Box{};
        const FTransform identity{};
        Call add(victim, "AddComponentByClass");
        add.set<UClass*>("Class", boxClass_).set<bool>("bManualAttachment", false).set<FTransform>("RelativeTransform", identity)
            .set<bool>("bDeferredFinish", true);
        UObject* box = add.invoke() ? add.get<UObject*>("ReturnValue") : nullptr;
        if (!box) {
            if (!boxFailLogged_) HSM_LOG("claims: could not add a Hit Box to my fighter; replays run without one");
            boxFailLogged_ = true;
            return nullptr;
        }
        // Collision is off before the component is registered, so it never overlaps anything, even once.
        Call(box, "SetCollisionEnabled").set<uint8_t>("NewType", 0).invoke();   // ECollisionEnabled::NoCollision
        Call(box, "SetGenerateOverlapEvents").set<bool>("bInGenerateOverlapEvents", false).invoke();
        Call(victim, "FinishAddComponent").set<UObject*>("Component", box).set<bool>("bManualAttachment", false)
            .set<FTransform>("RelativeTransform", identity).invoke();
        b = Box{victim, ObjectIndex(victim), box, ObjectIndex(box)};
        HSM_LOG("claims: private Hit Box %s made on my fighter e%u", ObjectName(box).c_str(), c.victim);
    }
    FTransform t{};
    t.rotation = ToF(in.hitBoxWorld.rot);
    t.translation = ToF(in.hitBoxWorld.pos);
    t.scale = ToF(in.hitBoxWorld.scale);
    Call place(b.box, "K2_SetWorldTransform");
    place.set<FTransform>("NewTransform", t).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
    Call(b.box, "SetBoxExtent").set<FVector>("InBoxExtent", ToF(in.hitBoxExtent)).set<bool>("bUpdateOverlaps", false).invoke();
    return b.box;
}

bool Claims::CallDealComplexDamage(UObject* victim, UObject* mesh, UObject* striker, FName bone, const DcdInputs& in,
                                   UObject* hitBox, double* damageOut) {
    Call dcd(victim, "Deal Complex Damage");
    if (!dcd.valid()) return false;
    dcd.set<UObject*>("Hit Component", mesh)
        .set<UObject*>("Collided Component", striker)
        .setName("Hit Bone", bone)
        .set<FVector>("Location", ToF(in.location))
        .set<FVector>("Normal", ToF(in.normal))
        .set<FVector>("Hit Velocity", ToF(in.hitVelocity))
        .set<FVector>("Hit Impulse", ToF(in.hitImpulse))
        .set<double>("Cutting Power", in.cuttingPower)
        .set<double>("Stab Rate", in.stabRate)
        .set<double>("Rigidity", in.rigidity)
        .set<int32_t>("Blunt Destruction Int", in.bluntDestruction)
        .set<bool>("Lower Threshold In", in.lowerThreshold)
        .set<bool>("Damage Parent Bone?", in.damageParentBone)
        .set<double>("Kick Power", in.kickPower)
        .set<UObject*>("Hit Box", hitBox)
        .set<bool>("Extra High Velocity", in.extraHighVelocity)
        .set<double>("Draw Cut", in.drawCut);
    bool ok = false;
    {
        ScriptHookBypass bypass;   // our own call, and the Get Damage inside it, pass the claims hooks
        ok = dcd.invoke();
    }
    if (ok && damageOut) *damageOut = dcd.get<double>("Damage Out");
    return ok;
}

// ---------------------------------------------------------------- third parties, parries, deaths

// Someone else's blow between two fighters both simulated elsewhere: shown on my stand-in of the victim (cuts, paint)
// through the same function; the stand-in is Invulnerable and its dismemberment is off, so nothing else happens.
void Claims::Effect(const HitClaim& c) {
    if (!active_) return;
    UObject* standIn = standIns_->ActorOf(c.victim);
    UObject* mesh = standIn ? adapter_->Mesh(standIn) : nullptr;
    Xform boneWorld;
    const FName bone = (mesh && !c.hitBone.empty()) ? MakeName(c.hitBone) : FName{};
    if (!mesh || c.hitBone.empty() || !BoneXform(mesh, bone, boneWorld)) { ++n_.effectsSkipped; return; }
    const DcdInputs in = combat::ReplayInputs(c, boneWorld);
    if (double* p = Field<double>(standIn, "Last Complex Damage Impulse")) *p = 0.0;
    UObject* striker = StrikerOn(standIns_->ActorOf(c.attacker), c);
    if (CallDealComplexDamage(standIn, mesh, striker, bone, in, nullptr, nullptr)) ++n_.effects;
    else ++n_.effectsSkipped;
}

// My weapon met attacker's stand-in weapon: noted at the owner time that stand-in shows, so a claim from that attacker
// made around then is a parried blow (ParryWindow). At most one per attacker per 100 ms.
void Claims::NoteClash(uint8_t attacker, double nowMs) {
    if (!active_ || !ValidEntity(attacker)) return;
    if (nowMs - lastClashMs_[attacker] < kClashEveryMs) return;
    uint32_t shown = 0;
    if (!standIns_->ShownOwnerTs(attacker, shown)) return;
    lastClashMs_[attacker] = nowMs;
    parry_.NoteClash(attacker, shown);
    ++n_.clashes;
}

uint8_t Claims::RecentAttacker(uint8_t victim, double nowMs) const {
    if (!ValidEntity(victim)) return 0;
    const Recent& r = recent_[victim];
    return r.attacker && nowMs - r.atMs <= kRecentAttackerMs ? r.attacker : 0;
}

std::string Claims::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    char line[512];
    std::snprintf(line, sizeof line,
                  "claims: sent %llu (gated %llu limited %llu friendly %llu) | replayed %llu (dupes %llu stale %llu "
                  "age? %llu parried %llu novictim %llu nobone %llu nostriker %llu failed %llu) | blocked: on stand-ins %llu, "
                  "on mine %llu, get damage %llu | fx %llu (skipped %llu) clashes %llu",
                  d(a.sent, b.sent), d(a.gated, b.gated), d(a.limited, b.limited), d(a.friendly, b.friendly),
                  d(a.replayed, b.replayed), d(a.dupes, b.dupes), d(a.stale, b.stale), d(a.ageUnknown, b.ageUnknown),
                  d(a.parried, b.parried), d(a.noVictim, b.noVictim), d(a.noBone, b.noBone), d(a.noStriker, b.noStriker),
                  d(a.failed, b.failed), d(a.blockedStandIn, b.blockedStandIn), d(a.blockedOnMine, b.blockedOnMine),
                  d(a.getDamageBlocked, b.getDamageBlocked), d(a.effects, b.effects), d(a.effectsSkipped, b.effectsSkipped),
                  d(a.clashes, b.clashes));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game

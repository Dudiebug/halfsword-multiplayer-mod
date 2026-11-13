#include "gameplay/fx_sync.h"
#include "combat/hsm_paintgate.h"
#include "match/hsm_strip.h"
#include "log.h"
#include "ue/script_hook.h"
#include "util/hsm_sanitize.h"
#include "util/hsm_validate.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace hsm::game {

using namespace hsm::ue;
using combat::FxKind;
using combat::PaintFxMsg;
using combat::Xform;

namespace {

struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };   // any TArray's header (data, count, capacity)

constexpr const char* kHookOwner = "fxsync";
constexpr uint64_t kLogFirst = 10;   // the first reports and replays are logged in full (test evidence)
constexpr double kVoiceRateMin = 0.0, kVoiceRateMax = 2.0;  // clamp voice rate
constexpr double kPaintWaitMs = 1500.0;   // longest an armour change waits for its fighter's paint tasks (T5)
// A strip request is not applied mid-paint at all (pre24 tester feedback: a crash stripping a bleeding-out fighter): it
// waits this long for the paint to go quiet, then is dropped (the stripper can try again).
constexpr double kStripWaitMaxMs = 6000.0;
constexpr size_t kHeldMax = 64;           // armour changes held at once; beyond it they apply at once, as before
constexpr uint32_t kPaintLogFirst = 20;
constexpr double kWoundRetryMs = 1500.0;   // longest a wound waits for its stand-in's Box Vertex Paint gate to open
constexpr size_t kWoundRetryMax = 16;      // wounds waiting at once; beyond it they are dropped (they are cosmetic)

bool ChangesArmor(const PaintFxMsg& m) {
    return combat::IsStripRequest(m) || m.kind == FxKind::Armor || m.kind == FxKind::ArmorOn;
}

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

FTransform ToF(const Xform& x) {
    FTransform t{};
    t.rotation = ToF(x.rot);
    t.translation = ToF(x.pos);
    t.scale = ToF(x.scale);
    return t;
}

double Len(const FVector& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
double Clamp(double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : lo; }
sync::V3 ClampV(const sync::V3& v, double lim) { return sync::V3{Clamp(v.x, -lim, lim), Clamp(v.y, -lim, lim), Clamp(v.z, -lim, lim)}; }
uint8_t Part(uint8_t p) { return p < combat::kFxBodyParts ? p : 0; }

// A double member of the fighter, 0 when it cannot be read.
double Num(UObject* willie, const char* name) {
    const double* p = Field<double>(willie, name);
    return p && std::isfinite(*p) ? *p : 0.0;
}
uint8_t Byte(UObject* willie, const char* name) {
    const uint8_t* p = Field<uint8_t>(willie, name);
    return p ? *p : 0;
}
bool Flag(UObject* willie, const char* name) {
    bool v = false;
    return GetBool(willie, name, v) && v;
}
// Box Vertex Paint's four busy flags on a fighter (Willie_BP.cpp:2206, build 26568321; combat::BoxVertexPaintBusy).
bool PaintFlagsBusy(UObject* willie) {
    return combat::BoxVertexPaintBusy(Flag(willie, "Dismemberment In Process"), Flag(willie, "Armor Swap In Process"),
                                      Flag(willie, "Setup Armor in Process"), Flag(willie, "Hat Not Removable 2"));
}
// Block Undress Slots (Willie_BP.cpp:1395; filled by a dismember, 4366-4397): Strip Armor From Bone skips a slot in it.
bool UndressBlocked(UObject* willie, uint8_t slot) {
    const FObjArray* a = Field<FObjArray>(willie, "Block Undress Slots");   // a TArray<uint8>: read as bytes
    if (!a || !a->data || a->num <= 0 || a->num > 64) return false;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(a->data);
    for (int32_t i = 0; i < a->num; ++i)
        if (bytes[i] == slot) return true;
    return false;
}

const char* KindName(FxKind k) {
    switch (k) {
        case FxKind::Blood: return "blood";
        case FxKind::Wound: return "wound";
        case FxKind::Voice: return "voice";
        case FxKind::HitSound: return "hit sound";
        case FxKind::Armor: return "armour removal";
        case FxKind::ArmorOn: return "armour put on";
        case FxKind::Crossbow: return "crossbow";
    }
    return "?";
}

// Get Damage's cues (Willie_BP.cpp:4071, 4385), loaded with the fighter blueprint that references them.
const char* BluntCueName(combat::BluntCue c) {
    switch (c) {
        case combat::BluntCue::Light: return "Sound_Gore_blunt_light_10_Cue";
        case combat::BluntCue::WetHard: return "Wet_Hard_Hit_Cue";
        case combat::BluntCue::BluntHard: return "Sound_Gore_blunt_hard_10_Cue";
        case combat::BluntCue::None: break;
    }
    return nullptr;
}
constexpr const char* kBoneCue = "Bone_Cue";
constexpr double kArmorMatchUu = 1500.0;       // a stand-in picks up the nearest loose piece of the owner's class within this
constexpr const char* kArmorBone = "pelvis";   // Remove Armor's spawn transform travels in the pelvis frame

}  // namespace

bool FxSync::Start(Adapter& adapter, StandIns& standIns, Env env) {
    Stop();
    adapter_ = &adapter;
    standIns_ = &standIns;
    env_ = std::move(env);
    boxClass_ = FindClass("BoxComponent");
    int n = ObserveScript(kHookOwner, "Willie_BP_C", "Get Damage", {}, [this](ScriptCall& c) { OnGetDamage(c); });
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Spawn Blood", [this](ScriptCall& c) { OnSpawnBlood(c); });
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Box Vertex Paint", {}, [this](ScriptCall& c) { OnBoxVertexPaint(c); });
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Helmet Muffle Voice", [this](ScriptCall& c) { OnVoice(c); });
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Equip Armor", {}, [this](ScriptCall& c) { OnEquipArmor(c); });   // was Pick Up Armor before build 26568321 (Willie_BP.cpp:4623)
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Remove Armor", {}, [this](ScriptCall& c) { OnRemoveArmor(c); });
    n += HookScript(kHookOwner, "Willie_BP_C", "Remove Armor", [this](ScriptCall& c) { return StripKeepsArmor(c); });   // the Stripping row Off
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Strip Armor From Bone", [this](ScriptCall& c) { OnStripBegin(c); },
                       [this](ScriptCall&) { OnStripEnd(); });
    active_ = ScriptHookActive();
    HSM_LOG("fx: %s (%d function(s) observed now; box class %s)",
            active_ ? "on: blood, wounds, voice, hit sounds and armour off and on on fighters simulated here are shown on every stand-in of them"
                    : "NOT ACTIVE (script VM hook unavailable): stand-ins show no blood, wounds, sounds or armour removal",
            n, boxClass_ ? "ok" : "MISSING");
    return active_;
}

void FxSync::Stop() {
    if (adapter_) {
        ClearScriptHooks(kHookOwner);
        if (active_) HSM_LOG("fx: off (%s)", HealthLine().c_str());
        for (auto& [entity, b] : boxes_)
            if (b.box && adapter_->StillValid(b.box, b.boxIndex) && adapter_->StillValid(b.actor, b.actorIndex))
                Call(b.box, "K2_DestroyComponent").set<UObject*>("Object", b.actor).invoke();   // Object: the owner
    }
    *this = FxSync{};
}

// ---------------------------------------------------------------- the owner's side (observers, inside the VM)

// Get Damage paints its wound on Damaged Mesh at the hit: within its Hit Box when it has one, else at Location with a
// radius from Damage Raw Scaled (a local of the call, still in the frame once it returned) and |Impulse|. It painted
// nothing when Damage Raw Scaled stayed 0 (any of its early gates, Invulnerable first) or vertex paint is off, nor for
// damage it passed on to the parent bone (combat::IsPassedOnDamage).
void FxSync::OnGetDamage(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity) return;
    double drs = 0;
    if (!c.get("Damage Raw Scaled", drs) || !(drs > 0)) return;
    SendHitSound(entity, self, c, drs);
    if (Flag(self, "Force Disable Vertex Paint")) return;
    FName bone{};
    c.get("Bone", bone);
    UObject* box = nullptr;
    if (c.get("Hit Box", box) && box) {
        Call world(box, "K2_GetComponentToWorld");
        Call extent(box, "GetUnscaledBoxExtent");
        if (world.invoke() && extent.invoke()) {
            SendWound(entity, self, bone, ToXform(world.get<FTransform>("ReturnValue")), ToV(extent.get<FVector>("ReturnValue")));
            return;
        }
    }
    FVector location{}, impulse{};
    if (!c.get("Location", location)) return;
    if (combat::IsPassedOnDamage(ToV(location), box != nullptr)) { ++n_.passedOn; return; }
    c.get("Impulse", impulse);
    const double r = combat::WoundRadius(drs, Len(impulse));
    Xform at;
    at.pos = ToV(location);
    SendWound(entity, self, bone, at, sync::V3{r, r, r});
}

// Spawn Blood on a fighter simulated here, seen before its body: the call as made plus the state its gates read (they
// change it as the body runs). Its callers (build 26568321): Get Damage (Willie_BP.cpp:5210), the blood of a real blow, and
// Dismember Function Delayed's two bursts (4204, 4225), which pass Skip Check true. The latter are not sent: the stand-in's
// replayed sever makes them again. A call its first gate drops (28789: 4 Attached Blood Emitters and no Skip Check) shows
// nowhere, so it is not sent either (combat::SendSpawnBlood).
void FxSync::OnSpawnBlood(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity || Flag(self, "Is Zombie?")) return;
    bool skipCheck = false;
    c.getBool("Skip Check", skipCheck);
    const FObjArray* emitters = Field<FObjArray>(self, "Attached Blood Emitters");   // a TArray: the count is all that is read
    const int attached = emitters ? emitters->num : 0;
    if (!combat::SendSpawnBlood(attached, skipCheck)) {
        if (n_.bloodGated++ < kLogFirst)
            HSM_LOG("fx: Spawn Blood on my e%u not sent: %s (%d Attached Blood Emitters) (%llu)", entity,
                    skipCheck ? "Skip Check call (a dismember burst; the stand-in's sever makes its own)" : "the game's own gate drops it (4 or more emitters)",
                    attached, static_cast<unsigned long long>(n_.bloodGated));
        return;
    }
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::Blood;
    FName bone{};
    c.get("Bone", bone);
    FTransform t{};
    c.get("Transform", t);
    m.local = ToXform(t);   // world for now; Send moves it into the bone's frame
    m.rate = Clamp(c.get<double>("Spawn Rate"), 0, combat::kFxMaxRate);
    m.bodyPart = Part(c.get<uint8_t>("Body Partt"));
    m.loopCount = uint8_t(std::clamp<int32_t>(c.get<int32_t>("Loop Count"), 1, combat::kFxMaxLoops));
    bool extra = false;
    c.getBool("Extra Velocity", extra);
    m.extraVelocity = extra;
    m.velocity = ToV(c.get<FVector>("Velocity"));
    m.power = ToV(c.get<FVector>("Pover"));
    m.bleeding = Clamp(Num(self, "Bleeding"), 0, combat::kFxMaxBleeding);
    m.gateLastRate = Clamp(Num(self, "Last Spawned Blood Rate"), 0, combat::kFxMaxRate);
    m.gateSpRate = Clamp(Num(self, "Spawn Blood Sp Rate"), 0, combat::kFxMaxRate);
    m.gatePart = Part(Byte(self, "Spawn Blood Body Part"));
    if (Send(m, self, bone)) ++n_.blood;
}

// Box Vertex Paint on a fighter simulated here (a stuck stab's wound), seen once it returned: it painted when its first
// gate passed (Willie_BP.cpp:2206, build 26568321): !Invulnerable and none of Dismemberment In Process, Armor Swap In
// Process, Setup Armor in Process, Hat Not Removable 2 (combat::BoxVertexPaintRuns). Vertex paint off is its second gate,
// rechecked by the replay. A call that gate refused painted nothing, so it is not sent.
void FxSync::OnBoxVertexPaint(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity) return;
    if (!combat::BoxVertexPaintRuns(Flag(self, "Invulnerable"), Flag(self, "Dismemberment In Process"), Flag(self, "Armor Swap In Process"),
                                    Flag(self, "Setup Armor in Process"), Flag(self, "Hat Not Removable 2"))) {
        if (n_.paintGated++ < kLogFirst)
            HSM_LOG("fx: Box Vertex Paint on my e%u painted nothing (Invulnerable, dismembering, armour swap or setup, or hat flag set): not sent (%llu)",
                    entity, static_cast<unsigned long long>(n_.paintGated));
        return;
    }
    UObject* box = nullptr;
    if (!c.get("Box", box) || !box) return;
    FName bone{};
    c.get("Hit Bone", bone);
    Call world(box, "K2_GetComponentToWorld");
    Call extent(box, "GetUnscaledBoxExtent");
    if (!world.invoke() || !extent.invoke()) return;
    SendWound(entity, self, bone, ToXform(world.get<FTransform>("ReturnValue")), ToV(extent.get<FVector>("ReturnValue")));
}

// The blunt and bone sounds the blow just played here (combat::PlanHitSound decides them again on the receiver; a blow
// that played neither is not sent).
void FxSync::SendHitSound(uint8_t entity, UObject* willie, ScriptCall& c, double drs) {
    bool inside = false, lower = false;
    c.getBool("Inside", inside);
    c.getBool("Lower Threshold", lower);
    FVector location{}, impulse{};
    c.get("Location", location);
    c.get("Impulse", impulse);
    const double len = Len(impulse);
    const combat::HitSoundPlan plan = combat::PlanHitSound(len, drs, lower, inside);
    if (plan.blunt == combat::BluntCue::None && !(plan.boneVolume > 0)) return;
    FName bone{};
    c.get("Bone", bone);
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::HitSound;
    m.local.pos = ToV(location);   // world for now
    m.velocity = sync::V3{Clamp(len, 0, combat::kFxMaxVector), Clamp(drs, 0, combat::kFxMaxVector), 0};
    m.extraVelocity = lower;
    if (Send(m, willie, bone, false)) ++n_.hits;
}

// Helmet Muffle Voice on a fighter simulated here: every one of its voice lines (pain, curse, exertion, zombie) calls
// it right after Voice Audio.Play (Willie_BP.cpp, 9 sites), so the cue just started is the component's Sound.
void FxSync::OnVoice(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity) return;
    UObject* const* audio = Field<UObject*>(self, "Voice Audio");
    UObject* const* sound = audio && *audio ? Field<UObject*>(*audio, "Sound") : nullptr;
    const float* volume = audio && *audio ? Field<float>(*audio, "VolumeMultiplier") : nullptr;
    if (!sound || !*sound) return;
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::Voice;
    m.bone = ObjectName(*sound);
    m.rate = Clamp(volume ? *volume : 1.0, 0, combat::kFxMaxRate);
    if (!combat::ValidVoiceCueName(m.bone)) { ++n_.noName; return; }   // a cue every receiver would refuse is not sent
    if (!limiter_.Allow(m.entity, m.kind, SteadyNowMs())) { ++n_.limited; return; }
    m.seq = ++seq_;
    env_.send(m);
    ++n_.pains;
}

// Equip Armor (Willie_BP.cpp:4623, build 26568321; Pick Up Armor before it) on a fighter simulated here, seen once it
// returned: Picked Up says whether the piece went on. Its callers are the hand-to-body equips (19439, 19810, 19873).
void FxSync::OnEquipArmor(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity) return;
    if (!equipLogged_) {
        equipLogged_ = true;
        HSM_LOG("fx: armour put on is observed at Equip Armor (build 26568321; it was Pick Up Armor before)");
    }
    bool picked = false;
    UObject* actor = nullptr;
    if (!c.getBool("Picked Up", picked) || !picked || !c.get("Armor Actor", actor) || !actor) return;
    if (env_.gearChanged) env_.gearChanged(entity);   // the new piece wears the game's own colours until the kit is put back
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::ArmorOn;
    m.local.pos = ToV(adapter_->ActorLocation(actor));   // world for now (the piece, carried in a hand)
    const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(actor)));
    if (!combat::ValidArmorClassName(cls)) { ++n_.noName; return; }   // a class every receiver would refuse is not sent
    if (Send(m, self, MakeName(kArmorBone), false, cls)) ++n_.armor;
}

// Remove Armor on a fighter simulated here, seen once it returned: the slot left its passport and a loose piece was
// spawned at SpawnTransform (Willie_BP.cpp:5087). Whoever caused it (a pick-up swap, a strip, the fighter itself).
// On a stand-in, outside our own replays, it is this machine's fighter stripping a fallen one (Strip Armor From Bone,
// 7386): that goes to the owner as a strip request, the only machine that may change its fighter.
void FxSync::OnRemoveArmor(ScriptCall& c) {
    if (!active_ || replaying_) return;
    UObject* self = c.self();
    uint8_t entity = env_.ownedEntity(self);
    bool strip = false;
    if (!entity) {
        entity = standIns_->EntityOf(self);
        strip = entity != 0;
    }
    if (!entity) return;
    uint8_t slot = 0;
    FTransform t{};
    if (!c.get("Slot", slot) || !c.get("SpawnTransform", t)) return;
    // T5 evidence: the game's own Remove Armor while that fighter's paint tasks still run (the mod cannot hold this one).
    if (!strip && paintBusyOwnLogged_ < kPaintLogFirst && PaintBusy(self))
        HSM_LOG("fx: the game removed armour slot %u from my e%u while its paint tasks were still queued (%u)", unsigned(slot),
                entity, ++paintBusyOwnLogged_);
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::Armor;
    m.rate = slot;
    m.extraVelocity = strip;
    m.local = ToXform(t);   // world for now
    if (strip) stripped_.insert({entity, slot});   // the owner's report of it is not replayed again here
    if (Send(m, self, MakeName(kArmorBone), false)) ++n_.armor;
    if (env_.noteSpawn) env_.noteSpawn();   // the loose piece is a new object: the object sync looks for it now
}

// Strip Armor From Bone (Willie_BP.cpp:10109, build 26568321) strips a slot only while the stripped fighter is Fallen
// and the slot is not in its Block Undress Slots (10298, per slot), and a stand-in's Fallen never follows its owner.
// While my fighter strips a stand-in whose owner reports it down (vitals: dead, fallen or knocked out), the stand-in is
// Fallen for that one call, so the stock strip runs and its Remove Armor goes to the owner as a strip request (the owner
// refuses a slot in its own Block Undress Slots: ApplyStrip). The call's other branch, a sphere trace at the stripper's
// hand (10116-10120), takes a weapon the stand-in really holds whatever its Fallen: it is not sent to the owner; the
// weapon taken is logged (stripWeapon).
void FxSync::OnStripBegin(ScriptCall& c) {
    stripFallen_ = nullptr;
    stripWatch_ = nullptr;
    stripping_ = active_ && !replaying_ ? c.get<UObject*>("Stripped Willie") : nullptr;   // StripKeepsArmor
    if (!active_ || replaying_ || !env_.ownedEntity(c.self())) return;
    UObject* target = c.get<UObject*>("Stripped Willie");
    const uint8_t entity = target ? standIns_->EntityOf(target) : 0;
    if (entity) {
        stripWatch_ = target;
        stripWatchEntity_ = entity;
        for (int i = 0; i < 3; ++i) {
            UObject* const* w = Field<UObject*>(target, i == 0 ? "Weapon R" : i == 1 ? "Weapon L" : "Weapon Slot Back");
            stripWeapons_[i] = w ? *w : nullptr;
        }
    }
    const VitalsMsg* v = entity ? standIns_->Vitals(entity) : nullptr;
    if (!v || !((v->flags & 7u) != 0 || v->health <= 0.f) || Flag(target, "Fallen")) return;
    // A bleeding body's paint tasks read the armour being swapped (the engine's vertex-paint crash): while they are queued
    // the stand-in stays not Fallen, so the stock strip does nothing this time (the stripper's next try gets through).
    if (PaintBusy(target)) {
        if (n_.stripHeld++ < 10) HSM_LOG("fx: strip of e%u's stand-in held back (its paint tasks are still queued)", entity);
        return;
    }
    if (!SetBool(target, "Fallen", true)) return;
    stripFallen_ = target;
    if (n_.stripTries++ < 10) HSM_LOG("fx: stripping e%u's stand-in (its owner reports it down)", entity);
}

// The Stripping row Off (#1931): the Remove Armor a strip makes on the fighter being stripped is blocked here, whoever strips,
// so nothing comes off and nothing is sent (the stripper's hand has already let go). Spawned Armor reads null, which ends the
// strip (Willie_BP.cpp:10302). Remove Armor for anything else (a pick-up swap, our replays) runs as usual.
bool FxSync::StripKeepsArmor(ScriptCall& c) {
    const bool off = active_ && !replaying_ && env_.stripOff && env_.stripOff();
    if (!BlockStripArmor(off, stripping_, c.self())) return false;
    c.ZeroOutputs();
    uint8_t slot = 0;
    c.get("Slot", slot);
    if (n_.stripOff++ < kLogFirst)
        HSM_LOG("fx: Stripping is Off in this lobby: armour slot %u stays on (the strip took nothing) (%llu)", unsigned(slot),
                static_cast<unsigned long long>(n_.stripOff));
    return true;
}

void FxSync::OnStripEnd() {
    stripping_ = nullptr;
    if (stripFallen_) SetBool(stripFallen_, "Fallen", false);
    stripFallen_ = nullptr;
    if (stripWatch_) {
        for (int i = 0; i < 3; ++i) {
            UObject* const* w = Field<UObject*>(stripWatch_, i == 0 ? "Weapon R" : i == 1 ? "Weapon L" : "Weapon Slot Back");
            if (!stripWeapons_[i] || (w && *w == stripWeapons_[i])) continue;
            if (n_.stripWeapon++ < kLogFirst)
                HSM_LOG("fx: my fighter took the %s weapon of e%u's stand-in with Strip Armor From Bone: local only, not sent to its owner (%llu)",
                        i == 0 ? "right-hand" : i == 1 ? "left-hand" : "back", stripWatchEntity_, static_cast<unsigned long long>(n_.stripWeapon));
        }
    }
    stripWatch_ = nullptr;
}

void FxSync::SendWound(uint8_t entity, UObject* willie, FName bone, const Xform& boxWorld, const sync::V3& extent) {
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::Wound;
    m.local = boxWorld;   // world for now
    const double lim = combat::kFxMaxExtent;
    m.extent = sync::V3{Clamp(extent.x, 0.1, lim), Clamp(extent.y, 0.1, lim), Clamp(extent.z, 0.1, lim)};
    m.bodyPart = Part(Byte(willie, "Last Hit Body Part"));
    m.damageRate = Clamp(Num(willie, "Damage Rate (Additional)"), 0, combat::kFxMaxRate);
    if (Send(m, willie, bone)) ++n_.wounds;
}

// A report not sent for want of a usable bone frame; the first ones say why (test 13 dropped 250-300 a machine), and so
// does the first of every kind and reason after them (log 9: one machine dropped 299 in two bursts).
void FxSync::NoBone(const PaintFxMsg& m, int reason, const Xform* boneWorld) {
    static const char* const kWhy[] = {"the bone is not on its mesh", "too far from the bone or degenerate"};
    const uint32_t bit = 1u << ((uint32_t(m.kind) & 15u) * 2 + uint32_t(reason & 1));
    const bool firstOfKind = !(noBoneLogged_ & bit);
    noBoneLogged_ |= bit;
    if (++n_.noBone > kLogFirst && !firstOfKind) return;
    const char* why = kWhy[reason & 1];
    const Xform& x = m.local;
    const double rn = x.rot.x * x.rot.x + x.rot.y * x.rot.y + x.rot.z * x.rot.z + x.rot.w * x.rot.w;
    char frame[96] = "";   // a cut-off bone the game shrank would show here
    if (boneWorld)
        std::snprintf(frame, sizeof frame, "; the bone's world scale %.3g %.3g %.3g", boneWorld->scale.x, boneWorld->scale.y,
                      boneWorld->scale.z);
    HSM_LOG("fx: %s on e%u bone %s not sent: %s (at %.0f %.0f %.0f, rotation norm %.3f, scale %.3g %.3g %.3g%s)", KindName(m.kind),
            m.entity, m.bone.c_str(), why, x.pos.x, x.pos.y, x.pos.z, rn, x.scale.x, x.scale.y, x.scale.z, frame);
}

// Moves m's world transform and directions into bone's frame on the fighter's own Mesh and sends it. The decoder's
// bounds are applied here too, so a report the receivers would reject is never sent.
bool FxSync::Send(PaintFxMsg& m, UObject* willie, FName bone, bool directions, const std::string& label) {
    m.bone = NameToString(bone);
    if (m.bone.empty() || m.bone == "None" || m.bone.size() > combat::kMaxClaimString) { ++n_.noName; return false; }
    Xform boneWorld;
    if (!BoneXform(adapter_->Mesh(willie), bone, boneWorld)) { NoBone(m, 0); return false; }
    if (!limiter_.Allow(m.entity, m.kind, SteadyNowMs())) { ++n_.limited; return false; }
    m.local = combat::FxToBone(boneWorld, m.local);
    if (!label.empty()) m.bone = label;   // ArmorOn: the frame is the pelvis, the bone field names the class
    if (!combat::detail::LocalXformOk(m.local)) { NoBone(m, 1, &boneWorld); return false; }
    if (directions) {
        m.velocity = ClampV(boneWorld.ToLocalDir(m.velocity), combat::kFxMaxVector);
        m.power = ClampV(boneWorld.ToLocalDir(m.power), combat::kFxMaxVector);
    }
    m.seq = ++seq_;
    env_.send(m);
    if (m.kind == FxKind::Armor || n_.blood + n_.wounds + n_.pains + n_.hits < kLogFirst)
        HSM_LOG("fx: sent #%u %s on e%u bone %s part %u rate %.2f dmg rate %.2f loops %u extent %.1f", m.seq,
                KindName(m.kind), m.entity, m.bone.c_str(), m.bodyPart, m.rate, m.damageRate,
                m.loopCount, m.extent.x);
    return true;
}

bool FxSync::BoneXform(UObject* mesh, FName bone, Xform& out) const {
    if (!mesh) return false;
    Call index(mesh, "GetBoneIndex");
    if (!index.setName("BoneName", bone).invoke() || index.get<int32_t>("ReturnValue") < 0) return false;
    Call socket(mesh, "GetSocketTransform");
    socket.setName("InSocketName", bone);   // TransformSpace stays 0 in the zeroed buffer: RTS_World
    if (!socket.invoke()) return false;
    const FTransform t = socket.get<FTransform>("ReturnValue");
    if (!std::isfinite(t.translation.x) || !std::isfinite(t.rotation.w)) return false;
    out = ToXform(t);
    return true;
}

// ---------------------------------------------------------------- everyone else (a stand-in of that fighter)

// A peer's report on the receive path (host before it relays, client before it applies): the names it carries must be
// ones the receiver may look up or intern (M1, M2, M7), and a fighter's reports are limited per second (M1).
bool FxSync::Accept(const PaintFxMsg& m) {
    if (m.kind == FxKind::Voice) {
        if (!combat::ValidVoiceCueName(m.bone)) { Refuse("voice cue name", m, n_.badName); return false; }
    } else if (m.kind == FxKind::ArmorOn) {
        if (!combat::ValidArmorClassName(m.bone)) { Refuse("armour class name", m, n_.badName); return false; }
    } else if (!util::ValidBoneName(m.bone)) {
        Refuse("bone name", m, n_.badBone);
        return false;
    }
    if (!rxLimiter_.Allow(m.entity, m.kind, SteadyNowMs())) {
        if (++n_.rxLimited <= 3 || n_.rxLimited % 2000 == 0)
            HSM_LOG("fx: %s on e%u over the receive limit of %d a second (%llu dropped so far)", KindName(m.kind), m.entity,
                    combat::kFxMaxRxPerSecond, static_cast<unsigned long long>(n_.rxLimited));
        return false;
    }
    return true;
}

void FxSync::Refuse(const char* why, const PaintFxMsg& m, uint64_t& counter) {
    if (++counter > 3 && counter % 1000 != 0) return;
    HSM_LOG("fx: refused %s from e%u in a %s report (%llu so far): '%.64s'", why, m.entity, KindName(m.kind),
            static_cast<unsigned long long>(counter), util::StripControlChars(m.bone).c_str());
}

void FxSync::Forget(uint8_t entity) {
    if (entity <= kMaxWillies) crossbows_[entity] = {};
    limiter_.Forget(entity);
    rxLimiter_.Forget(entity);
    gate_.Forget(entity);
    held_.erase(std::remove_if(held_.begin(), held_.end(), [entity](const Held& h) { return h.m.entity == entity; }), held_.end());
    woundRetry_.erase(std::remove_if(woundRetry_.begin(), woundRetry_.end(), [entity](const Held& h) { return h.m.entity == entity; }),
                      woundRetry_.end());
}

UObject* FxSync::ArmorTarget(const PaintFxMsg& m) {
    if (combat::IsStripRequest(m)) return env_.ownedActor ? env_.ownedActor(m.entity) : nullptr;
    return standIns_->ActorOf(m.entity);
}

// HasAnyTasksQueuedUp on the fighter's VertexPaintDetectionComponent (REVERSE_ENGINEERING.md: check it before changing
// a painted mesh). False when the component or the call is missing: the change then goes ahead as before.
bool FxSync::PaintBusy(UObject* willie) {
    if (!willie || !adapter_) return false;
    if (!paintClassLooked_) {
        paintClassLooked_ = true;
        paintClass_ = FindClass("VertexPaintDetectionComponent");
        HSM_LOG("fx: paint component class %s (armour changes %s for paint tasks)", paintClass_ ? "ok" : "MISSING",
                paintClass_ ? "wait" : "do not wait");
    }
    if (!paintClass_) return false;
    Call get(willie, "GetComponentByClass");
    get.set<UClass*>("ComponentClass", paintClass_);
    UObject* comp = get.valid() && get.invoke() ? get.get<UObject*>("ReturnValue") : nullptr;
    if (!comp) return false;
    Call busy(comp, "HasAnyTasksQueuedUp");
    return busy.valid() && busy.invoke() && busy.get<bool>("ReturnValue");
}

void FxSync::Apply(const PaintFxMsg& m) {
    if (!active_) return;
    const double now = SteadyNowMs();
    if (ChangesArmor(m)) {
        gate_.NoteArmour(m.entity, now);
        if (held_.size() < kHeldMax) {
            const bool queued = std::any_of(held_.begin(), held_.end(), [&m](const Held& h) { return h.m.entity == m.entity; });
            gate_.Observe(m.entity, PaintBusy(ArmorTarget(m)), now);
            // Held until the paint component has been idle for a quarter second without a break, not just now (T5).
            if (queued || !gate_.Quiet(m.entity, now)) { held_.push_back({m, now}); return; }   // keeps the order per fighter
        }
    } else if (m.kind == FxKind::Blood || m.kind == FxKind::Wound) {
        // Blood and wound paint start the tasks that read the armour piece being swapped: dropped for a second after a change.
        if (gate_.BloodHeld(m.entity, now) ||
            std::any_of(held_.begin(), held_.end(), [&m](const Held& h) { return h.m.entity == m.entity; })) { ++n_.skipped; return; }
    }
    ApplyNow(m);
}

void FxSync::Tick() {
    CrossbowTick();
    if (!woundRetry_.empty()) {
        std::vector<Held> todo;
        todo.swap(woundRetry_);
        const double now = SteadyNowMs();
        for (Held& h : todo) {
            UObject* standIn = active_ ? standIns_->ActorOf(h.m.entity) : nullptr;
            if (!standIn) { ++n_.skipped; continue; }
            if (PaintFlagsBusy(standIn) && now - h.since < kWoundRetryMs) { woundRetry_.push_back(std::move(h)); continue; }
            if (PaintFlagsBusy(standIn)) {   // still refused: dropped, never counted as applied
                ++n_.skipped;
                if (n_.woundGaveUp++ < kLogFirst)
                    HSM_LOG("fx: wound on e%u dropped: its stand-in's Box Vertex Paint gate stayed shut for %.0f ms (%llu)", h.m.entity,
                            now - h.since, static_cast<unsigned long long>(n_.woundGaveUp));
                continue;
            }
            ApplyNow(h.m);
        }
    }
    if (held_.empty()) return;
    if (!active_) { held_.clear(); return; }
    const double now = SteadyNowMs();
    std::set<uint8_t> waiting;   // a fighter whose first held change still waits keeps its later ones too
    std::vector<Held> todo, keep;
    todo.swap(held_);
    for (Held& h : todo) {
        if (waiting.count(h.m.entity)) { keep.push_back(std::move(h)); continue; }
        const bool timedOut = now - h.since >= kPaintWaitMs;
        gate_.Observe(h.m.entity, PaintBusy(ArmorTarget(h.m)), now);
        if (!timedOut && !gate_.Quiet(h.m.entity, now)) { waiting.insert(h.m.entity); keep.push_back(std::move(h)); continue; }
        if (timedOut && combat::IsStripRequest(h.m) && !gate_.Quiet(h.m.entity, now)) {
            if (now - h.since < kStripWaitMaxMs) { waiting.insert(h.m.entity); keep.push_back(std::move(h)); continue; }
            ++n_.skipped;
            HSM_LOG("fx: strip of armour slot %u from my e%u dropped: its paint tasks stayed busy for %.0f ms", unsigned(h.m.rate),
                    h.m.entity, now - h.since);
            continue;
        }
        if (paintWaitsLogged_ < kPaintLogFirst)
            HSM_LOG("fx: armour change (%s) on e%u waited %.0f ms for its paint tasks%s (%u)", KindName(h.m.kind), h.m.entity,
                    now - h.since, timedOut ? ", gave up waiting" : "", ++paintWaitsLogged_);
        gate_.NoteArmour(h.m.entity, now);
        ApplyNow(h.m);
    }
    held_.swap(keep);
}

void FxSync::ApplyNow(const PaintFxMsg& m) {
    if (!active_) return;
    if (combat::IsStripRequest(m)) { ApplyStrip(m); return; }
    UObject* standIn = standIns_->ActorOf(m.entity);
    UObject* mesh = standIn ? adapter_->Mesh(standIn) : nullptr;
    if (m.kind == FxKind::Voice) {
        if (standIn) ApplyVoice(m, standIn); else ++n_.skipped;
        return;
    }
    if (m.kind == FxKind::Crossbow) {
        if (standIn) ApplyCrossbow(m, standIn); else ++n_.skipped;
        return;
    }
    const bool framedByPelvis = m.kind == FxKind::ArmorOn;
    if (!framedByPelvis && !util::ValidBoneName(m.bone)) { Refuse("bone name", m, n_.badBone); return; }   // M7: before any MakeName of it
    Xform boneWorld;
    if (!mesh || !BoneXform(mesh, MakeName(framedByPelvis ? kArmorBone : m.bone.c_str()), boneWorld)) { ++n_.skipped; return; }
    switch (m.kind) {
        case FxKind::Blood: ApplyBlood(m, standIn, mesh, boneWorld); break;
        case FxKind::Wound: ApplyWound(m, standIn, mesh, boneWorld); break;
        case FxKind::Voice: break;      // above
        case FxKind::Crossbow: break;   // above
        case FxKind::HitSound: ApplyHitSound(m, standIn, boneWorld); break;
        case FxKind::Armor: ApplyArmor(m, standIn, mesh, boneWorld); break;
        case FxKind::ArmorOn: ApplyArmorOn(m, standIn, boneWorld); break;
    }
}

// ---------------------------------------------------------------- crossbows (rev 43, combat/hsm_crossbow_fx.h)

UObject* FxSync::CrossbowIn(UObject* willie, int hand) {
    UObject** held = willie ? Field<UObject*>(willie, hand == 0 ? "Weapon R" : "Weapon L") : nullptr;
    UObject* w = held ? *held : nullptr;
    if (!w || IsGarbage(w)) return nullptr;
    const UClass* cls = ClassOf(w);
    auto it = crossbowClass_.find(cls);
    if (it == crossbowClass_.end())
        it = crossbowClass_.emplace(cls, FindProperty(ClassOf(w), "String Loaded") && FindProperty(ClassOf(w), "Bolt Loaded")).first;
    return it->second ? w : nullptr;
}

bool FxSync::ReadCrossbow(UObject* crossbow, combat::CrossbowState& out) {
    return GetBool(crossbow, "String Loaded", out.string) && GetBool(crossbow, "Bolt Loaded", out.bolt);
}

// The owner's side: each fighter simulated here, each hand that holds a crossbow. A crossbow first seen in a hand (picked
// up, given, a new fighter, a hand-off) only sets the baseline; the stand-in's own catch-up covers what it missed.
void FxSync::CrossbowTick() {
    if (!active_ || !env_.ownedActor || !adapter_) return;
    for (uint8_t e = 1; e <= kMaxWillies; ++e) {
        UObject* willie = env_.ownedActor(e);
        for (int hand = 0; hand < 2; ++hand) {
            CrossbowWatch& w = crossbows_[e][size_t(hand)];
            UObject* x = willie ? CrossbowIn(willie, hand) : nullptr;
            combat::CrossbowState now;
            if (!x || !ReadCrossbow(x, now)) { w = {}; continue; }
            if (x != w.crossbow || ObjectIndex(x) != w.index) { w = {x, ObjectIndex(x), now}; continue; }
            const auto action = combat::CrossbowEdge(w.state, now);
            w.state = now;
            if (!action || !limiter_.Allow(e, FxKind::Crossbow, SteadyNowMs())) continue;
            PaintFxMsg m;
            m.entity = e;
            m.kind = FxKind::Crossbow;
            m.bone = hand == 0 ? "hand_r" : "hand_l";
            m.bodyPart = uint8_t(*action);
            m.seq = ++seq_;
            env_.send(m);
            if (++n_.crossbow <= kLogFirst * 3)
                HSM_LOG("fx: sent #%u crossbow %s on e%u (%s hand)", m.seq, combat::CrossbowActionName(*action), e,
                        hand == 0 ? "right" : "left");
        }
    }
}

// A bolt of the stand-in's own on its crossbow: the bolt Event Add Bolt left waiting, else a new one spawned out of the
// way as the game spawns it, then Event Load Bolt with it (it needs String Loaded and no bolt; it gives the bolt the
// crossbow's holder, the stand-in, so the bolt deals no damage here). A bolt that did not seat is destroyed.
bool FxSync::LoadBolt(UObject* crossbow, UObject* standIn) {
    Call load(crossbow, "Event Load Bolt");
    if (!load.valid()) return false;
    if (!FindProperty(load.function(), "Loaded Ammo")) {   // an older crossbow: Add Bolt spawns from the quiver, Load Bolt seats it
        if (UObject** q = Field<UObject*>(standIn, "Attached Bolt Quiver"); q && *q)
            if (int32_t* n = Field<int32_t>(*q, "Projectile Count"); n && *n < 1) *n = 1;
        Call(crossbow, "Event Add Bolt").invoke();
        return load.invoke();
    }
    UObject** waiting = Field<UObject*>(crossbow, "Loaded Ammo");
    UObject* bolt = waiting && *waiting && !IsGarbage(*waiting) ? *waiting : nullptr;
    const bool spawned = !bolt;
    if (spawned) {
        UClass* cls = adapter_->CrossbowBoltClass();
        bolt = cls ? adapter_->SpawnActorAt(cls, FVector{0, 0, -100000}, FRotator{}) : nullptr;
        if (!bolt) {
            if (!crossbowFailLogged_)
                HSM_LOG("fx: crossbow bolt class %s: stand-ins' crossbows show no bolt", cls ? "spawn failed" : "missing");
            crossbowFailLogged_ = true;
            return false;
        }
    }
    load.set<UObject*>("Loaded Ammo", bolt);
    load.invoke();
    combat::CrossbowState s;
    if (ReadCrossbow(crossbow, s) && s.bolt) return true;
    if (spawned) adapter_->DestroyActor(bolt);
    return false;
}

// Everyone else: the stand-in's crossbow in the owner's hand (or the other one) runs what it needs to match the owner.
void FxSync::ApplyCrossbow(const PaintFxMsg& m, UObject* standIn) {
    const int hand = m.bone == "hand_l" ? 1 : 0;
    UObject* x = CrossbowIn(standIn, hand);
    if (!x) x = CrossbowIn(standIn, 1 - hand);
    combat::CrossbowState s;
    if (!x || !ReadCrossbow(x, s)) {
        ++n_.skipped;
        if (n_.skipped <= 3) HSM_LOG("fx: #%u crossbow on e%u: its stand-in holds no crossbow", m.seq, m.entity);
        return;
    }
    const auto action = combat::CrossbowAction(m.bodyPart);
    const std::vector<combat::CrossbowStep> steps = combat::PlanCrossbowReplay(s, action);
    ScriptHookBypass bypass;   // our own calls
    bool ok = true;
    for (combat::CrossbowStep step : steps) {
        switch (step) {
            case combat::CrossbowStep::LoadString: {
                Call string(x, "Event Load String");
                if (string.valid()) string.invoke();
                combat::CrossbowState now;
                if (!ReadCrossbow(x, now) || !now.string) ok &= SetBool(x, "String Loaded", true);   // Block String Loading held it
                break;
            }
            case combat::CrossbowStep::LoadBolt: ok &= LoadBolt(x, standIn); break;
            case combat::CrossbowStep::Shoot: ok &= Call(x, "Event Shoot").invoke(); break;
        }
    }
    ++n_.crossbowReplayed;
    if (!ok) { Count(false, m); return; }
    ++n_.applied;
    if (n_.crossbowReplayed <= kLogFirst * 3)
        HSM_LOG("fx: replayed #%u crossbow %s on the stand-in of e%u (%zu step(s), was string %d bolt %d)", m.seq,
                combat::CrossbowActionName(action), m.entity, steps.size(), int(s.string), int(s.bolt));
}

// The owner's cue on the stand-in's own Voice Audio, then its Helmet Muffle Voice (the low-pass a helmet puts on it).
void FxSync::ApplyVoice(const PaintFxMsg& m, UObject* standIn) {
    // M1: only a stock cue name is ever looked up (the lookup scans every loaded object). A name that did not resolve is
    // remembered for a minute, and new names are scanned for at most 4 a second, so a peer cannot turn messages into scans.
    if (!combat::ValidVoiceCueName(m.bone)) { Refuse("voice cue name", m, n_.badName); Count(false, m); return; }
    UObject* const* audio = Field<UObject*>(standIn, "Voice Audio");
    // Clamp rate to safe range
    double rate = Clamp(m.rate, kVoiceRateMin, kVoiceRateMax);
    const double now = SteadyNowMs();
    UObject* sound = nullptr;
    const combat::NameCache<ObjRef>::Entry* known = voiceCues_.Find(m.bone, now);
    if (known && known->found && adapter_->StillValid(known->value.obj, known->value.index)) sound = known->value.obj;
    else if (known && !known->found) { ++n_.skipped; return; }   // looked up and not there: no second scan
    else {
        if (!cueScans_.Allow(now)) { ++n_.scanLimited; return; }
        sound = FindObject(m.bone, combat::IsVoiceWaveName(m.bone) ? "SoundWave" : "SoundCue");   // Gulp, Spit: wave assets (VO_Gulp, Willie_BP.cpp:31005)
        voiceCues_.Put(m.bone, sound ? ObjRef{sound, ObjectIndex(sound)} : ObjRef{}, sound != nullptr, now);
    }
    if (!audio || !*audio || !sound) { Count(false, m); return; }
    Call(*audio, "SetSound").set<UObject*>("NewSound", sound).invoke();
    Call(*audio, "SetVolumeMultiplier").set<float>("NewVolumeMultiplier", float(rate)).invoke();
    Call play(*audio, "Play");
    bool ok = play.set<float>("StartTime", 0.0f).invoke();
    {
        ScriptHookBypass bypass;   // our own call
        Call(standIn, "Helmet Muffle Voice").invoke();
    }
    Count(ok, m);
}

// A strip request for a fighter simulated here: another machine's fighter stripped our fallen fighter's stand-in
// there. Honoured only while it really is fallen and the slot is not in its Block Undress Slots (Strip Armor From Bone's
// own per-slot gate, Willie_BP.cpp:10298); the Remove Armor it runs is then reported to everyone as usual.
void FxSync::ApplyStrip(const PaintFxMsg& m) {
    UObject* own = env_.ownedActor ? env_.ownedActor(m.entity) : nullptr;
    if (!own || !Flag(own, "Fallen")) { ++n_.skipped; return; }
    if (env_.stripOff && env_.stripOff()) {   // the Stripping row Off: the stripper's machine keeps it on too (StripKeepsArmor)
        ++n_.skipped;
        if (n_.stripOff++ < kLogFirst)
            HSM_LOG("fx: strip of armour slot %u from my fallen e%u refused: Stripping is Off in this lobby (%llu)", unsigned(m.rate), m.entity,
                    static_cast<unsigned long long>(n_.stripOff));
        return;
    }
    if (UndressBlocked(own, uint8_t(m.rate))) {
        ++n_.skipped;
        if (n_.stripBlocked++ < kLogFirst)
            HSM_LOG("fx: strip of armour slot %u from my fallen e%u refused: the slot is in its Block Undress Slots (a limb is off) (%llu)",
                    unsigned(m.rate), m.entity, static_cast<unsigned long long>(n_.stripBlocked));
        return;
    }
    UObject* mesh = adapter_->Mesh(own);
    Xform boneWorld;
    if (!mesh || !BoneXform(mesh, MakeName(kArmorBone), boneWorld)) { ++n_.skipped; return; }
    Call remove(own, "Remove Armor");
    remove.set<FTransform>("SpawnTransform", ToF(combat::FxFromBone(boneWorld, m.local))).set<uint8_t>("Slot", uint8_t(m.rate))
        .set<UObject*>("Disable Collision With", mesh);
    bool ok = false;
    {
        ScriptHookBypass bypass;   // our own call (the Remove Armor observer still reports it)
        ok = remove.valid() && remove.invoke();
    }
    HSM_LOG("fx: armour slot %u stripped from my fallen e%u at another machine's request%s", unsigned(m.rate), m.entity,
            ok ? "" : ": FAILED");
    Count(ok, m);
}

// The nearest loose piece of the owner's class to where the owner's lay, put on the stand-in by its own Equip Armor
// (Willie_BP.cpp:4623, build 26568321: it reads no Player flag, so none is raised any more).
void FxSync::ApplyArmorOn(const PaintFxMsg& m, UObject* standIn, const Xform& boneWorld) {
    // M2: only the armour Blueprint family is searched for (then handed to a Blueprint call on the stand-in). A class no
    // loaded object has is remembered for 5 s, and walks are limited to 10 a second.
    if (!combat::ValidArmorClassName(m.bone)) { Refuse("armour class name", m, n_.badName); Count(false, m); return; }
    const double now = SteadyNowMs();
    if (missingArmor_.Find(m.bone, now)) { ++n_.skipped; return; }
    if (!armorWalks_.Allow(now)) { ++n_.scanLimited; return; }
    const sync::V3 at = boneWorld.FromLocal(m.local.pos);
    bool classExists = false;
    UObject* best = nullptr;
    double bestD = kArmorMatchUu * kArmorMatchUu;
    std::unordered_map<UClass*, bool> ofClass;   // each class named once per pass, not each object
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o)) continue;
        auto cls = ofClass.find(ClassOf(o));
        if (cls == ofClass.end()) cls = ofClass.emplace(ClassOf(o), ObjectName(reinterpret_cast<UObject*>(ClassOf(o))) == m.bone).first;
        if (!cls->second) continue;
        classExists = true;
        Call parent(o, "GetAttachParentActor");
        if (!parent.invoke() || parent.get<UObject*>("ReturnValue")) continue;   // worn or carried already
        const sync::V3 p = ToV(adapter_->ActorLocation(o));
        const double d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) + (p.z - at.z) * (p.z - at.z);
        if (d < bestD) { bestD = d; best = o; }
    }
    if (!classExists) missingArmor_.PutMiss(m.bone, now);   // no object of that class at all: not looked for again for a while
    if (!best) {
        HSM_LOG("fx: no loose %s within %.0f uu of the owner's for the stand-in of e%u", m.bone.c_str(), kArmorMatchUu, m.entity);
        Count(false, m);
        return;
    }
    bool staticMesh = false;
    GetBool(best, "World Mesh Static", staticMesh);
    UObject* const* piece = Field<UObject*>(best, staticMesh ? "Armor Mesh Static" : "Armor Mesh Skeletal");
    Call pick(standIn, "Equip Armor");
    pick.set<UObject*>("Picked Armor", piece ? *piece : nullptr).set<UObject*>("Armor Actor", best);
    bool ok = false;
    {
        ScriptHookBypass bypass;   // our own call
        replaying_ = true;         // its swap-out Remove Armor is the owner's to report
        ok = pick.valid() && pick.invoke();
        replaying_ = false;
    }
    // Picked Up is the game's own verdict (a blocked slot or missing arming points leave it false)
    if (ok && FindProperty(pick.function(), "Picked Up") && !pick.get<bool>("Picked Up")) ok = false;
    HSM_LOG("fx: %s put on the stand-in of e%u%s", m.bone.c_str(), m.entity, ok ? "" : ": FAILED");
    if (ok && env_.gearChanged) env_.gearChanged(m.entity);
    Count(ok, m);
}

// Get Damage's blunt and bone cues at the hit, on the stand-in's bone (the stand-in's own Get Damage stops at
// Invulnerable before it plays them).
void FxSync::ApplyHitSound(const PaintFxMsg& m, UObject* standIn, const Xform& boneWorld) {
    const combat::HitSoundPlan plan = combat::PlanHitSound(m.velocity.x, m.velocity.y, m.extraVelocity, false);
    const sync::V3 at = boneWorld.FromLocal(m.local.pos);
    bool ok = true;
    if (const char* cue = BluntCueName(plan.blunt)) ok &= PlaySoundAt(standIn, cue, at, plan.bluntVolume);
    if (plan.boneVolume > 0) ok &= PlaySoundAt(standIn, kBoneCue, at, plan.boneVolume);
    Count(ok, m);
}

// The stand-in's own Remove Armor of the owner's slot at the owner's spawn spot: the piece leaves the stand-in, and the
// loose one it spawns is where the owner's lies, so the prop census pairs them.
void FxSync::ApplyArmor(const PaintFxMsg& m, UObject* standIn, UObject* mesh, const Xform& boneWorld) {
    if (stripped_.erase({m.entity, uint8_t(m.rate)})) { Count(true, m); return; }   // this machine stripped it already
    Xform spawn = combat::FxFromBone(boneWorld, m.local);
    Call remove(standIn, "Remove Armor");
    remove.set<FTransform>("SpawnTransform", ToF(spawn)).set<uint8_t>("Slot", uint8_t(m.rate))
        .set<UObject*>("Disable Collision With", mesh);
    bool ok = false;
    {
        ScriptHookBypass bypass;   // our own call
        replaying_ = true;
        ok = remove.valid() && remove.invoke();
        replaying_ = false;
    }
    HSM_LOG("fx: armour slot %u removed from the stand-in of e%u (#%u)%s", unsigned(m.rate), m.entity, m.seq, ok ? "" : ": FAILED");
    if (ok && env_.noteSpawn) env_.noteSpawn();   // the same loose piece, on this machine
    Count(ok, m);
}

bool FxSync::PlaySoundAt(UObject* standIn, const char* cue, const sync::V3& at, double volume) {
    static UObject* statics = nullptr;
    if (!statics) if (UObject* c = FindObject("GameplayStatics", "Class")) statics = DefaultObject(reinterpret_cast<UClass*>(c));
    UObject*& sound = cues_[cue];
    if (!sound) sound = FindObject(cue, "SoundCue");   // assets: they stay loaded with the fighter blueprint
    if (!statics || !sound) {
        if (!cueFailLogged_) HSM_LOG("fx: sound cue %s not found; hit sounds are not played", cue);
        cueFailLogged_ = true;
        return false;
    }
    Call play(statics, "PlaySoundAtLocation");
    play.set<UObject*>("WorldContextObject", standIn).set<UObject*>("Sound", sound).set<FVector>("Location", ToF(at))
        .set<float>("VolumeMultiplier", float(volume)).set<float>("PitchMultiplier", 1.0f);
    return play.valid() && play.invoke();
}

// The stand-in's own Spawn Blood, its gates set to the owner's state first so it decides as the owner's did, and its
// Bleeding (which scales the loop count) the owner's for the call.
void FxSync::ApplyBlood(const PaintFxMsg& m, UObject* standIn, UObject* mesh, const Xform& boneWorld) {
    double* bleeding = Field<double>(standIn, "Bleeding");
    double* lastRate = Field<double>(standIn, "Last Spawned Blood Rate");
    double* spRate = Field<double>(standIn, "Spawn Blood Sp Rate");
    uint8_t* part = Field<uint8_t>(standIn, "Spawn Blood Body Part");
    if (lastRate) *lastRate = m.gateLastRate;
    if (spRate) *spRate = m.gateSpRate;
    if (part) *part = m.gatePart;
    const double keepBleeding = bleeding ? *bleeding : 0.0;
    if (bleeding) *bleeding = m.bleeding;
    Call blood(standIn, "Spawn Blood");
    blood.set<FTransform>("Transform", ToF(combat::FxFromBone(boneWorld, m.local)))
        .set<double>("Spawn Rate", m.rate)
        .setName("Bone", MakeName(m.bone))
        .set<uint8_t>("Body Partt", m.bodyPart)
        .set<UObject*>("Body Mesh", mesh)
        .set<int32_t>("Loop Count", m.loopCount)
        .set<bool>("Extra Velocity", m.extraVelocity)
        .set<FVector>("Velocity", ToF(boneWorld.FromLocalDir(m.velocity)))
        .set<FVector>("Pover", ToF(boneWorld.FromLocalDir(m.power)))
        .set<bool>("Skip Check", false);
    bool ok = false;
    {
        ScriptHookBypass bypass;   // our own call
        ok = blood.valid() && blood.invoke();
    }
    if (bleeding) *bleeding = keepBleeding;
    Count(ok, m);
}

// The stand-in's own Box Vertex Paint over a private box at the owner's wound, with the owner's body part and damage
// rate, and Invulnerable (its first gate) lowered for the one call. The rig sets Invulnerable again every frame anyway.
void FxSync::ApplyWound(const PaintFxMsg& m, UObject* standIn, UObject* mesh, const Xform& boneWorld) {
    // Box Vertex Paint's first gate also refuses while the stand-in dismembers, swaps armour, sets armour up or has its hat
    // flag set (Willie_BP.cpp:2206): the call would return with nothing painted. The wound waits for the gate (Tick) for
    // up to kWoundRetryMs and is not counted as applied; one still refused then is dropped (skipped).
    if (PaintFlagsBusy(standIn)) {
        if (woundRetry_.size() < kWoundRetryMax) {
            if (n_.woundDeferred++ < kLogFirst)
                HSM_LOG("fx: wound on e%u waits: its stand-in is dismembering, swapping or setting up armour (retries up to %.0f ms) (%llu)",
                        m.entity, kWoundRetryMs, static_cast<unsigned long long>(n_.woundDeferred));
            woundRetry_.push_back({m, SteadyNowMs()});
        } else {
            ++n_.skipped;
        }
        return;
    }
    UObject* box = PaintBox(m.entity, standIn);
    if (!box) { ++n_.failed; return; }
    const Xform world = combat::FxFromBone(boneWorld, m.local);
    Call place(box, "K2_SetWorldTransform");
    place.set<FTransform>("NewTransform", ToF(world)).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
    Call(box, "SetBoxExtent").set<FVector>("InBoxExtent", ToF(m.extent)).set<bool>("bUpdateOverlaps", false).invoke();
    uint8_t* part = Field<uint8_t>(standIn, "Last Hit Body Part");
    double* rate = Field<double>(standIn, "Damage Rate (Additional)");
    const uint8_t keepPart = part ? *part : 0;
    const double keepRate = rate ? *rate : 0.0;
    if (part) *part = m.bodyPart;
    if (rate) *rate = m.damageRate;
    Call paint(standIn, "Box Vertex Paint");
    paint.set<UObject*>("Box", box).setName("Hit Bone", MakeName(m.bone)).set<UObject*>("Hit Primitive Component", mesh);
    bool ok = false;
    if (SetBool(standIn, "Invulnerable", false)) {
        {
            ScriptHookBypass bypass;   // our own call
            ok = paint.valid() && paint.invoke();
        }
        SetBool(standIn, "Invulnerable", true);
    }
    if (part) *part = keepPart;
    if (rate) *rate = keepRate;
    Count(ok, m);
}

void FxSync::Count(bool ok, const PaintFxMsg& m) {
    if (!ok) {
        ++n_.failed;
        if (n_.failed <= 3) HSM_LOG("fx: #%u %s on e%u could not be replayed", m.seq, KindName(m.kind), m.entity);
        return;
    }
    ++n_.applied;
    if (n_.applied <= kLogFirst)
        HSM_LOG("fx: replayed #%u %s on the stand-in of e%u bone %s", m.seq, KindName(m.kind), m.entity,
                m.bone.c_str());
}

// One private BoxComponent per stand-in, collision off before it is registered (the claims Hit Box pattern): Box Vertex
// Paint uses it only for its geometry ("is within" tests).
UObject* FxSync::PaintBox(uint8_t entity, UObject* standIn) {
    if (!boxClass_) return nullptr;
    Box& b = boxes_[entity];
    if (b.box && b.actor == standIn && adapter_->StillValid(b.actor, b.actorIndex) && adapter_->StillValid(b.box, b.boxIndex)) return b.box;
    b = Box{};
    const FTransform identity{};
    Call add(standIn, "AddComponentByClass");
    add.set<UClass*>("Class", boxClass_).set<bool>("bManualAttachment", false).set<FTransform>("RelativeTransform", identity)
        .set<bool>("bDeferredFinish", true);
    UObject* box = add.invoke() ? add.get<UObject*>("ReturnValue") : nullptr;
    if (!box) {
        if (!boxFailLogged_) HSM_LOG("fx: could not add a paint box to a stand-in; wounds are not shown");
        boxFailLogged_ = true;
        return nullptr;
    }
    Call(box, "SetCollisionEnabled").set<uint8_t>("NewType", 0).invoke();   // ECollisionEnabled::NoCollision
    Call(box, "SetGenerateOverlapEvents").set<bool>("bInGenerateOverlapEvents", false).invoke();
    Call(standIn, "FinishAddComponent").set<UObject*>("Component", box).set<bool>("bManualAttachment", false)
        .set<FTransform>("RelativeTransform", identity).invoke();
    b = Box{standIn, ObjectIndex(standIn), box, ObjectIndex(box)};
    HSM_LOG("fx: private paint box %s made on the stand-in of e%u", ObjectName(box).c_str(), entity);
    return box;
}

std::string FxSync::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    char line[512];
    std::snprintf(line, sizeof line, "fx: sent blood %llu wounds %llu pain %llu hits %llu armour %llu crossbow %llu (limited %llu nobone %llu noname %llu passed-on %llu) | replayed %llu (crossbow %llu, skipped %llu failed %llu) | refused rx-limit %llu name %llu bone %llu scan-limit %llu",
                  d(a.blood, b.blood), d(a.wounds, b.wounds), d(a.pains, b.pains), d(a.hits, b.hits), d(a.armor, b.armor), d(a.crossbow, b.crossbow), d(a.limited, b.limited), d(a.noBone, b.noBone), d(a.noName, b.noName), d(a.passedOn, b.passedOn),
                  d(a.applied, b.applied), d(a.crossbowReplayed, b.crossbowReplayed),
                  d(a.skipped, b.skipped), d(a.failed, b.failed), d(a.rxLimited, b.rxLimited), d(a.badName, b.badName), d(a.badBone, b.badBone),
                  d(a.scanLimited, b.scanLimited));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game

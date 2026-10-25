#include "gameplay/fx_sync.h"
#include "log.h"
#include "ue/script_hook.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hsm::game {

using namespace hsm::ue;
using combat::FxKind;
using combat::PaintFxMsg;
using combat::Xform;

namespace {

constexpr const char* kHookOwner = "fxsync";
constexpr uint64_t kLogFirst = 10;   // the first reports and replays are logged in full (test evidence)

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

const char* KindName(FxKind k) {
    switch (k) {
        case FxKind::Blood: return "blood";
        case FxKind::Wound: return "wound";
        case FxKind::Voice: return "voice";
        case FxKind::HitSound: return "hit sound";
        case FxKind::Armor: return "armour removal";
        case FxKind::ArmorOn: return "armour put on";
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
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Pick Up Armor", {}, [this](ScriptCall& c) { OnPickUpArmor(c); });
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Remove Armor", {}, [this](ScriptCall& c) { OnRemoveArmor(c); });
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
// nothing when Damage Raw Scaled stayed 0 (any of its early gates, Invulnerable first) or vertex paint is off.
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
    c.get("Location", location);
    c.get("Impulse", impulse);
    const double r = combat::WoundRadius(drs, Len(impulse));
    Xform at;
    at.pos = ToV(location);
    SendWound(entity, self, bone, at, sync::V3{r, r, r});
}

// Spawn Blood on a fighter simulated here, seen before its body: the call as made plus the state its gates read (they
// change it as the body runs). Get Damage is its only caller, so every blood burst of a real blow passes here.
void FxSync::OnSpawnBlood(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity || Flag(self, "Is Zombie?")) return;
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

// Box Vertex Paint on a fighter simulated here (a stuck stab's wound), seen once it returned: it painted when the
// fighter was not Invulnerable (its first gate; vertex paint off is its second, rechecked by the replay).
void FxSync::OnBoxVertexPaint(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity || Flag(self, "Invulnerable")) return;
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
    if (m.bone.empty() || m.bone.size() > combat::kMaxClaimString) { ++n_.noName; return; }
    if (!limiter_.Allow(m.entity, m.kind, SteadyNowMs())) { ++n_.limited; return; }
    m.seq = ++seq_;
    env_.send(m);
    ++n_.pains;
}

// Pick Up Armor on a fighter simulated here, seen once it returned: Picked Up says whether the piece went on.
void FxSync::OnPickUpArmor(ScriptCall& c) {
    if (!active_) return;
    UObject* self = c.self();
    const uint8_t entity = env_.ownedEntity(self);
    if (!entity) return;
    bool picked = false;
    UObject* actor = nullptr;
    if (!c.getBool("Picked Up", picked) || !picked || !c.get("Armor Actor", actor) || !actor) return;
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::ArmorOn;
    m.local.pos = ToV(adapter_->ActorLocation(actor));   // world for now (the piece, carried in a hand)
    if (Send(m, self, MakeName(kArmorBone), false, ObjectName(reinterpret_cast<UObject*>(ClassOf(actor))))) ++n_.armor;
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
    PaintFxMsg m;
    m.entity = entity;
    m.kind = FxKind::Armor;
    m.rate = slot;
    m.extraVelocity = strip;
    m.local = ToXform(t);   // world for now
    if (strip) stripped_.insert({entity, slot});   // the owner's report of it is not replayed again here
    if (Send(m, self, MakeName(kArmorBone), false)) ++n_.armor;
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

// A report not sent for want of a usable bone frame; the first ones say why (test 13 dropped 250-300 a machine).
void FxSync::NoBone(const PaintFxMsg& m, const char* why) {
    if (++n_.noBone > kLogFirst) return;
    const Xform& x = m.local;
    const double rn = x.rot.x * x.rot.x + x.rot.y * x.rot.y + x.rot.z * x.rot.z + x.rot.w * x.rot.w;
    HSM_LOG("fx: %s on e%u bone %s not sent: %s (at %.0f %.0f %.0f, rotation norm %.3f, scale %.3g %.3g %.3g)", KindName(m.kind),
            m.entity, m.bone.c_str(), why, x.pos.x, x.pos.y, x.pos.z, rn, x.scale.x, x.scale.y, x.scale.z);
}

// Moves m's world transform and directions into bone's frame on the fighter's own Mesh and sends it. The decoder's
// bounds are applied here too, so a report the receivers would reject is never sent.
bool FxSync::Send(PaintFxMsg& m, UObject* willie, FName bone, bool directions, const std::string& label) {
    m.bone = NameToString(bone);
    if (m.bone.empty() || m.bone == "None" || m.bone.size() > combat::kMaxClaimString) { ++n_.noName; return false; }
    Xform boneWorld;
    if (!BoneXform(adapter_->Mesh(willie), bone, boneWorld)) { NoBone(m, "the bone is not on its mesh"); return false; }
    if (!limiter_.Allow(m.entity, m.kind, SteadyNowMs())) { ++n_.limited; return false; }
    m.local = combat::FxToBone(boneWorld, m.local);
    if (!label.empty()) m.bone = label;   // ArmorOn: the frame is the pelvis, the bone field names the class
    if (!combat::detail::LocalXformOk(m.local)) { NoBone(m, "too far from the bone or degenerate"); return false; }
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

void FxSync::Apply(const PaintFxMsg& m) {
    if (!active_) return;
    if (combat::IsStripRequest(m)) { ApplyStrip(m); return; }
    UObject* standIn = standIns_->ActorOf(m.entity);
    UObject* mesh = standIn ? adapter_->Mesh(standIn) : nullptr;
    if (m.kind == FxKind::Voice) {
        if (standIn) ApplyVoice(m, standIn); else ++n_.skipped;
        return;
    }
    const bool framedByPelvis = m.kind == FxKind::ArmorOn;
    Xform boneWorld;
    if (!mesh || !BoneXform(mesh, MakeName(framedByPelvis ? kArmorBone : m.bone.c_str()), boneWorld)) { ++n_.skipped; return; }
    switch (m.kind) {
        case FxKind::Blood: ApplyBlood(m, standIn, mesh, boneWorld); break;
        case FxKind::Wound: ApplyWound(m, standIn, mesh, boneWorld); break;
        case FxKind::Voice: break;   // above
        case FxKind::HitSound: ApplyHitSound(m, standIn, boneWorld); break;
        case FxKind::Armor: ApplyArmor(m, standIn, mesh, boneWorld); break;
        case FxKind::ArmorOn: ApplyArmorOn(m, standIn, boneWorld); break;
    }
}

// The owner's cue on the stand-in's own Voice Audio, then its Helmet Muffle Voice (the low-pass a helmet puts on it).
void FxSync::ApplyVoice(const PaintFxMsg& m, UObject* standIn) {
    UObject* const* audio = Field<UObject*>(standIn, "Voice Audio");
    UObject*& sound = cues_[m.bone];
    if (!sound) sound = FindObject(m.bone, "SoundCue");
    if (!audio || !*audio || !sound) { Count(false, m); return; }
    Call(*audio, "SetSound").set<UObject*>("NewSound", sound).invoke();
    Call(*audio, "SetVolumeMultiplier").set<float>("NewVolumeMultiplier", float(m.rate)).invoke();
    Call play(*audio, "Play");
    bool ok = play.set<float>("StartTime", 0.0f).invoke();
    {
        ScriptHookBypass bypass;   // our own call
        Call(standIn, "Helmet Muffle Voice").invoke();
    }
    Count(ok, m);
}

// A strip request for a fighter simulated here: another machine's fighter stripped our fallen fighter's stand-in
// there. Honoured only while it really is fallen (Strip Armor From Bone's own gate); the Remove Armor it runs is then
// reported to everyone as usual.
void FxSync::ApplyStrip(const PaintFxMsg& m) {
    UObject* own = env_.ownedActor ? env_.ownedActor(m.entity) : nullptr;
    if (!own || !Flag(own, "Fallen")) { ++n_.skipped; return; }
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

// The nearest loose piece of the owner's class to where the owner's lay, put on the stand-in by its own Pick Up Armor
// (Player raised for the call: it is the function's first gate).
void FxSync::ApplyArmorOn(const PaintFxMsg& m, UObject* standIn, const Xform& boneWorld) {
    const sync::V3 at = boneWorld.FromLocal(m.local.pos);
    UObject* best = nullptr;
    double bestD = kArmorMatchUu * kArmorMatchUu;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || IsGarbage(o) || ObjectName(reinterpret_cast<UObject*>(ClassOf(o))) != m.bone) continue;
        Call parent(o, "GetAttachParentActor");
        if (!parent.invoke() || parent.get<UObject*>("ReturnValue")) continue;   // worn or carried already
        const sync::V3 p = ToV(adapter_->ActorLocation(o));
        const double d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) + (p.z - at.z) * (p.z - at.z);
        if (d < bestD) { bestD = d; best = o; }
    }
    if (!best) {
        HSM_LOG("fx: no loose %s within %.0f uu of the owner's for the stand-in of e%u", m.bone.c_str(), kArmorMatchUu, m.entity);
        Count(false, m);
        return;
    }
    bool staticMesh = false;
    GetBool(best, "World Mesh Static", staticMesh);
    UObject* const* piece = Field<UObject*>(best, staticMesh ? "Armor Mesh Static" : "Armor Mesh Skeletal");
    Call pick(standIn, "Pick Up Armor");
    pick.set<UObject*>("Picked Armor", piece ? *piece : nullptr).set<UObject*>("Armor Actor", best);
    bool wasPlayer = false;
    GetBool(standIn, "Player", wasPlayer);
    SetBool(standIn, "Player", true);
    bool ok = false;
    {
        ScriptHookBypass bypass;   // our own call
        replaying_ = true;         // its swap-out Remove Armor is the owner's to report
        ok = pick.valid() && pick.invoke();
        replaying_ = false;
    }
    SetBool(standIn, "Player", wasPlayer);
    HSM_LOG("fx: %s put on the stand-in of e%u%s", m.bone.c_str(), m.entity, ok ? "" : ": FAILED");
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
    char line[320];
    std::snprintf(line, sizeof line, "fx: sent blood %llu wounds %llu pain %llu hits %llu armour %llu (limited %llu nobone %llu noname %llu) | replayed %llu (skipped %llu failed %llu)",
                  d(a.blood, b.blood), d(a.wounds, b.wounds), d(a.pains, b.pains), d(a.hits, b.hits), d(a.armor, b.armor), d(a.limited, b.limited), d(a.noBone, b.noBone), d(a.noName, b.noName),
                  d(a.applied, b.applied),
                  d(a.skipped, b.skipped), d(a.failed, b.failed));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game

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

}  // namespace

bool FxSync::Start(Adapter& adapter, StandIns& standIns, Env env) {
    Stop();
    adapter_ = &adapter;
    standIns_ = &standIns;
    env_ = std::move(env);
    boxClass_ = FindClass("BoxComponent");
    if (UClass* willie = FindClass("Willie_BP_C")) {
        for (const char* name : {"Get Damage", "Spawn Blood"}) {
            UFunction* fn = FindFunction(willie, name);
            FProperty* bone = fn ? FindProperty(fn, "Bone") : nullptr;
            HSM_LOG("fx: %s Bone binding=%s offset=%d size=%d", name,
                    bone ? PropertyName(bone).c_str() : "MISSING", bone ? PropertyOffset(bone) : -1,
                    bone ? PropertySize(bone) : 0);
        }
    }
    int n = ObserveScript(kHookOwner, "Willie_BP_C", "Get Damage", {}, [this](ScriptCall& c) { OnGetDamage(c); });
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Spawn Blood", [this](ScriptCall& c) { OnSpawnBlood(c); });
    n += ObserveScript(kHookOwner, "Willie_BP_C", "Box Vertex Paint", {}, [this](ScriptCall& c) { OnBoxVertexPaint(c); });
    active_ = ScriptHookActive();
    HSM_LOG("fx: %s (%d function(s) observed now; box class %s)",
            active_ ? "on: blood and wounds on fighters simulated here are shown on every stand-in of them"
                    : "NOT ACTIVE (script VM hook unavailable): stand-ins show no blood or wounds",
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
    if (Flag(self, "Force Disable Vertex Paint")) return;
    FName bone{};
    if (!c.get("Bone", bone)) { ++n_.noBone; return; }
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
    if (!c.get("Bone", bone)) { ++n_.noBone; return; }
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

// Moves m's world transform and directions into bone's frame on the fighter's own Mesh and sends it. The decoder's
// bounds are applied here too, so a report the receivers would reject is never sent.
bool FxSync::Send(PaintFxMsg& m, UObject* willie, FName bone) {
    m.bone = NameToString(bone);
    if (m.bone.empty() || m.bone == "None" || m.bone.size() > combat::kMaxClaimString) return false;
    Xform boneWorld;
    if (!BoneXform(adapter_->Mesh(willie), bone, boneWorld)) { ++n_.noBone; return false; }
    if (!limiter_.Allow(m.entity, m.kind, SteadyNowMs())) { ++n_.limited; return false; }
    m.local = combat::FxToBone(boneWorld, m.local);
    if (!combat::detail::LocalXformOk(m.local)) { ++n_.noBone; return false; }   // too far from the bone to stand for it
    m.velocity = ClampV(boneWorld.ToLocalDir(m.velocity), combat::kFxMaxVector);
    m.power = ClampV(boneWorld.ToLocalDir(m.power), combat::kFxMaxVector);
    m.seq = ++seq_;
    env_.send(m);
    if (n_.blood + n_.wounds < kLogFirst)
        HSM_LOG("fx: sent #%u %s on e%u bone %s part %u rate %.2f dmg rate %.2f loops %u extent %.1f", m.seq,
                m.kind == FxKind::Blood ? "blood" : "wound", m.entity, m.bone.c_str(), m.bodyPart, m.rate, m.damageRate,
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
    UObject* standIn = standIns_->ActorOf(m.entity);
    UObject* mesh = standIn ? adapter_->Mesh(standIn) : nullptr;
    Xform boneWorld;
    if (!mesh || !BoneXform(mesh, MakeName(m.bone), boneWorld)) { ++n_.skipped; return; }
    if (m.kind == FxKind::Blood) ApplyBlood(m, standIn, mesh, boneWorld);
    else ApplyWound(m, standIn, mesh, boneWorld);
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
        if (n_.failed <= 3) HSM_LOG("fx: #%u %s on e%u could not be replayed", m.seq, m.kind == FxKind::Blood ? "blood" : "wound", m.entity);
        return;
    }
    ++n_.applied;
    if (n_.applied <= kLogFirst)
        HSM_LOG("fx: replayed #%u %s on the stand-in of e%u bone %s", m.seq, m.kind == FxKind::Blood ? "blood" : "wound", m.entity,
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
    char line[256];
    std::snprintf(line, sizeof line, "fx: sent blood %llu wounds %llu (limited %llu nobone %llu) | replayed %llu (skipped %llu failed %llu)",
                  d(a.blood, b.blood), d(a.wounds, b.wounds), d(a.limited, b.limited), d(a.noBone, b.noBone), d(a.applied, b.applied),
                  d(a.skipped, b.skipped), d(a.failed, b.failed));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game

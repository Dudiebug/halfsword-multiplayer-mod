#include "gameplay/standin_rig.h"
#include "sync/hsm_grip.h"
#include "util/hsm_names.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <string>

namespace hsm::game {
using namespace hsm::ue;
using sync::Q4;
using sync::V3;

namespace {
struct FObjArray { UObject** data = nullptr; int32_t num = 0, max = 0; };

// Willie_BP_C fields Neutralise writes every frame: HSMP main.lua PX.TONUS, neutralise_bp (Head Tonus, the root
// constraint powers), PAIN_ZERO, MUSCLE_FLAGS and PAIN_FALSE, plus the plan's damage backstops (Invulnerable, Force
// Disable Dismemberment).
constexpr const char* kStandInZeroDoubleFields[] = {
    "All Body Tonus", "Upper Body Tonus", "Arm R Tonus", "Arm L Tonus", "Leg R Tonus", "Leg L Tonus", "Muscle Power",
    "Head Tonus", "Root Angular Constraint Power", "Root Linear Constraint Power",
    "Pain Shock Rate", "Pain Upper Body", "Pain Lower Body", "Pain Neck", "Pain Head", "Pain Arm R", "Pain Arm L",
    "Pain Leg R", "Pain Leg L", "Pain Grab Rate", "Pain L Arm Alpha", "Pain R Arm Alpha"};
constexpr const char* kStandInTrueBoolFields[] = {
    "Block Body Muscles", "Block Upper Leg Muscles", "Block Lower Leg Muscles", "Block Shoulder Muscles",
    "Block Arm Muscles", "Invulnerable", "Force Disable Dismemberment"};
constexpr const char* kStandInFalseBoolFields[] = {"Pain Shock", "Bend Over Extreme"};

constexpr const char* kWeaponFields[sync::kPoseWeapons] = {"Weapon R", "Weapon L"};   // slot = hand
constexpr const char* kGripFields[2] = {"PhysicsConstraint R Hand", "PhysicsConstraint L Hand"};
// The hands' grip variables (docs/TWO_PLAYER_TEST_6.md section 1): the fingers follow the two grip types and the offhand flag in
// ABP_Willie; the other flags move the left hand's attach point. Order = GripState: offhand, alt, mordhau, switched, reverse.
constexpr const char* kGripBoolFields[] = {"L Hand In Offhand Attached", "R Alt Grip", "Mordhau Grip", "2H Switched Hands (Temp)",
                                            "R Hand Reverse Grip"};
static_assert(sizeof(kGripBoolFields) / sizeof(kGripBoolFields[0]) == 5, "GripState has five flags");
constexpr const char* kRootConstraints[] = {"PhysicsConstraint Main", "PhysicsConstraint_Z"};
constexpr const char* kHandles[] = {"PhysicsHandle GetUp", "PhysicsHandle LowerBody", "PhysicsHandle UpperBody"};
constexpr const char* kFistsPrefix = "Weapon_Fists";   // an empty hand's pseudo weapon, kinematic at the world origin
constexpr int kHandBone[2] = {sync::kBoneHandR, sync::kBoneHandL};

constexpr double kHeldMaxUu = 150.0;          // a weapon (root to tip) farther than this from its hand is not held
constexpr double kBoneComMaxUu = 80.0;        // a centre of mass this far from its bone origin is a bad read
constexpr double kWeaponComMaxUu = 300.0;
constexpr int kComTries = 120;                // frames a bad centre-of-mass read is retried before the origin is used
constexpr double kFullEarlyEveryMs = 500.0, kFullEarlyForMs = 6000.0, kFullEveryMs = 2000.0;
constexpr double kCapsuleTeleportUu = 150.0;  // HSMP main.lua:133 TELEPORT_DIST
constexpr uint8_t kWorldStatic = 0, kWorldDynamic = 1;   // ECollisionChannel
constexpr uint8_t kIgnore = 0;                // ECollisionResponse::ECR_Ignore
constexpr uint8_t kNoCollision = 0;           // ECollisionEnabled::NoCollision
constexpr uint8_t kFree = 0;                  // ELinearConstraintMotion::LCM_Free, EAngularConstraintMotion::ACM_Free
constexpr uint8_t kAlwaysTickPose = 0;        // EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones

V3 ToV3(const FVector& v) { return {v.x, v.y, v.z}; }
FVector ToF(const V3& v) { return {v.x, v.y, v.z}; }
Q4 ToQ4(const FQuat& q) { return {q.x, q.y, q.z, q.w}; }
V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
double Len(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
V3 Unrotate(const Q4& q, const V3& v) { return sync::detail::QRot(sync::detail::QConj(q), v); }

// Distance from p to the segment a-b.
double SegmentDistance(const V3& p, const V3& a, const V3& b) {
    const V3 ab = Sub(b, a), ap = Sub(p, a);
    const double len2 = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    const double t = len2 > 1e-9 ? std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2, 0.0, 1.0) : 0.0;
    return Len(Sub(ap, V3{ab.x * t, ab.y * t, ab.z * t}));
}

UObject* ObjField(UObject* o, const char* name) { UObject** p = Field<UObject*>(o, name); return p ? *p : nullptr; }
bool Live(const UObject* o, int32_t index) { return o && index >= 0 && ObjectAt(index) == o; }

// A Willie field of the given reflected type, or nullptr (logged) when it is missing or of another type.
FProperty* TypedField(const UClass* cls, const char* name, const char* type) {
    FProperty* p = FindProperty(cls, name);
    if (!p) { HSM_LOG("rig: Willie has no field '%s'", name); return nullptr; }
    const std::string t = PropertyType(p);
    if (t != type) { HSM_LOG("rig: Willie field '%s' is %s, not %s", name, t.c_str(), type); return nullptr; }
    return p;
}

// Teleports a component to a world location (HSMP main.lua snap_mesh).
void MoveTo(UObject* c, const V3& at) {
    Call(c, "K2_SetWorldLocation").set("NewLocation", ToF(at)).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
}
void MoveBy(UObject* c, const V3& d) {
    Call at(c, "K2_GetComponentLocation");
    if (at.invoke()) MoveTo(c, Add(ToV3(at.get<FVector>("ReturnValue")), d));
}

// Collision off for a component, asked first: a change rebuilds its physics state (HSMP main.lua:5037).
void CollisionOff(UObject* c) {
    if (!c) return;
    Call get(c, "GetCollisionEnabled");
    if (get.invoke() && get.get<uint8_t>("ReturnValue") == kNoCollision) return;
    Call(c, "SetCollisionEnabled").set<uint8_t>("NewType", kNoCollision).invoke();
}

int ObjectType(UObject* c) {
    if (!c) return -1;
    Call t(c, "GetCollisionObjectType");
    return t.invoke() ? int(t.get<uint8_t>("ReturnValue")) : -1;
}
}  // namespace

// ---- binding ---------------------------------------------------------------------------------------------------------

bool FighterRig::Bind(UObject* willie) {
    if (willie && willie == willie_ && StillValid()) return true;
    UObject* const quiet = failLogged_;   // a fighter that keeps failing (retried every frame) is logged once
    Unbind();
    failLogged_ = quiet;
    const bool log = willie != failLogged_;
    UObject* mesh = willie ? ObjField(willie, "Mesh") : nullptr;
    if (!mesh) return false;
    for (int i = 0; i < sync::kPoseBones; ++i) {
        const FName n = MakeName(sync::kPoseBoneNames[i]);
        Call c(mesh, "GetBoneIndex");
        c.setName("BoneName", n);
        if (!c.invoke() || c.get<int32_t>("ReturnValue") < 0) {
            if (log) HSM_LOG("rig: %s has no bone %s", ObjectName(willie).c_str(), sync::kPoseBoneNames[i]);
            failLogged_ = willie;
            return false;
        }
        bones_[i] = n;
    }
    const UClass* mc = ClassOf(mesh);
    socket_ = PreparedCall(mc, "GetSocketTransform", {"InSocketName", "TransformSpace", "ReturnValue"});
    linVelAt_ = PreparedCall(mc, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
    linVel_ = PreparedCall(mc, "GetPhysicsLinearVelocity", {"BoneName", "ReturnValue"});
    angVel_ = PreparedCall(mc, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    setLin_ = PreparedCall(mc, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    setAng_ = PreparedCall(mc, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    com_ = PreparedCall(mc, "GetCenterOfMass", {"BoneName", "ReturnValue"});
    const UClass* wc = ClassOf(willie);
    placeActor_ = PreparedCall(wc, "K2_SetActorLocation", {"NewLocation", "bSweep", "bTeleport"});
    actorLoc_ = PreparedCall(wc, "K2_GetActorLocation", {"ReturnValue"});
    turnActor_ = PreparedCall(wc, "K2_SetActorRotation", {"NewRotation", "bTeleportPhysics"});   // optional (PlaceCapsule yaw)
    if (!socket_.valid() || !linVelAt_.valid() || !linVel_.valid() || !angVel_.valid() || !setLin_.valid() || !setAng_.valid() || !com_.valid() ||
        !placeActor_.valid() || !actorLoc_.valid()) {
        if (log) HSM_LOG("rig: %s: per-frame engine calls did not resolve", ObjectName(willie).c_str());
        Unbind();
        failLogged_ = willie;
        return false;
    }
    for (int w = 0; w < sync::kPoseWeapons; ++w)
        if (FProperty* p = TypedField(wc, kWeaponFields[w], "ObjectProperty")) weaponOffset_[w] = PropertyOffset(p);
    for (int h = 0; h < 2; ++h)
        if (FProperty* p = TypedField(wc, kGripFields[h], "ObjectProperty")) gripOffset_[h] = PropertyOffset(p);
    if (uint8_t* p = Field<uint8_t>(willie, "R_GripType_Current")) gripTypeOffset_[0] = int32_t(p - reinterpret_cast<uint8_t*>(willie));
    if (uint8_t* p = Field<uint8_t>(willie, "L_GripType_Current")) gripTypeOffset_[1] = int32_t(p - reinterpret_cast<uint8_t*>(willie));
    for (int i = 0; i < 5; ++i) gripFlag_[i] = TypedField(wc, kGripBoolFields[i], "BoolProperty");
    for (const char* f : kStandInZeroDoubleFields)
        if (FProperty* p = TypedField(wc, f, "DoubleProperty")) zeroDoubles_.push_back(PropertyOffset(p));
    for (const char* f : kStandInTrueBoolFields)
        if (FProperty* p = TypedField(wc, f, "BoolProperty")) trueBools_.push_back(p);
    for (const char* f : kStandInFalseBoolFields)
        if (FProperty* p = TypedField(wc, f, "BoolProperty")) falseBools_.push_back(p);
    willie_ = willie;
    willieIndex_ = ObjectIndex(willie);
    mesh_ = mesh;
    meshIndex_ = ObjectIndex(mesh);
    HSM_LOG("rig: bound %s (%d bones; neutralise writes %zu doubles, %zu+%zu flags)", ObjectName(willie).c_str(),
            sync::kPoseBones, zeroDoubles_.size(), trueBools_.size(), falseBools_.size());
    return true;
}

void FighterRig::Unbind() { *this = FighterRig(); }   // the bind-failure log mark too: Bind keeps it itself

bool FighterRig::StillValid() const { return Live(willie_, willieIndex_) && Live(mesh_, meshIndex_); }

UObject* FighterRig::FieldAt(int32_t offset) const {
    return offset >= 0 ? *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(willie_) + offset) : nullptr;
}

void FighterRig::RefreshWeapon(int w) {
    Weapon& wp = weapons_[w];
    UObject* actor = FieldAt(weaponOffset_[w]);
    if (w == 1 && actor && actor == FieldAt(weaponOffset_[0])) actor = nullptr;   // one weapon in both hands: slot 0 has it
    if (actor == wp.actor && (!actor || Live(actor, wp.index))) return;
    wp = Weapon{};
    weaponChanged_ = true;
    gripFreed_[0] = gripFreed_[1] = nullptr;   // a re-made grip comes with its limits locked again
    gripLoose_[0] = gripLoose_[1] = nullptr;
    if (!actor) return;
    wp.actor = actor;
    wp.index = ObjectIndex(actor);
    const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(actor)));
    wp.classHash = sync::WeaponClassHash(cls.c_str());
    wp.fists = StartsWithNameAscii(cls, kFistsPrefix);
    if (wp.fists) return;
    Call rc(actor, "K2_GetRootComponent");
    UObject* root = rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr;
    if (!root) { HSM_LOG("rig: %s slot %d (%s) has no root component", ObjectName(willie_).c_str(), w, cls.c_str()); return; }
    const UClass* c = ClassOf(root);
    wp.toWorld = PreparedCall(c, "K2_GetComponentToWorld", {"ReturnValue"});
    wp.simulating = PreparedCall(c, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
    wp.linVelAt = PreparedCall(c, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
    wp.angVel = PreparedCall(c, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    wp.setLin = PreparedCall(c, "SetPhysicsLinearVelocity", {"NewVel", "bAddToCurrent", "BoneName"});
    wp.setAng = PreparedCall(c, "SetPhysicsAngularVelocityInDegrees", {"NewAngVel", "bAddToCurrent", "BoneName"});
    wp.com = PreparedCall(c, "GetCenterOfMass", {"BoneName", "ReturnValue"});
    if (!wp.toWorld.valid() || !wp.simulating.valid() || !wp.linVelAt.valid() || !wp.angVel.valid() || !wp.setLin.valid() ||
        !wp.setAng.valid() || !wp.com.valid()) {
        HSM_LOG("rig: %s slot %d (%s): root %s is not a physics body", ObjectName(willie_).c_str(), w, cls.c_str(),
                ObjectName(root).c_str());
        return;
    }
    wp.root = root;
    wp.rootIndex = ObjectIndex(root);
    wp.base = ObjField(actor, "Root Scene");
    wp.tip = ObjField(actor, "TippyTipScene");
    if (wp.base) wp.baseLoc = PreparedCall(ClassOf(wp.base), "K2_GetComponentLocation", {"ReturnValue"});
    if (wp.tip) wp.tipLoc = PreparedCall(ClassOf(wp.tip), "K2_GetComponentLocation", {"ReturnValue"});
    HSM_LOG("rig: %s slot %d = %s (root %s, base %s, tip %s)", ObjectName(willie_).c_str(), w, cls.c_str(), ObjectName(root).c_str(),
            wp.base ? "yes" : "no", wp.tip ? "yes" : "no");
}

// ---- reading ---------------------------------------------------------------------------------------------------------

bool FighterRig::ReadWeapon(int w, const V3& hand) {
    Weapon& wp = weapons_[w];
    wp.read = wp.haveTip = false;
    if (!wp.root || !Live(wp.actor, wp.index) || !Live(wp.root, wp.rootIndex)) return false;
    wp.simulating.put(0, FName{});
    if (!wp.simulating.invoke(wp.root) || !wp.simulating.take<bool>(1)) return false;   // kinematic: it follows the hand
    if (!wp.toWorld.invoke(wp.root)) return false;
    const FTransform t = wp.toWorld.take<FTransform>(0);
    wp.pos = ToV3(t.translation);
    wp.rot = ToQ4(t.rotation);
    wp.basePos = wp.tipPos = wp.pos;
    if (wp.base && wp.baseLoc.invoke(wp.base)) wp.basePos = ToV3(wp.baseLoc.take<FVector>(0));
    if (wp.tip && wp.tipLoc.invoke(wp.tip)) { wp.tipPos = ToV3(wp.tipLoc.take<FVector>(0)); wp.haveTip = true; }
    // Held: some point between the root and the tip is within reach of the hand (a polearm is gripped mid-shaft). A
    // dropped weapon still in the field, or one parked at the world origin, is not.
    if (SegmentDistance(hand, wp.pos, wp.tipPos) > kHeldMaxUu) return false;
    wp.read = true;
    return true;
}

bool FighterRig::Read(uint32_t tsMs, uint16_t stepUs, sync::PoseFrame* frame, sync::FighterNow* now) {
    if (!StillValid()) return false;
    if (frame) { frame->tsMs = tsMs; frame->stepUs = stepUs; }
    if (now) *now = sync::FighterNow{};
    pelvisRead_ = false;
    const bool measure = now && !comMeasured_;
    int comBad = 0;
    V3 hand[2]{};
    for (int i = 0; i < sync::kPoseBones; ++i) {
        socket_.put(0, bones_[i]);   // TransformSpace is never written: the zeroed buffer holds RTS_World (0)
        if (!socket_.invoke(mesh_)) return false;
        const FTransform t = socket_.take<FTransform>(2);
        const V3 pos = ToV3(t.translation);
        const Q4 rot = ToQ4(t.rotation);
        if (i == sync::kBonePelvis) { pelvis_ = pos; pelvisRead_ = true; }
        if (i == kHandBone[0]) hand[0] = pos;
        if (i == kHandBone[1]) hand[1] = pos;
        const bool body = sync::kPoseHasBody[i];
        if (frame) {
            sync::BoneSample& b = frame->bones[i];
            b = sync::BoneSample{pos, rot, V3{}, V3{}};
            if (body) {   // spine_01 has no body: its velocities stay zero
                linVelAt_.put(0, t.translation).put(1, bones_[i]);
                if (linVelAt_.invoke(mesh_)) b.vel = ToV3(linVelAt_.take<FVector>(2));
                angVel_.put(0, bones_[i]);
                if (angVel_.invoke(mesh_)) b.angVelDeg = ToV3(angVel_.take<FVector>(1));
            }
        }
        if (now) {
            if (measure && body) {   // the body's centre of mass in its bone frame (servo.rs `com`), measured once
                com_.put(0, bones_[i]);
                if (com_.invoke(mesh_)) {
                    const V3 off = Sub(ToV3(com_.take<FVector>(1)), pos);
                    if (Len(off) <= kBoneComMaxUu) comLocal_[i] = Unrotate(rot, off); else ++comBad;
                } else {
                    ++comBad;
                }
            }
            now->bones[i] = sync::BodyNow{pos, rot, comLocal_[i]};
            now->valid[i] = true;
        }
    }
    if (now) {   // the bodies the driver watches for blows (StandInDriver::DetectImpact)
        for (int b : sync::kContactBones) {
            linVel_.put(0, bones_[b]);
            if (linVel_.invoke(mesh_)) { now->vel[b] = ToV3(linVel_.take<FVector>(1)); now->velValid[b] = true; }
        }
    }
    if (measure && (comBad == 0 || ++comTries_ >= kComTries)) {
        comMeasured_ = true;
        if (comBad) HSM_LOG("rig: %s: %d bone centres of mass unreadable, servoing those about the bone origin", ObjectName(willie_).c_str(), comBad);
    }
    for (int w = 0; w < sync::kPoseWeapons; ++w) {
        RefreshWeapon(w);
        Weapon& wp = weapons_[w];
        const bool held = ReadWeapon(w, hand[w]);
        if (frame) {
            sync::WeaponSample& s = frame->weapons[w];
            s = sync::WeaponSample{};
            if (held) {
                s.present = true;
                s.hand = static_cast<uint8_t>(w);
                s.classHash = wp.classHash;
                s.pos = wp.pos;
                s.rot = wp.rot;
                wp.linVelAt.put(0, ToF(wp.pos)).put(1, FName{});
                if (wp.linVelAt.invoke(wp.root)) s.vel = ToV3(wp.linVelAt.take<FVector>(2));
                wp.angVel.put(0, FName{});
                if (wp.angVel.invoke(wp.root)) s.angVelDeg = ToV3(wp.angVel.take<FVector>(1));
                s.base = wp.basePos;
                s.tip = wp.tipPos;
            }
        }
        if (now && held) {
            if (!wp.comMeasured) {
                wp.com.put(0, FName{});
                const V3 off = wp.com.invoke(wp.root) ? Sub(ToV3(wp.com.take<FVector>(1)), wp.pos) : V3{kWeaponComMaxUu * 2, 0, 0};
                if (Len(off) <= kWeaponComMaxUu) { wp.comLocal = Unrotate(wp.rot, off); wp.comMeasured = true; }
                else if (++wp.comTries >= kComTries) {
                    wp.comMeasured = true;
                    HSM_LOG("rig: %s slot %d: centre of mass unreadable, servoing about the root", ObjectName(willie_).c_str(), w);
                }
            }
            now->weapons[w] = sync::BodyNow{wp.pos, wp.rot, wp.comLocal};
            now->weaponValid[w] = true;
        }
    }
    if (frame) frame->grip = ReadGrip();
    if (!typesLogged_) {
        typesLogged_ = true;
        auto type = [this](int w) { const Weapon& wp = weapons_[w]; return Live(wp.root, wp.rootIndex) ? ObjectType(wp.root) : -1; };
        HSM_LOG("rig: %s collision object types: mesh %d, weapon R %d, weapon L %d", ObjectName(willie_).c_str(), ObjectType(mesh_),
                type(0), type(1));
    }
    return true;
}

bool FighterRig::WeaponTip(int w, V3& out) const {
    if (w < 0 || w >= sync::kPoseWeapons || !weapons_[w].read || !weapons_[w].haveTip) return false;
    out = weapons_[w].tipPos;
    return true;
}

// ---- driving ---------------------------------------------------------------------------------------------------------

void FighterRig::HoldStill(Weapon& wp) {
    wp.setLin.put(0, FVector{}).put(1, false).put(2, FName{});
    wp.setLin.invoke(wp.root);
    wp.setAng.put(0, FVector{}).put(1, false).put(2, FName{});
    wp.setAng.invoke(wp.root);
}

// HSMP main.lua PX.grip_want: a hand's grip is freed once the weapon it holds is servoed to the owner's pose (hand and
// weapon then each go exactly where the owner has them), or when the hand holds fists, whose kinematic pseudo weapon at
// the world origin would otherwise twist the driven arm. An empty hand has nothing to hold to.
bool FighterRig::GripWanted(int hand) const {
    UObject* held = FieldAt(weaponOffset_[hand]);
    if (!held) return true;
    for (const Weapon& wp : weapons_)
        if (wp.actor == held && (wp.fists || wp.driven)) return true;   // a two-handed weapon sits in slot 0 for both hands
    return false;
}

void FighterRig::FreeGrip(int hand) {
    UObject* c = FieldAt(gripOffset_[hand]);
    if (!c || c == gripFreed_[hand]) return;
    // Drive and limits off (HSMP main.lua:1127 grip_limits, :2258 grips_off). Every value is zero, so the float/double
    // width of these parameters does not matter.
    Call(c, "SetAngularDriveParams").set<float>("PositionStrength", 0.f).set<float>("VelocityStrength", 0.f)
        .set<float>("InForceLimit", 0.f).invoke();
    Call(c, "SetLinearXLimit").set<uint8_t>("ConstraintType", kFree).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetLinearYLimit").set<uint8_t>("ConstraintType", kFree).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetLinearZLimit").set<uint8_t>("ConstraintType", kFree).set<float>("LimitSize", 0.f).invoke();
    Call(c, "SetAngularSwing1Limit").set<uint8_t>("MotionType", kFree).set<float>("Swing1LimitAngle", 0.f).invoke();
    Call(c, "SetAngularSwing2Limit").set<uint8_t>("MotionType", kFree).set<float>("Swing2LimitAngle", 0.f).invoke();
    Call(c, "SetAngularTwistLimit").set<uint8_t>("ConstraintType", kFree).set<float>("TwistLimitAngle", 0.f).invoke();
    gripFreed_[hand] = c;
    gripLoose_[hand] = c;
    gripLooseIndex_[hand] = ObjectIndex(c);
    if (gripLogged_[hand] != c) {
        gripLogged_[hand] = c;
        HSM_LOG("rig: %s: %s freed", ObjectName(willie_).c_str(), kGripFields[hand]);
    }
}

void FighterRig::Drive(const sync::FighterCommand& cmd) {
    if (!StillValid()) return;
    for (int i = 0; i < sync::kPoseBones; ++i) {
        if (!cmd.drive[i]) continue;
        setLin_.put(0, ToF(cmd.bones[i].vel)).put(1, false).put(2, bones_[i]);
        setLin_.invoke(mesh_);
        setAng_.put(0, ToF(cmd.bones[i].angVelDeg)).put(1, false).put(2, bones_[i]);
        setAng_.invoke(mesh_);
    }
    for (int w = 0; w < sync::kPoseWeapons; ++w) {
        Weapon& wp = weapons_[w];
        wp.driven = cmd.weaponDrive[w] && wp.read && !Pinned(w);
        if (!wp.driven) continue;
        wp.setLin.put(0, ToF(cmd.weapons[w].vel)).put(1, false).put(2, FName{});
        wp.setLin.invoke(wp.root);
        wp.setAng.put(0, ToF(cmd.weapons[w].angVelDeg)).put(1, false).put(2, FName{});
        wp.setAng.invoke(wp.root);
    }
    for (int h = 0; h < 2; ++h)
        if (GripWanted(h)) FreeGrip(h);
    for (int w = 0; w < sync::kPoseWeapons; ++w)   // gravity is off: a loose, undriven weapon would drift
        if (weapons_[w].read && !weapons_[w].driven && !Pinned(w) && GripLoose(w)) HoldStill(weapons_[w]);
}

// The hand's current grip constraint is one this rig freed (its limits stay free: they are never restored).
bool FighterRig::GripLoose(int hand) const {
    UObject* c = gripLoose_[hand];
    return c && Live(c, gripLooseIndex_[hand]) && c == FieldAt(gripOffset_[hand]);
}

void FighterRig::Shift(const V3& d) {
    if (!StillValid()) return;
    // Every body goes to where this frame's Read found it plus d, measured now rather than applied as a delta: a
    // teleporting capsule move in between may already have carried the Mesh (HSMP reads the pelvis for snap_mesh after
    // drive_root, main.lua:4773-4791 and :2340).
    V3 meshD = d;
    if (pelvisRead_) {
        socket_.put(0, bones_[sync::kBonePelvis]);
        if (socket_.invoke(mesh_)) meshD = Sub(Add(pelvis_, d), ToV3(socket_.take<FTransform>(2).translation));
    }
    MoveBy(mesh_, meshD);
    Call(mesh_, "SetAllPhysicsLinearVelocity").set("NewVel", FVector{}).set<bool>("bAddToCurrent", false).invoke();
    Call(mesh_, "SetAllPhysicsAngularVelocityInDegrees").set("NewAngVel", FVector{}).set<bool>("bAddToCurrent", false).invoke();
    // The simulated held weapons move with the body; left behind, the servo would drag a polearm back through the
    // moved arm (HSMP main.lua:2350). A pinned weapon stays where the joint holds it.
    for (int w = 0; w < sync::kPoseWeapons; ++w) {
        Weapon& wp = weapons_[w];
        if (!wp.read || Pinned(w)) continue;
        MoveTo(wp.root, Add(wp.pos, d));   // wp.pos is the root component's location as read this frame
        HoldStill(wp);
    }
    slots_.Shift(willie_, d);   // the sheathed weapons go with the body (standin_slots.h)
}

void FighterRig::PlaceCapsule(const V3& at, const double* yawDeg) {
    if (!StillValid() || !actorLoc_.invoke(willie_)) return;
    const bool jump = Len(Sub(ToV3(actorLoc_.take<FVector>(0)), at)) > kCapsuleTeleportUu;
    placeActor_.put(0, ToF(at)).put(1, false).put(2, jump);
    placeActor_.invoke(willie_);
    if (yawDeg && std::isfinite(*yawDeg) && turnActor_.valid()) {
        turnActor_.put(0, FRotator{0.0, *yawDeg, 0.0}).put(1, false);
        turnActor_.invoke(willie_);
    }
}

// ---- neutralising ----------------------------------------------------------------------------------------------------

void FighterRig::Neutralise(double nowMs) {
    if (!StillValid()) return;
    auto* base = reinterpret_cast<uint8_t*>(willie_);
    for (int32_t off : zeroDoubles_) *reinterpret_cast<double*>(base + off) = 0.0;
    for (FProperty* p : trueBools_) SetBoolValue(p, base + PropertyOffset(p), true);
    for (FProperty* p : falseBools_) SetBoolValue(p, base + PropertyOffset(p), false);
    if (boundAtMs_ < 0.0) boundAtMs_ = nowMs;
    if (nowMs < nextFullMs_ && !weaponChanged_) return;
    NeutraliseFull();
    weaponChanged_ = false;
    nextFullMs_ = nowMs + (nowMs - boundAtMs_ < kFullEarlyForMs ? kFullEarlyEveryMs : kFullEveryMs);
}

// ---- grips -----------------------------------------------------------------------------------------------------------

sync::GripState FighterRig::ReadGrip() const {
    sync::GripState g;
    if (!willie_) return g;
    const auto* base = reinterpret_cast<const uint8_t*>(willie_);
    if (gripTypeOffset_[0] >= 0) g.gripR = base[gripTypeOffset_[0]];
    if (gripTypeOffset_[1] >= 0) g.gripL = base[gripTypeOffset_[1]];
    bool* const flags[5] = {&g.offhand, &g.altGrip, &g.mordhau, &g.switched2H, &g.reverseR};
    for (int i = 0; i < 5; ++i) {
        bool v = false;
        if (gripFlag_[i] && GetBoolValue(gripFlag_[i], base + PropertyOffset(gripFlag_[i]), v)) *flags[i] = v;
    }
    return g;
}

void FighterRig::ApplyGrip(const sync::GripState& g) {
    if (!StillValid()) return;
    auto* base = reinterpret_cast<uint8_t*>(willie_);
    sync::GripState applied = g;
    uint8_t* const appliedType[2] = {&applied.gripR, &applied.gripL};
    const uint8_t poseType[2] = {g.gripR, g.gripL};
    for (int h = 0; h < 2; ++h) {
        if (gripTypeOffset_[h] < 0) continue;
        UObject* held = FieldAt(weaponOffset_[h]);   // this stand-in's own hand: a two-handed grip leaves Weapon L null
        uint8_t def = 0;
        if (held && poseType[h] == 0) {              // the weapon's default matters only where the pose has none
            const uint8_t* d = h == 0 ? Field<uint8_t>(held, "Grip R Hand Default") : Field<uint8_t>(held, "Grip L Hand Default");
            if (d) def = *d;
        }
        const uint8_t v = sync::GripToApply(poseType[h], held != nullptr, def);
        *appliedType[h] = v;
        if (held && v == 0) continue;                // the weapon's own default is 0 too: leave the game's value, 0 destroys it
        base[gripTypeOffset_[h]] = v;
    }
    const bool flags[5] = {g.offhand, g.altGrip, g.mordhau, g.switched2H, g.reverseR};
    for (int i = 0; i < 5; ++i)
        if (gripFlag_[i]) SetBoolValue(gripFlag_[i], base + PropertyOffset(gripFlag_[i]), flags[i]);
    const sync::GripState& l = gripStateLogged_;
    if (gripStateKnown_ && l.gripR == applied.gripR && l.gripL == applied.gripL && l.offhand == applied.offhand &&
        l.altGrip == applied.altGrip && l.mordhau == applied.mordhau && l.switched2H == applied.switched2H && l.reverseR == applied.reverseR)
        return;
    gripStateLogged_ = applied;
    gripStateKnown_ = true;
    HSM_LOG("rig: %s grips R %u L %u%s%s%s%s%s", ObjectName(willie_).c_str(), unsigned(applied.gripR), unsigned(applied.gripL),
            applied.offhand ? " offhand" : "", applied.altGrip ? " alt" : "", applied.mordhau ? " mordhau" : "",
            applied.switched2H ? " switched" : "", applied.reverseR ? " reverse" : "");
}

void FighterRig::SetBodyPolicy(UObject* body) {
    // Gravity off and world geometry ignored: the servo carries every body, and a floor or step must not snag it.
    // Pawn, body (armour: PhysicsBody) and weapon (custom channel 14) contacts stay, so blades and fists still meet it.
    Call(body, "SetEnableGravity").set<bool>("bGravityEnabled", false).invoke();
    Call(body, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldStatic).set<uint8_t>("NewResponse", kIgnore).invoke();
    Call(body, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldDynamic).set<uint8_t>("NewResponse", kIgnore).invoke();
}

void FighterRig::NeutraliseFull() {
    // Joint motors and physical animation: the physics asset's own drives pull the stand-in towards its animation
    // pose (HSMP measured 97 -> 9 deg head error with the motors zeroed). Every value is zero, so the float/double
    // width of these parameters does not matter.
    Call(mesh_, "SetAllMotorsAngularDriveParams").set<float>("InSpring", 0.f).set<float>("InDamping", 0.f)
        .set<float>("InForceLimit", 0.f).set<bool>("bSkipCustomPhysicsType", false).invoke();
    if (UObject* pa = ObjField(willie_, "PhysicalAnimation"))
        Call(pa, "SetStrengthMultiplyer").set<float>("InStrengthMultiplyer", 0.f).invoke();
    if (FObjArray* arr = Field<FObjArray>(willie_, "Phys Anim Array"); arr && arr->num > 0 && arr->num <= 64)
        for (int32_t i = 0; i < arr->num; ++i)
            if (arr->data[i]) Call(arr->data[i], "SetStrengthMultiplyer").set<float>("InStrengthMultiplyer", 0.f).invoke();
    // The BP's own physics handles (get-up, body pulls) let go.
    for (const char* f : kHandles) {
        UObject* h = ObjField(willie_, f);
        if (!h) continue;
        Call g(h, "GetGrabbedComponent");
        if (g.invoke() && g.get<UObject*>("ReturnValue")) Call(h, "ReleaseComponent").invoke();
    }
    // The pelvis-to-capsule constraints: broken once, the capsule is placed separately.
    for (const char* f : kRootConstraints) {
        UObject* c = ObjField(willie_, f);
        if (!c || std::find(brokenRoots_.begin(), brokenRoots_.end(), c) != brokenRoots_.end()) continue;
        Call(c, "BreakConstraint").invoke();
        brokenRoots_.push_back(c);
    }
    SetBodyPolicy(mesh_);
    for (Weapon& wp : weapons_)   // as of the last Read (Neutralise runs before this frame's): checked live
        if (Live(wp.actor, wp.index) && Live(wp.root, wp.rootIndex)) SetBodyPolicy(wp.root);
    CollisionOff(ObjField(willie_, "BoneCore"));   // HSMP main.lua:4673
    Call rc(willie_, "K2_GetRootComponent");
    CollisionOff(rc.invoke() ? rc.get<UObject*>("ReturnValue") : nullptr);   // the capsule only marks the position
    Call mv(willie_, "GetMovementComponent");
    if (UObject* m = mv.invoke() ? mv.get<UObject*>("ReturnValue") : nullptr) Call(m, "DisableMovement").invoke();
    // Bones refreshed from physics even off screen, or the servo reads a stale pose (HSMP main.lua:4672).
    if (FProperty* p = FindProperty(ClassOf(mesh_), "VisibilityBasedAnimTickOption"); p && PropertySize(p) == 1)
        *(reinterpret_cast<uint8_t*>(mesh_) + PropertyOffset(p)) = kAlwaysTickPose;
    gripFreed_[0] = gripFreed_[1] = nullptr;   // the grips are freed again on the next Drive: the BP may re-lock them
    slots_.Maintain(willie_);   // the sheathed weapons: diagnosis line (standin_slots.h)
}

}  // namespace hsm::game

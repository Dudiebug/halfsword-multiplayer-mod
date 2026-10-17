#include "gameplay/world_sync.h"
#include "log.h"

namespace hsm::game {
using namespace hsm::ue;
namespace {

constexpr const char* kWeaponClassName = "ModularWeaponBP_C";
// Below this speed (cm/s)^2 a loose weapon is reported sleeping, so WorldTracker resends it less often.
constexpr double kSleepSpeedThreshold2 = 4.0;

// Actor-level rotation has no reflected quaternion getter/setter in this build (only K2_GetActorRotation /
// K2_SetActorLocationAndRotation, both FRotator); KismetMathLibrary's own conversion functions close the gap
// instead of reimplementing UE's rotator<->quaternion math by hand.
UObject* MathLibraryCDO() {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* c = FindObject("KismetMathLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(c));
    return lib;
}

}  // namespace

std::vector<hsm::WorldSample> CollectLooseWeapons(Adapter& a, const std::function<uint8_t(UObject*)>& entityOfFighter) {
    (void)a;
    std::vector<hsm::WorldSample> out;
    if (!entityOfFighter) { HSM_LOG("world_sync: CollectLooseWeapons missing entityOfFighter"); return out; }
    UObject* mathLib = MathLibraryCDO();
    if (!mathLib) { HSM_LOG("world_sync: KismetMathLibrary unavailable"); return out; }

    for (UObject* actor : FindInstances(kWeaponClassName, true)) {
        UObject** parent = Field<UObject*>(actor, "Parent Actor");
        if (parent && *parent) continue;   // currently held: not a loose weapon

        uint8_t* sheath = Field<uint8_t>(actor, "SheathSlot");
        if (!sheath) { HSM_LOG("world_sync: weapon %s missing SheathSlot", ObjectName(actor).c_str()); continue; }

        uint64_t key;
        uint8_t owner = 0;
        UObject** last = Field<UObject*>(actor, "Last Parent");
        if (last && *last && (owner = entityOfFighter(*last)) != 0) {
            key = (uint64_t(owner) << 8) | *sheath;
        } else {
            owner = 0;
            int32_t idx = ObjectIndex(actor);
            if (idx < 0) continue;
            key = (uint64_t(1) << 63) | uint64_t(idx);   // never collides with an owner/slot key
        }

        Call locCall(actor, "K2_GetActorLocation");
        Call rotCall(actor, "K2_GetActorRotation");
        if (!locCall.invoke() || !rotCall.invoke()) { HSM_LOG("world_sync: actor transform read failed"); continue; }
        FVector loc = locCall.get<FVector>("ReturnValue");
        FRotator rot = rotCall.get<FRotator>("ReturnValue");

        Call velCall(actor, "GetVelocity");
        FVector vel = velCall.invoke() ? velCall.get<FVector>("ReturnValue") : FVector{};

        Call toQuat(mathLib, "Conv_RotatorToQuaternion");
        toQuat.set<FRotator>("InRot", rot);
        if (!toQuat.invoke()) { HSM_LOG("world_sync: Conv_RotatorToQuaternion failed"); continue; }
        FQuat q = toQuat.get<FQuat>("ReturnValue");

        hsm::WorldSample s;
        s.key = key;
        s.owner = owner; s.slot = owner ? *sheath : 0;
        s.kind = hsm::WorldKind::Weapon;
        s.px = float(loc.x); s.py = float(loc.y); s.pz = float(loc.z);
        s.rot = hsm::PackQuat(float(q.x), float(q.y), float(q.z), float(q.w));
        s.vx = float(vel.x); s.vy = float(vel.y); s.vz = float(vel.z);
        s.held = false;
        s.sleeping = (vel.x * vel.x + vel.y * vel.y + vel.z * vel.z) < kSleepSpeedThreshold2;
        out.push_back(s);
    }
    return out;
}

bool ApplyWorldObject(Adapter& a, UObject* actor, const hsm::WorldObject& o) {
    (void)a;
    if (!actor) return false;
    UObject* mathLib = MathLibraryCDO();
    if (!mathLib) { HSM_LOG("world_sync: KismetMathLibrary unavailable"); return false; }

    float qx, qy, qz, qw;
    hsm::UnpackQuat(o.rot, qx, qy, qz, qw);
    Call toRot(mathLib, "Quat_Rotator");
    toRot.set<FQuat>("Q", FQuat{double(qx), double(qy), double(qz), double(qw)});
    if (!toRot.invoke()) { HSM_LOG("world_sync: Quat_Rotator failed"); return false; }
    FRotator rot = toRot.get<FRotator>("ReturnValue");

    Call move(actor, "K2_SetActorLocationAndRotation");
    move.set<FVector>("NewLocation", FVector{double(o.px), double(o.py), double(o.pz)})
        .set<FRotator>("NewRotation", rot).set<bool>("bSweep", false).set<bool>("bTeleport", true);
    if (!move.invoke()) { HSM_LOG("world_sync: K2_SetActorLocationAndRotation failed"); return false; }

    if (UObject** meshPtr = Field<UObject*>(actor, "BaseMesh")) {
        if (UObject* mesh = *meshPtr) {
            Call sim(mesh, "IsSimulatingPhysics");
            if (sim.invoke() && sim.get<bool>("ReturnValue")) {
                Call(mesh, "SetPhysicsLinearVelocity")
                    .set<FVector>("NewVel", FVector{double(o.vx), double(o.vy), double(o.vz)})
                    .set<bool>("bAddToCurrent", false)
                    .invoke();
            }
        }
    }
    return true;
}

UObject* FindClientWeapon(Adapter& a, uint8_t ownerEntity, int weaponIndex, const std::function<UObject*(uint8_t)>& actorOfEntity) {
    (void)a;
    if (!actorOfEntity) { HSM_LOG("world_sync: FindClientWeapon missing actorOfEntity"); return nullptr; }
    UObject* owner = actorOfEntity(ownerEntity);
    if (!owner) return nullptr;

    for (UObject* w : FindInstances(kWeaponClassName, true)) {
        UObject** parent = Field<UObject*>(w, "Parent Actor");
        UObject** last = Field<UObject*>(w, "Last Parent");
        UObject* holder = (parent && *parent) ? *parent : (last ? *last : nullptr);
        if (holder != owner) continue;
        uint8_t* sheath = Field<uint8_t>(w, "SheathSlot");
        if (sheath && int(*sheath) == weaponIndex) return w;
    }
    return nullptr;
}

}  // namespace hsm::game

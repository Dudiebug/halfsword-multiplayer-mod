#include "gameplay/spectate.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace hsm::game {
using namespace hsm::ue;
namespace {

constexpr int32_t kLocalPlayer = 0;     // the mod's local player; adapter.cpp uses index 0 for its controller lookups too
constexpr uint8_t kBlendLinear = 0;     // EViewTargetBlendFunction::VTBlend_Linear

// Default object of GameplayStatics: its static helpers are UFUNCTIONs called on the CDO (as Adapter does).
UObject* GameplayStatics() {
    static UObject* cdo = nullptr;
    if (!cdo) if (UClass* cls = FindClass("GameplayStatics")) cdo = DefaultObject(cls);
    return cdo;
}

// PlayerController of the local player, or nullptr when the world or the controller is not up.
UObject* LocalController(Adapter& a) {
    UObject* statics = GameplayStatics();
    if (!statics || !a.World()) return nullptr;
    Call c(statics, "GetPlayerController");
    c.set<UObject*>("WorldContextObject", a.World()).set<int32_t>("PlayerIndex", kLocalPlayer);
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

// PlayerController::SetViewTargetWithBlend(NewViewTarget, BlendTime, BlendFunc, BlendExp, bLockOutgoing), all reflected
// by name on the controller. Every parameter is set so none depends on the zeroed buffer's defaults.
bool Blend(UObject* controller, UObject* target, float blendSeconds) {
    Call c(controller, "SetViewTargetWithBlend");
    c.set<UObject*>("NewViewTarget", target)
     .set<float>("BlendTime", blendSeconds)
     .set<uint8_t>("BlendFunc", kBlendLinear)
     .set<float>("BlendExp", 0.0f)
     .set<bool>("bLockOutgoing", false);
    return c.invoke();
}

constexpr double kCamBack = 260.0;     // uu behind the head
constexpr double kCamUp = 70.0;        // uu above it
constexpr double kCamWallGap = 25.0;   // kept off a wall the camera would be behind

struct Follow {
    UObject* cam = nullptr;
    int32_t camIndex = -1;
    FVector pos;
    int logged = 0;   // follow lines written (the first 20 a game run)
    bool failLogged = false;
} g_follow;

// GameplayStatics::BeginDeferredActorSpawnFromClass / FinishSpawningActor, as Adapter::SpawnFighter does.
UObject* SpawnCamera(Adapter& a, const FVector& at) {
    UObject* statics = GameplayStatics();
    UClass* cls = FindClass("CameraActor");
    if (!statics || !cls || !a.World()) return nullptr;
    FTransform t; t.translation = at;
    Call begin(statics, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", a.World()).set<UClass*>("ActorClass", cls).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 1).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* cam = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!cam) return nullptr;
    Call finish(statics, "FinishSpawningActor");
    finish.set<UObject*>("Actor", cam).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    cam = finish.invoke() ? finish.get<UObject*>("ReturnValue") : nullptr;
    if (!cam) return nullptr;
    // No letterbox: the camera takes the window's shape.
    if (UClass* compCls = FindClass("CameraComponent")) {
        Call get(cam, "GetComponentByClass");
        get.set<UClass*>("ComponentClass", compCls);
        if (UObject* comp = get.invoke() ? get.get<UObject*>("ReturnValue") : nullptr)
            Call(comp, "SetConstraintAspectRatio").set<bool>("bInConstrainAspectRatio", false).invoke();
    }
    return cam;
}

double Len2(double x, double y) { return std::sqrt(x * x + y * y); }

}  // namespace

UObject* FollowCamera(Adapter& a, UObject* target, bool snap, float dtSeconds) {
    if (!target) return nullptr;
    FVector head;
    if (!a.BoneLocation(target, "head", head)) { head = a.ActorLocation(target); head.z += 60; }
    if (!std::isfinite(head.x) || !std::isfinite(head.y) || !std::isfinite(head.z)) return nullptr;
    Follow& f = g_follow;
    if (f.cam && !a.StillValid(f.cam, f.camIndex)) { f.cam = nullptr; f.camIndex = -1; }
    if (!f.cam) {
        f.cam = SpawnCamera(a, head);
        if (!f.cam) {
            if (!f.failLogged) { f.failLogged = true; HSM_LOG("spectate: the follow camera could not be spawned"); }
            return nullptr;
        }
        f.camIndex = ObjectIndex(f.cam);
        snap = true;
    }
    // Where it should be: behind the head along the camera's current bearing (a snap: behind the fighter's facing).
    double dx = f.pos.x - head.x, dy = f.pos.y - head.y;
    if (snap || Len2(dx, dy) < 1.0) {
        const double yaw = a.ActorYaw(target) * 0.017453292519943295;
        dx = -std::cos(yaw); dy = -std::sin(yaw);
    } else {
        const double l = Len2(dx, dy); dx /= l; dy /= l;
    }
    FVector want{head.x + dx * kCamBack, head.y + dy * kCamBack, head.z + kCamUp};
    // A wall between the head and that spot: stop short of it (the fighter's own body is skipped).
    const FVector from{head.x + dx * 30, head.y + dy * 30, head.z + 15};
    FVector hit;
    if (a.TraceLine(from, want, {target}, &hit, nullptr) && std::isfinite(hit.x)) {
        const double hx = hit.x - head.x, hy = hit.y - head.y, hz = hit.z - head.z;
        const double d = std::sqrt(hx * hx + hy * hy + hz * hz);
        const double keep = d > kCamWallGap * 2 ? (d - kCamWallGap) / d : 0.5;
        want = {head.x + hx * keep, head.y + hy * keep, head.z + hz * keep};
    }
    if (snap) f.pos = want;
    else {
        const double k = 1.0 - std::exp(-8.0 * double(std::clamp(dtSeconds, 0.0f, 0.25f)));
        f.pos = {f.pos.x + (want.x - f.pos.x) * k, f.pos.y + (want.y - f.pos.y) * k, f.pos.z + (want.z - f.pos.z) * k};
    }
    const double lx = head.x - f.pos.x, ly = head.y - f.pos.y, lz = head.z + 10 - f.pos.z;
    FRotator look;
    look.yaw = std::atan2(ly, lx) * 57.29577951308232;
    look.pitch = std::atan2(lz, Len2(lx, ly)) * 57.29577951308232;
    Call move(f.cam, "K2_SetActorLocationAndRotation");
    move.set("NewLocation", f.pos).set("NewRotation", look).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
    if (snap && f.logged < 20) { ++f.logged; HSM_LOG("spectate: follow camera on %s", ObjectName(target).c_str()); }
    return f.cam;
}

void EndFollowCamera(Adapter& a) {
    Follow& f = g_follow;
    if (f.cam && a.StillValid(f.cam, f.camIndex)) a.DestroyActor(f.cam);
    f.cam = nullptr; f.camIndex = -1;
}

bool SetViewTarget(Adapter& a, UObject* target, float blendSeconds) {
    if (!target || !std::isfinite(blendSeconds)) return false;
    UObject* controller = LocalController(a);
    if (!controller) return false;
    return Blend(controller, target, std::max(0.0f, blendSeconds));
}

bool ResetViewToPawn(Adapter& a) {
    UObject* statics = GameplayStatics();
    if (!statics || !a.World()) return false;
    Call p(statics, "GetPlayerPawn");
    p.set<UObject*>("WorldContextObject", a.World()).set<int32_t>("PlayerIndex", kLocalPlayer);
    UObject* pawn = p.invoke() ? p.get<UObject*>("ReturnValue") : nullptr;
    UObject* controller = LocalController(a);
    if (!pawn || !controller) return false;
    return Blend(controller, pawn, 0.0f);
}

}  // namespace hsm::game

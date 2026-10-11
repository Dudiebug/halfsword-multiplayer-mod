#include "gameplay/spectate.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

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

}  // namespace

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

#include "gameplay/spectate.h"
#include "log.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

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

void ClearCameraFade(Adapter& a) {
    UObject* statics = GameplayStatics();
    if (!statics || !a.World()) return;
    Call m(statics, "GetPlayerCameraManager");
    m.set<UObject*>("WorldContextObject", a.World()).set<int32_t>("PlayerIndex", kLocalPlayer);
    if (UObject* manager = m.invoke() ? m.get<UObject*>("ReturnValue") : nullptr) Call(manager, "StopCameraFade").invoke();
}

std::string WidgetsOnScreen() {
    UClass* uw = FindClass("UserWidget");
    std::string out;
    for (int32_t i = 0, n = ObjectCount(); i < n && uw; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o) || !IsA(o, uw)) continue;
        Call v(o, "IsInViewport");
        if (!v.invoke() || !v.get<bool>("ReturnValue")) continue;
        if (!out.empty()) out += ", ";
        out += ObjectName(reinterpret_cast<UObject*>(ClassOf(o)));
    }
    return out.empty() ? "none" : out;
}

namespace {

constexpr uint8_t kSlateHidden = 2;   // ESlateVisibility::Hidden

// "UI_VignetteDark_C" -> "uivignettedarkc": the index's normalised form, so a spelling of the underscores cannot miss.
std::string Squash(const std::string& s) {
    std::string out;
    for (char ch : s) if (std::isalnum(static_cast<unsigned char>(ch))) out += char(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

bool IsDeathOverlay(const std::string& squashed) {
    return squashed == "uivignettec" || squashed == "uivignettedarkc" || squashed == "uivignettedamagec";
}

// UI_HUD's Tick sets Vignette from Consciousness and Stamina, Vignette_WakeUp to 1 - Wake Up Rate and BLACK to 1 at Wake Up Rate 0
// (0.75 at Consciousness 0) (UI_HUD.cpp:77, 86-87). The death timeline drives Wake Up Rate down (Willie_BP.cpp:8265, 8351), so a
// dead fighter's HUD is a solid black image whichever camera is the view target.
constexpr const char* kHudDarkImages[] = {"BLACK", "Vignette_WakeUp", "Vignette"};

struct HiddenOverlay { UObject* w = nullptr; int32_t index = -1; uint8_t visibility = 0; };
std::vector<HiddenOverlay> g_hidden;

}  // namespace

void HideDeathOverlays(UObject* own, bool log) {
    std::string seen;
    int postProcess = 0;
    // Hides one widget (remembering how it was) and notes it for the log; a widget already hidden is left alone.
    auto hide = [&](UObject* o, const std::string& label) {
        if (std::any_of(g_hidden.begin(), g_hidden.end(), [o](const HiddenOverlay& h) { return h.w == o; })) return;
        Call gv(o, "GetVisibility");
        const uint8_t was = gv.invoke() ? gv.get<uint8_t>("ReturnValue") : uint8_t(4);   // SelfHitTestInvisible if unread
        if (log) {
            Call op(o, "GetRenderOpacity");
            const float opacity = op.invoke() ? op.get<float>("ReturnValue") : -1.0f;
            char buf[160];
            std::snprintf(buf, sizeof buf, "%s%s (visibility %u, opacity %.2f)", seen.empty() ? "" : ", ", label.c_str(),
                          unsigned(was), opacity);
            seen += buf;
        }
        if (was == kSlateHidden || was == 1) return;   // already off (Hidden, Collapsed)
        Call(o, "SetVisibility").set<uint8_t>("InVisibility", kSlateHidden).invoke();
        g_hidden.push_back({o, ObjectIndex(o), was});
    };
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || IsDefaultObject(o)) continue;
        const std::string cls = Squash(ObjectName(reinterpret_cast<UObject*>(ClassOf(o))));
        if (log && cls == "bpcustompostprocessc") ++postProcess;
        if (cls == "uihudc") {   // the HUD's own full-screen images: its Tick sets their opacity from the fighter every frame
            for (const char* name : kHudDarkImages)
                if (UObject** img = Field<UObject*>(o, name); img && *img) hide(*img, std::string("UI_HUD.") + name);
            continue;
        }
        if (IsDeathOverlay(cls)) hide(o, ObjectName(reinterpret_cast<UObject*>(ClassOf(o))));
    }
    if (!log) return;
    double consciousness = -1.0;
    if (own) if (const double* c = Field<double>(own, "Consciousness")) consciousness = *c;
    HSM_LOG("spectate: hid the death overlays: %s; my fighter's consciousness %.2f; %d custom post-process actor(s)",
            seen.empty() ? "none found" : seen.c_str(), consciousness, postProcess);
}

void RestoreDeathOverlays(Adapter& a) {
    for (const HiddenOverlay& h : g_hidden)
        if (a.StillValid(h.w, h.index)) Call(h.w, "SetVisibility").set<uint8_t>("InVisibility", h.visibility).invoke();
    g_hidden.clear();
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

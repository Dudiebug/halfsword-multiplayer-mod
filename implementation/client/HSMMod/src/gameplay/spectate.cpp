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

struct Follow {
    UObject* cam = nullptr;
    UObject* camComp = nullptr;   // its CameraComponent: what the audio listener is attached to while spectating (#190)
    int32_t camIndex = -1;
    FVector pos;          // where the camera was last put
    FVector anchor;       // the (smoothed) head it orbits
    double pull = 0;      // the orbit distance after the wall pull-in (eased back out)
    double yaw = 0, pitch = 0;
    bool placed = false;
    int logged = 0;   // follow lines written (the first 20 a game run)
    bool failLogged = false;
} g_follow;

// GameplayStatics::BeginDeferredActorSpawnFromClass / FinishSpawningActor, as Adapter::SpawnFighter does. camComp comes
// back with the camera's CameraComponent when there is one (nullptr otherwise).
UObject* SpawnCamera(Adapter& a, const FVector& at, UObject*& camComp) {
    camComp = nullptr;
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
        if (UObject* comp = get.invoke() ? get.get<UObject*>("ReturnValue") : nullptr) {
            camComp = comp;
            Call(comp, "SetConstraintAspectRatio").set<bool>("bInConstrainAspectRatio", false).invoke();
        }
    }
    return cam;
}

}  // namespace

namespace {

// The camera actor, spawned at `at` when we have none (or it was destroyed with the level).
UObject* EnsureCamera(Adapter& a, const FVector& at, bool& created) {
    Follow& f = g_follow;
    created = false;
    if (f.cam && !a.StillValid(f.cam, f.camIndex)) { f.cam = nullptr; f.camComp = nullptr; f.camIndex = -1; }
    if (f.cam) return f.cam;
    f.cam = SpawnCamera(a, at, f.camComp);
    if (!f.cam) {
        if (!f.failLogged) { f.failLogged = true; HSM_LOG("spectate: the follow camera could not be spawned"); }
        return nullptr;
    }
    f.camIndex = ObjectIndex(f.cam);
    created = true;
    return f.cam;
}

void PlaceCamera(Follow& f, const FVector& pos, double yaw, double pitch) {
    f.pos = pos; f.yaw = yaw; f.pitch = pitch; f.placed = true;
    FRotator rot; rot.yaw = yaw; rot.pitch = pitch;
    Call move(f.cam, "K2_SetActorLocationAndRotation");
    move.set("NewLocation", pos).set("NewRotation", rot).set<bool>("bSweep", false).set<bool>("bTeleport", true).invoke();
}

}  // namespace

UObject* FollowCamera(Adapter& a, UObject* target, UObject* alsoIgnore, bool snap, float dtSeconds, const freecam::Orbit& orbit) {
    if (!target) return nullptr;
    FVector head;
    if (!a.BoneLocation(target, "head", head)) { head = a.ActorLocation(target); head.z += 60; }
    if (!std::isfinite(head.x) || !std::isfinite(head.y) || !std::isfinite(head.z)) return nullptr;
    Follow& f = g_follow;
    bool created = false;
    if (!EnsureCamera(a, head, created)) return nullptr;
    if (created) snap = true;
    // The head it orbits: smoothed (a running fighter's head bobs), cut to the new head on a snap.
    if (snap) f.anchor = head;
    else {
        const double k = 1.0 - std::exp(-12.0 * double(std::clamp(dtSeconds, 0.0f, 0.25f)));
        f.anchor = {f.anchor.x + (head.x - f.anchor.x) * k, f.anchor.y + (head.y - f.anchor.y) * k, f.anchor.z + (head.z - f.anchor.z) * k};
    }
    // A wall between the head and the spot the orbit wants: stop short of it. The trace starts at the true head, so the
    // smoothing cannot put the camera behind a wall the fighter is standing at.
    const freecam::Vec3 h{head.x, head.y, head.z};
    const freecam::Vec3 want = freecam::OrbitCameraPos(h, orbit);
    double hitDist = -1.0;
    FVector hit;
    std::vector<UObject*> skip{target};
    if (alsoIgnore) skip.push_back(alsoIgnore);
    if (a.TraceLine(head, FVector{want.x, want.y, want.z}, skip, &hit, nullptr) && std::isfinite(hit.x) && std::isfinite(hit.y) && std::isfinite(hit.z))
        hitDist = std::sqrt((hit.x - head.x) * (hit.x - head.x) + (hit.y - head.y) * (hit.y - head.y) + (hit.z - head.z) * (hit.z - head.z));
    const double pulled = freecam::PullIn(orbit.dist, hitDist);
    f.pull = snap ? pulled : freecam::EaseDist(f.pull, pulled, dtSeconds);
    f.pull = std::min(f.pull, pulled);   // eased out, never past what the wall allows now
    const freecam::Vec3 anchor{f.anchor.x, f.anchor.y, f.anchor.z};
    const freecam::Vec3 pos = freecam::OrbitCameraPos(anchor, orbit, f.pull);
    PlaceCamera(f, FVector{pos.x, pos.y, pos.z}, orbit.yaw, orbit.pitch);
    // The view is this camera's, so the ears go with it: player 0's listener attenuation otherwise stays where the game put
    // it once, the own fighter's First Person Camera (Adapter::KeepAudioListener re-points it there too), and a spectator
    // hears the fight from its dead fighter's position (#190). Attached to the camera's component as the game attaches its
    // own; session.cpp puts it back when spectating ends. Redone when the camera is spawned and each time the view snaps.
    if (f.camComp && (created || snap))
        if (UObject* ctrl = LocalController(a)) {
            Call(ctrl, "SetAudioListenerAttenuationOverride").set<UObject*>("AttachToComponent", f.camComp).invoke();
            static bool logged = false;
            if (!logged) { logged = true; HSM_LOG("spectate: the audio listener rides the follow camera"); }
        }
    if (snap && f.logged < 20) { ++f.logged; HSM_LOG("spectate: follow camera on %s", ObjectName(target).c_str()); }
    return f.cam;
}

UObject* FreeCamera(Adapter& a, const freecam::Pose& pose) {
    Follow& f = g_follow;
    bool created = false;
    if (!EnsureCamera(a, FVector{pose.pos.x, pose.pos.y, pose.pos.z}, created)) return nullptr;
    PlaceCamera(f, FVector{pose.pos.x, pose.pos.y, pose.pos.z}, pose.yaw, pose.pitch);
    return f.cam;
}

bool CameraPose(freecam::Pose& out) {
    const Follow& f = g_follow;
    if (!f.placed) return false;
    out.pos = {f.pos.x, f.pos.y, f.pos.z}; out.yaw = f.yaw; out.pitch = f.pitch;
    return true;
}

bool ReadMouse(Adapter& a, MouseDelta& out) {
    static bool failLogged = false;
    out = MouseDelta{};
    UObject* controller = LocalController(a);
    if (!controller) return false;
    Call d(controller, "GetInputMouseDelta");
    if (!d.invoke()) {
        if (!failLogged) { failLogged = true; HSM_LOG("spectate: the mouse delta could not be read from the controller"); }
        return false;
    }
    const float dx = d.get<float>("DeltaX"), dy = d.get<float>("DeltaY");
    if (std::isfinite(dx) && std::isfinite(dy)) { out.dx = dx; out.dy = dy; }
    // The wheel is the analog value of the MouseWheelAxis key this frame (an FKey starts with the key's FName).
    static FName wheelKey;
    static bool haveKey = false;
    if (!haveKey) { wheelKey = MakeName("MouseWheelAxis"); haveKey = true; }
    Call w(controller, "GetInputAnalogKeyState");
    if (void* key = w.paramPtr("Key")) {
        *static_cast<FName*>(key) = wheelKey;
        if (w.invoke()) { const float v = w.get<float>("ReturnValue"); if (std::isfinite(v)) out.wheel = v; }
    }
    return true;
}

void EndFollowCamera(Adapter& a) {
    Follow& f = g_follow;
    if (f.cam && a.StillValid(f.cam, f.camIndex)) a.DestroyActor(f.cam);
    f.cam = nullptr; f.camComp = nullptr; f.camIndex = -1; f.placed = false; f.pull = 0;
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
// (0.9 at Consciousness 0) (UI_HUD.cpp:114-115), and drives its pain and damage images from the same fighter every tick:
// Vignette_Pain from Damage Taken, HPDmg1-3 from Health, HeadDmg, ArmLDmg, ArmRDmg, LegLDmg and LegRDmg from the head and limb
// healths (UI_HUD.cpp:105-116). Build 26568321 added ArmLSta and ArmRSta (black, opacity from the hand's L/R Hand Stamina Burn
// Rate), LowerBodySta (Stamina Burn Lower Body) and Vignette_Cold / Vignette_Hot (opacity from the fighter's Temperature)
// (UI_HUD.cpp:30-60, Get_ArmLSta_ColorAndOpacity ... Get_Vignette_Hot_ColorAndOpacity). Fall Asleep Event (Death calls it) plays Timeline_33, which drives Wake Up Rate down (Willie_BP.cpp:16877, 30497), so a dead fighter's
// HUD is a solid black image whichever camera is the view target, and its pain and damage overlays sit over the fight watched.
constexpr const char* kHudDarkImages[] = {"BLACK", "Vignette_WakeUp", "Vignette", "Vignette_Pain", "HPDmg1", "HPDmg2", "HPDmg3",
                                          "HeadDmg", "ArmLDmg", "ArmRDmg", "LegLDmg", "LegRDmg",
                                          "ArmLSta", "ArmRSta", "LowerBodySta", "Vignette_Cold", "Vignette_Hot"};

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

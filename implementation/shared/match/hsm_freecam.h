// The spectator's camera while dead: the orbit round the fighter being watched (mouse look, wheel zoom, pulled in when a
// wall is in the way), the free camera (F flies it with the keys and the mouse), and the follow / free state between
// them. Pure maths, game-independent and unit-tested; gameplay/spectate.cpp moves our CameraActor to the result.
// Units are Unreal's: centimetres and degrees (yaw to the right is positive, pitch up is positive, +Z is up).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hsm::freecam {

struct Vec3 { double x = 0, y = 0, z = 0; };

inline constexpr double kPitchMin = -60.0, kPitchMax = 70.0;     // the orbit's view pitch: -60 looks down from above, +70 up from below
inline constexpr double kFreePitchLimit = 89.0;                   // the free camera may look nearly straight up or down
inline constexpr double kDistMin = 120.0, kDistMax = 800.0;       // 1.2 m .. 8 m behind the head
inline constexpr double kDistDefault = 260.0, kPitchDefault = -15.0;
inline constexpr double kWheelStepCm = 40.0;                      // a wheel notch
inline constexpr double kSensitivity = 0.15;                      // degrees per mouse count
inline constexpr double kMaxMouseCounts = 2000.0;                 // one frame's delta beyond this is a glitch, not a hand
inline constexpr double kWallMargin = 25.0;                       // kept off the wall that pulled the camera in
inline constexpr double kPullMinCm = 40.0;                        // never closer to the head than this (or the wanted distance when smaller)
inline constexpr double kSpeedCmS = 600.0, kFastSpeedCmS = 1800.0;   // 6 m/s, 18 m/s with Shift
inline constexpr double kMaxStepS = 0.1;                          // a frame longer than this moves the camera as if it were this long
inline constexpr double kBoundsMarginCm = 1000.0;                 // the box round the spawn points: 10 m each way
inline constexpr double kBoundsBelowCm = 300.0, kBoundsAboveCm = 1500.0;   // under the lowest point, over the highest

// Whether a look-axis event is the dead body's and ours to swallow: only while watching, and only on the body that was dead when the
// watching began. A fighter that came back (a round reset, a CHANGE LOADOUT respawn) is a new body, so its first look events reach it
// even in the frames before the view returns to it; `body` null (no body to name) swallows nothing.
inline bool SwallowLook(bool watching, const void* axisOwner, const void* body) { return watching && body && axisOwner == body; }

inline bool Finite(double v) { return std::isfinite(v); }
inline bool Finite(const Vec3& v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }

// Degrees into (-180, 180].
inline double WrapYaw(double yaw) {
    if (!Finite(yaw)) return 0.0;
    yaw = std::fmod(yaw, 360.0);
    if (yaw > 180.0) yaw -= 360.0;
    else if (yaw <= -180.0) yaw += 360.0;
    return yaw;
}

// The direction a camera at (yaw, pitch) looks along.
inline Vec3 ViewDir(double yawDeg, double pitchDeg) {
    const double rad = 0.017453292519943295, y = yawDeg * rad, p = pitchDeg * rad;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

// ------------------------------------------------------------------ orbit
struct Orbit {
    double yaw = 0, pitch = kPitchDefault, dist = kDistDefault;   // the view direction and the distance behind the head
};

// A mouse delta (counts; dx right, dy up as Unreal reports it) turns the view: all the way round in yaw, pitch held in
// kPitchMin..kPitchMax. A non-finite delta or sensitivity changes nothing; a huge one is clamped to a glitch limit.
inline Orbit ApplyMouse(Orbit o, double dx, double dy, double sensitivity = kSensitivity) {
    if (!Finite(dx) || !Finite(dy) || !Finite(sensitivity) || sensitivity <= 0.0) return o;
    dx = std::clamp(dx, -kMaxMouseCounts, kMaxMouseCounts);
    dy = std::clamp(dy, -kMaxMouseCounts, kMaxMouseCounts);
    o.yaw = WrapYaw(o.yaw + dx * sensitivity);
    o.pitch = std::clamp(o.pitch + dy * sensitivity, kPitchMin, kPitchMax);
    return o;
}

// Wheel notches (up is positive) bring the camera closer; held in kDistMin..kDistMax.
inline Orbit ApplyWheel(Orbit o, double notches) {
    if (!Finite(notches)) return o;
    notches = std::clamp(notches, -50.0, 50.0);
    o.dist = std::clamp(o.dist - notches * kWheelStepCm, kDistMin, kDistMax);
    return o;
}

// Where the camera sits: `dist` behind the head along the view direction, so it looks at the head.
inline Vec3 OrbitCameraPos(const Vec3& head, const Orbit& o, double dist) {
    const Vec3 d = ViewDir(o.yaw, o.pitch);
    return {head.x - d.x * dist, head.y - d.y * dist, head.z - d.z * dist};
}
inline Vec3 OrbitCameraPos(const Vec3& head, const Orbit& o) { return OrbitCameraPos(head, o, o.dist); }

// The distance to use when a line trace from the head toward the wanted camera spot hit something `hitDist` out (a
// negative or non-finite hitDist: nothing was hit). Stops `margin` short of the hit, never nearer than kPullMinCm
// (or the wanted distance, when that is nearer still), never farther than wanted.
inline double PullIn(double desiredDist, double hitDist, double margin = kWallMargin) {
    if (!Finite(desiredDist) || desiredDist < 0.0) return kDistDefault;
    if (!Finite(hitDist) || hitDist < 0.0) return desiredDist;
    const double floorDist = std::min(desiredDist, kPullMinCm);
    return std::clamp(hitDist - margin, floorDist, desiredDist);
}

// Eases the pulled-in distance: in at once (the wall is in the way now), back out over about a third of a second.
inline double EaseDist(double current, double wanted, double dtSeconds) {
    if (!Finite(wanted)) return Finite(current) ? current : kDistDefault;
    if (!Finite(current) || wanted <= current) return wanted;
    const double dt = Finite(dtSeconds) ? std::clamp(dtSeconds, 0.0, kMaxStepS * 2.5) : 0.0;
    return current + (wanted - current) * (1.0 - std::exp(-8.0 * dt));
}

// ------------------------------------------------------------------ free camera
struct Pose { Vec3 pos; double yaw = 0, pitch = 0; };
struct FreeInput { double fwd = 0, right = 0, up = 0; };   // -1..1 each: W/S, D/A, Space/Ctrl

// The box the free camera stays in.
struct Bounds {
    Vec3 min, max;
    bool valid = false;
};

// The box round a set of points (the level's spawn points, and wherever fighters are): kBoundsMarginCm out sideways,
// kBoundsBelowCm under the lowest and kBoundsAboveCm over the highest. Non-finite points are skipped; none left means no
// box (valid false), and the free camera is then not offered.
inline Bounds MakeBounds(const Vec3* pts, size_t n) {
    Bounds b;
    for (size_t i = 0; i < n; ++i) {
        const Vec3& p = pts[i];
        if (!Finite(p)) continue;
        if (!b.valid) { b.min = b.max = p; b.valid = true; continue; }
        b.min = {std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
        b.max = {std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
    }
    if (!b.valid) return b;
    b.min = {b.min.x - kBoundsMarginCm, b.min.y - kBoundsMarginCm, b.min.z - kBoundsBelowCm};
    b.max = {b.max.x + kBoundsMarginCm, b.max.y + kBoundsMarginCm, b.max.z + kBoundsAboveCm};
    return b;
}

inline Vec3 Clamp(const Vec3& p, const Bounds& b) {
    if (!b.valid) return p;
    return {std::clamp(p.x, b.min.x, b.max.x), std::clamp(p.y, b.min.y, b.max.y), std::clamp(p.z, b.min.z, b.max.z)};
}

// The free camera's view turned by a mouse delta: all the way round, up and down to kFreePitchLimit.
inline Pose ApplyLook(Pose p, double dx, double dy, double sensitivity = kSensitivity) {
    if (!Finite(dx) || !Finite(dy) || !Finite(sensitivity) || sensitivity <= 0.0) return p;
    dx = std::clamp(dx, -kMaxMouseCounts, kMaxMouseCounts);
    dy = std::clamp(dy, -kMaxMouseCounts, kMaxMouseCounts);
    p.yaw = WrapYaw(p.yaw + dx * sensitivity);
    p.pitch = std::clamp(p.pitch + dy * sensitivity, -kFreePitchLimit, kFreePitchLimit);
    return p;
}

// One frame of flying: forward along where the camera looks, right sideways (level), up along +Z. Each input is held
// to -1..1 and the sum to a length of 1, so a diagonal is no faster; the speed is kSpeedCmS, or kFastSpeedCmS with
// `fast`. A frame longer than kMaxStepS counts as kMaxStepS (a hitch must not throw the camera across the map). The
// result is held inside `bounds`; a position that is not finite (before the move) restarts from the box's middle.
inline Pose StepFree(Pose p, FreeInput in, bool fast, double dtSeconds, const Bounds& bounds) {
    if (!Finite(p.pos)) {
        p.pos = bounds.valid ? Vec3{(bounds.min.x + bounds.max.x) / 2, (bounds.min.y + bounds.max.y) / 2, (bounds.min.z + bounds.max.z) / 2} : Vec3{};
    }
    const double dt = Finite(dtSeconds) ? std::clamp(dtSeconds, 0.0, kMaxStepS) : 0.0;
    auto axis = [](double v) { return Finite(v) ? std::clamp(v, -1.0, 1.0) : 0.0; };
    const double f = axis(in.fwd), r = axis(in.right), u = axis(in.up);
    const Vec3 look = ViewDir(p.yaw, p.pitch);
    const Vec3 side = ViewDir(p.yaw + 90.0, 0.0);
    Vec3 v{look.x * f + side.x * r, look.y * f + side.y * r, look.z * f + u};
    const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 1.0) { v.x /= len; v.y /= len; v.z /= len; }
    const double speed = (fast ? kFastSpeedCmS : kSpeedCmS) * dt;
    p.pos = Clamp({p.pos.x + v.x * speed, p.pos.y + v.y * speed, p.pos.z + v.z * speed}, bounds);
    return p;
}

// ------------------------------------------------------------------ follow / free
enum class CamMode : uint8_t { Follow, Free };

// The spectator camera's state. The orbit (angle and distance) lives here, not with the fighter being watched, so
// switching target keeps it; going free and back keeps it too; a respawn (Reset) clears everything.
class SpectateCam {
public:
    CamMode mode() const { return mode_; }
    bool free() const { return mode_ == CamMode::Free; }
    bool allowed() const { return allowed_; }
    const Orbit& orbit() const { return orbit_; }
    const Pose& pose() const { return pose_; }

    // The host's freeCam option. Turning it off while flying returns the view to following at once.
    void SetAllowed(bool on) { allowed_ = on; if (!on) mode_ = CamMode::Follow; }

    // The first time the view is set after a Reset: behind the fighter's facing. Later calls (a new target) change nothing.
    void OrientOnce(double facingYawDeg) {
        if (oriented_) return;
        oriented_ = true;
        orbit_.yaw = WrapYaw(facingYawDeg);
    }

    void Mouse(double dx, double dy, double sensitivity = kSensitivity) {
        if (mode_ == CamMode::Free) pose_ = ApplyLook(pose_, dx, dy, sensitivity);
        else orbit_ = ApplyMouse(orbit_, dx, dy, sensitivity);
    }
    void Wheel(double notches) { if (mode_ == CamMode::Follow) orbit_ = ApplyWheel(orbit_, notches); }

    // F. Follow -> Free starts the flight at `current` (where the camera is now) when the host allows it and the box
    // is usable; Free -> Follow always works. True when the mode changed.
    bool Toggle(const Pose& current, const Bounds& bounds) {
        if (mode_ == CamMode::Free) { mode_ = CamMode::Follow; return true; }
        if (!allowed_ || !bounds.valid || !Finite(current.pos) || !Finite(current.yaw) || !Finite(current.pitch)) return false;
        pose_ = current;
        pose_.yaw = WrapYaw(pose_.yaw);
        pose_.pitch = std::clamp(pose_.pitch, -kFreePitchLimit, kFreePitchLimit);
        pose_.pos = Clamp(pose_.pos, bounds);
        mode_ = CamMode::Free;
        return true;
    }

    void Fly(FreeInput in, bool fast, double dtSeconds, const Bounds& bounds) {
        if (mode_ == CamMode::Free) pose_ = StepFree(pose_, in, fast, dtSeconds, bounds);
    }

    // A respawn (or the end of spectating): following, the orbit back to its defaults, the next target faced afresh.
    void Reset() { mode_ = CamMode::Follow; orbit_ = Orbit{}; pose_ = Pose{}; oriented_ = false; }

private:
    CamMode mode_ = CamMode::Follow;
    bool allowed_ = true;
    bool oriented_ = false;
    Orbit orbit_;
    Pose pose_;
};

// ------------------------------------------------------------------ the game's photo mode in a match (#2624)
// As the game ships it, photo mode pauses the world, which stalls a session. That pause is a switch on photo mode's own
// component (BPC_PhotoMode, on every fighter: bPauseGameInPhotoMode, and bPauseGameInGallery for its gallery). With both
// off the same screen opens over a running fight. So in a running match the photo mode key opens the game's own photo
// mode, not paused, while the player's fighter lives and the host's Free cam row is on. The fighter's own input is held
// meanwhile (PhotoInput), the mod's HUD is hidden for the shot and its hotkeys stay quiet. The game's own key and
// buttons close it; the mod closes it when its condition ends (PhotoEnds).
constexpr uint32_t kPhotoSettleMs = 300;
// The key reached my fighter while photo mode was open and its own screen did not close it within this: the mod does.
constexpr uint32_t kPhotoCloseAfterMs = 150;
inline bool PhotoKeyCloses(bool open, uint32_t sinceKeyMs) { return open && sinceKeyMs >= kPhotoCloseAfterMs; }
struct PhotoWorld {
    bool matchLevel = false;    // a session match's level
    bool fighting = false;      // a match runs (not the lobby, not the results)
    bool alive = false;         // my fighter lives (a dead player has the spectator's camera)
    bool allowed = false;       // the host's Free cam row
    bool busy = false;          // one of the mod's own screens has the keyboard (chat, the pause panel, the lobby page)
    bool ready = false;         // the component is on my fighter and both of its pause switches read off
    bool open = false;          // photo mode (or its gallery) is on screen now
    uint32_t closedForMs = kPhotoSettleMs;   // how long ago it closed (kPhotoSettleMs and up: long enough, or never open)
};
// The photo mode key reaching my own fighter. Only Open lets the game's key event through. While photo mode is open the
// key is its screen's: the fighter's event would toggle it a second time (the game only guards that with its pause), so
// it is kept from the fighter (Close), and for kPhotoSettleMs after it closed too (the same press, a frame later).
enum class PhotoKey : uint8_t { Open, Close, NoMatch, Dead, NotAllowed, Busy, NotReady };
inline PhotoKey PhotoKeyAnswer(const PhotoWorld& w) {
    if (w.open) return PhotoKey::Close;
    if (w.closedForMs < kPhotoSettleMs) return PhotoKey::Busy;
    if (!w.matchLevel || !w.fighting) return PhotoKey::NoMatch;
    if (!w.alive) return PhotoKey::Dead;
    if (!w.allowed) return PhotoKey::NotAllowed;
    if (w.busy) return PhotoKey::Busy;
    if (!w.ready) return PhotoKey::NotReady;
    return PhotoKey::Open;
}
// What the player is told when the key did nothing (empty: nothing).
inline const char* PhotoKeyLine(PhotoKey a) {
    switch (a) {
        case PhotoKey::NoMatch: return "Photo mode works during a match";
        case PhotoKey::NotAllowed: return "The host turned free cameras off for this match, photo mode with them";
        case PhotoKey::NotReady: return "Photo mode could not start here";
        case PhotoKey::Dead: return "";    // a dead player has the spectator's camera and its F key
        case PhotoKey::Busy: return "";    // the screen that has the keyboard got the key
        case PhotoKey::Open: return "";    // the game's own screen says the rest
        case PhotoKey::Close: return "";
    }
    return "";
}
// While it is open: why the mod closes it, or Keep. One of the mod's screens never ends it (none opens over it).
enum class PhotoEnd : uint8_t { Keep, MatchOver, Down, HostOff };
inline PhotoEnd PhotoEnds(const PhotoWorld& w) {
    if (!w.matchLevel || !w.fighting) return PhotoEnd::MatchOver;
    if (!w.alive) return PhotoEnd::Down;
    if (!w.allowed) return PhotoEnd::HostOff;
    return PhotoEnd::Keep;
}
inline const char* PhotoEndText(PhotoEnd e) {
    switch (e) {
        case PhotoEnd::MatchOver: return "the match or the level ended";
        case PhotoEnd::Down: return "my fighter is down";
        case PhotoEnd::HostOff: return "the host turned free cameras off";
        case PhotoEnd::Keep: return "";
    }
    return "";
}

// A body's own input while it is held (a seeker waiting, photo mode open): an axis reads zero (so nothing stays pushed),
// a press is dropped, a release passes (a key held when the hold began lets go as usual).
enum class HeldInput : uint8_t { Pass, Zero, Drop };
inline HeldInput HoldInput(bool held, bool isAxis, bool pressed) {
    if (!held) return HeldInput::Pass;
    return isAxis ? HeldInput::Zero : pressed ? HeldInput::Drop : HeldInput::Pass;
}
// The fighter's own input while photo mode is open. The look axes pass while the game's own camera is the view (photo
// mode turns it with them, as in play) and read zero while photo mode's free camera flies (that one reads the mouse
// itself). Everything else is held.
inline HeldInput PhotoInput(bool open, bool freeCam, bool isAxis, bool isLook, bool pressed) {
    if (open && isAxis && isLook && !freeCam) return HeldInput::Pass;
    return HoldInput(open, isAxis, pressed);
}

}  // namespace hsm::freecam

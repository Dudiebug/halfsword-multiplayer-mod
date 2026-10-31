// Lock-on under host physics (host_lock_on). The game's lock-on lives in Willie_BP's ReceiveTick (decomp Willie_BP.cpp:16513
// and the 0.1 s pulse LatentResume_Lc916, :15778): while the player guards with the camera lock on (GI Settings "Camera Lock
// On", flipped by the Toggle Camera Lock key, :11280) and Lock On Strength is above 0, a sphere trace of object type 13 (the
// other fighters' "Lock On Detector Beacon") from the spine forwards along the camera picks the target, and the control
// rotation is turned towards it every frame (RInterpTo, :16526-16529). A client under host physics runs none of that: its
// fighter is a replica whose ReceiveTick is blocked, and the host's copy of it has Lock on Strength 0 (the host's own
// camera lock setting must not steer it). So the client plays the same rules itself on its own camera, and its control
// rotation, which the host already gives its fighter (Adapter::KeepRemoteFighter), carries the lock to every screen.
// Pure rules; the engine side only feeds positions in and the rotation out. Unit-tested in test_hsm_lockon.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hsm::lockon {

struct V3 { double x = 0, y = 0, z = 0; };
struct Candidate { uint8_t entity = 0; V3 at; };   // another fighter, standing: where its beacon is
struct Rot { double yaw = 0, pitch = 0; };         // degrees

constexpr double kPulseMs = 100.0;                // the game's Delay 0.1 between traces
constexpr double kReachNear = 200.0, kReachFar = 500.0, kSweepRadius = 200.0;   // :15778: from spine + 200 to + 500 along the camera, radius 200
// The trace hits the beacon's sphere, not its middle; its size is not known, so a little slack stands for it.
constexpr double kBeaconSlack = 50.0;

// Whether the lock is on this frame (:16542): a player guarding, upright, with strength above 0 and the camera lock set.
// The game also leaves it when the right hand holds a grip type 16 (not mirrored: nothing here can read it).
inline bool Active(bool cameraLockOn, bool guarding, bool fallen, double strength) {
    return cameraLockOn && guarding && !fallen && strength > 0.0;
}

inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Dir(V3 from, V3 to) { V3 d = Sub(to, from); double l = Len(d); return l > 1e-9 ? V3{d.x / l, d.y / l, d.z / l} : V3{}; }

// The camera's forward vector for a control rotation (UE: pitch up is positive, yaw about Z).
inline V3 Forward(Rot r) {
    const double k = 3.14159265358979323846 / 180.0, cp = std::cos(r.pitch * k);
    return {cp * std::cos(r.yaw * k), cp * std::sin(r.yaw * k), std::sin(r.pitch * k)};
}

// Distance of a point from the segment a-b.
inline double SegmentDistance(V3 p, V3 a, V3 b) {
    const V3 ab = Sub(b, a), ap = Sub(p, a);
    const double l2 = Dot(ab, ab), t = l2 > 0 ? std::clamp(Dot(ap, ab) / l2, 0.0, 1.0) : 0.0;
    return Len(Sub(ap, V3{ab.x * t, ab.y * t, ab.z * t}));
}

// One pulse: the beacons inside the sweep, and of those the one the camera looks most straight at (the game keeps the
// first hit and takes a later one only when its direction from the camera is closer to the camera's forward, :15793).
// Returns the entity, 0 for none (Lock On Found false). `body` is the player's spine, `eye` its camera.
inline uint8_t Pick(V3 body, V3 eye, V3 forward, const std::vector<Candidate>& cands) {
    const V3 a{body.x + forward.x * kReachNear, body.y + forward.y * kReachNear, body.z + forward.z * kReachNear};
    const V3 b{body.x + forward.x * kReachFar, body.y + forward.y * kReachFar, body.z + forward.z * kReachFar};
    uint8_t best = 0; double bestDot = -2.0;
    for (const Candidate& c : cands) {
        if (c.entity == 0 || SegmentDistance(c.at, a, b) > kSweepRadius + kBeaconSlack) continue;
        const double d = Dot(Dir(eye, c.at), forward);
        if (d > bestDot || (d == bestDot && c.entity < best)) { best = c.entity; bestDot = d; }
    }
    return best;
}

inline double NormalizeDeg(double d) { d = std::fmod(d, 360.0); return d > 180.0 ? d - 360.0 : d < -180.0 ? d + 360.0 : d; }

// The look-at rotation from `head` to `at` (FindLookAtRotation).
inline Rot LookAt(V3 head, V3 at) {
    const V3 d = Sub(at, head);
    const double k = 180.0 / 3.14159265358979323846;
    return {std::atan2(d.y, d.x) * k, std::atan2(d.z, std::hypot(d.x, d.y)) * k};
}

// UE's RInterpTo on one angle: the share dt * speed of the way (the whole way at speed 0 or less, and when already close).
inline double InterpAngle(double cur, double target, double dt, double speed) {
    const double delta = NormalizeDeg(target - cur);
    if (speed <= 0.0 || std::fabs(delta) < 1e-3) return target;
    return NormalizeDeg(cur + delta * std::clamp(dt * speed, 0.0, 1.0));
}

// One frame of the turn (:16526-16529): pitch at Lock On Strength, yaw at that times a factor that grows with the flat
// distance from the head to the target (10 uu: 0.1, 100 uu or more: 4), so it does not spin when close. The game's
// versus factor (x3) is not mirrored.
inline Rot Slew(Rot cur, V3 head, V3 at, double dtSec, double strength) {
    const Rot want = LookAt(head, at);
    const double flat = std::hypot(at.x - head.x, at.y - head.y);
    const double ramp = 0.1 + (4.0 - 0.1) * std::clamp((flat - 10.0) / 90.0, 0.0, 1.0);
    return {InterpAngle(cur.yaw, want.yaw, dtSec, strength * ramp), InterpAngle(cur.pitch, want.pitch, dtSec, strength)};
}

// The pulse clock: Due() says when to trace again; the target found is kept until the next pulse and dropped the moment
// the lock goes off. A fighter that falls or leaves is dropped by the caller (Drop).
class Tracker {
public:
    bool Due(bool active, double dtMs) {
        if (!active) { target_ = 0; sinceMs_ = kPulseMs; return false; }
        sinceMs_ += dtMs;
        if (sinceMs_ < kPulseMs) return false;
        sinceMs_ = 0;
        return true;
    }
    void Found(uint8_t entity) { target_ = entity; }
    void Drop() { target_ = 0; }
    uint8_t target() const { return target_; }
private:
    uint8_t target_ = 0;
    double sinceMs_ = kPulseMs;
};

}  // namespace hsm::lockon

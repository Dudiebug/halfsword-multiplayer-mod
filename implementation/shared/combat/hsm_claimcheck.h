// Rev 42, lag-compensated hit validation (competitive netcode, docs/PLAN_COMPETITIVE_NETCODE.md 6.5). Pure logic, unit-tested.
//
// Owner sync shows every stand-in in the past, so the attacker strikes the victim where it was. The victim's owner holds
// the truth: its own fighter. It keeps a short history of its own bones (SelfPoseHistory, stamped on its pose stream
// clock, the clock the attacker's stand-in pose carried), and a claim names the victim pose time the attacker's screen
// showed (HitClaim.victimViewTsMs) and where the blow met it (worldLocation). CheckClaim puts the struck spot of the
// victim's own bone at that time and asks whether it was near the contact: a stand-in that showed the victim where it
// really was passes; a blow on a stand-in far off the truth, or a made-up one, does not.
//
// Like Valorant's capped rewind, a view older than rewindCapMs is checked against the victim's pose rewindCapMs ago, not
// against the claimed time: a very late attacker cannot hit someone who was already safe.
#pragma once
#include "combat/hsm_claim.h"
#include "sync/hsm_pose.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

namespace hsm::combat {

constexpr double kClaimRewindCapMs = 200.0;   // the oldest victim pose a blow is checked against
constexpr double kClaimToleranceUu = 45.0;    // stand-in error a fair blow may carry: codec, servo and a frame of motion
constexpr size_t kSelfHistoryFrames = 64;     // per owned fighter: about a second at 60 Hz

// The pose bone a claim's hit bone is, or -1. Exact pose bone names first; the game's extra bones (twists, fingers,
// toes) map to the pose bone they ride on.
inline int PoseBoneOf(const std::string& name) {
    for (int i = 0; i < sync::kPoseBones; ++i) if (name == sync::kPoseBoneNames[size_t(i)]) return i;
    const bool l = name.size() > 2 && name.compare(name.size() - 2, 2, "_l") == 0;
    const bool r = name.size() > 2 && name.compare(name.size() - 2, 2, "_r") == 0;
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    if (l || r) {
        if (has("finger") || has("thumb") || has("index") || has("middle") || has("ring") || has("pinky")) return l ? sync::kBoneHandL : sync::kBoneHandR;
        if (has("lowerarm")) return l ? 11 : 15;
        if (has("upperarm")) return l ? 10 : 14;
        if (has("thigh")) return l ? 17 : 20;
        if (has("calf")) return l ? 18 : 21;
        if (has("ball") || has("toe")) return l ? sync::kBoneFootL : sync::kBoneFootR;
    }
    return -1;
}

// One owned fighter's recent bones, oldest first, on its pose stream clock.
class SelfPoseHistory {
public:
    struct Frame { uint32_t tsMs = 0; std::array<sync::V3, sync::kPoseBones> pos{}; std::array<sync::Q4, sync::kPoseBones> rot{}; };

    void Push(const sync::PoseFrame& f) {
        if (n_ > 0 && int32_t(f.tsMs - At(n_ - 1).tsMs) <= 0) return;   // the stream clock only goes forward
        Frame& o = ring_[(head_ + n_) % kSelfHistoryFrames];
        if (n_ == kSelfHistoryFrames) head_ = (head_ + 1) % kSelfHistoryFrames; else ++n_;
        o.tsMs = f.tsMs;
        for (int i = 0; i < sync::kPoseBones; ++i) { o.pos[size_t(i)] = f.bones[size_t(i)].pos; o.rot[size_t(i)] = f.bones[size_t(i)].rot; }
    }
    void Clear() { n_ = 0; head_ = 0; }
    bool Empty() const { return n_ == 0; }
    uint32_t NewestTs() const { return n_ ? At(n_ - 1).tsMs : 0; }

    // The bone's world transform at tsMs, interpolated between the frames around it (clamped to the oldest and newest).
    bool BoneAt(uint32_t tsMs, int bone, Xform& out) const {
        if (n_ == 0 || bone < 0 || bone >= sync::kPoseBones) return false;
        const size_t b = size_t(bone);
        auto set = [&](const Frame& f) { out.pos = f.pos[b]; out.rot = f.rot[b]; out.scale = V3{1, 1, 1}; return true; };
        if (int32_t(tsMs - At(0).tsMs) <= 0) return set(At(0));
        if (int32_t(tsMs - At(n_ - 1).tsMs) >= 0) return set(At(n_ - 1));
        for (size_t i = 1; i < n_; ++i) {
            const Frame& a = At(i - 1); const Frame& c = At(i);
            if (int32_t(tsMs - c.tsMs) > 0) continue;
            const double span = double(int32_t(c.tsMs - a.tsMs)), s = span > 0 ? double(int32_t(tsMs - a.tsMs)) / span : 0.0;
            out.pos = V3{ a.pos[b].x + (c.pos[b].x - a.pos[b].x) * s, a.pos[b].y + (c.pos[b].y - a.pos[b].y) * s, a.pos[b].z + (c.pos[b].z - a.pos[b].z) * s };
            Q4 q1 = c.rot[b];
            const Q4& q0 = a.rot[b];
            if (q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w < 0) q1 = Q4{ -q1.x, -q1.y, -q1.z, -q1.w };   // the short way
            Q4 q{ q0.x + (q1.x - q0.x) * s, q0.y + (q1.y - q0.y) * s, q0.z + (q1.z - q0.z) * s, q0.w + (q1.w - q0.w) * s };
            const double len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            out.rot = len > 1e-9 ? Q4{ q.x / len, q.y / len, q.z / len, q.w / len } : q0;
            out.scale = V3{1, 1, 1};
            return true;
        }
        return set(At(n_ - 1));
    }

private:
    const Frame& At(size_t i) const { return ring_[(head_ + i) % kSelfHistoryFrames]; }
    std::array<Frame, kSelfHistoryFrames> ring_{};
    size_t head_ = 0, n_ = 0;
};

enum class ClaimVerdict : uint8_t {
    Ok,         // the struck spot was within tolerance of the contact at the (capped) view time
    Unchecked,  // nothing to check against: no view time (an older sender), no history, a bone the pose does not carry, a stuck blade
    Far,        // the victim's bone was not where the attacker's stand-in showed it
};
inline const char* ClaimVerdictName(ClaimVerdict v) { return v == ClaimVerdict::Ok ? "ok" : v == ClaimVerdict::Far ? "far" : "unchecked"; }

struct ClaimCheckResult {
    ClaimVerdict verdict = ClaimVerdict::Unchecked;
    double distUu = -1;      // struck spot (victim truth) to the claimed contact
    double rewindMs = 0;     // how far back the check looked (capped)
    bool capped = false;     // the claimed view was older than the cap
};

// nowTsMs: the victim's own pose stream clock now (the newest frame it streamed).
inline ClaimCheckResult CheckClaim(const SelfPoseHistory& h, const HitClaim& c, uint32_t nowTsMs,
                                   double rewindCapMs = kClaimRewindCapMs, double toleranceUu = kClaimToleranceUu) {
    ClaimCheckResult r;
    if (c.victimViewTsMs == 0 || h.Empty() || c.striker == StrikerKind::Stuck) return r;
    const int bone = PoseBoneOf(c.hitBone);
    if (bone < 0) return r;
    uint32_t at = c.victimViewTsMs;
    const int32_t back = int32_t(nowTsMs - at);
    if (back > rewindCapMs) { at = nowTsMs - uint32_t(rewindCapMs); r.capped = true; }
    r.rewindMs = std::max(0.0, double(int32_t(nowTsMs - at)));
    Xform x;
    if (!h.BoneAt(at, bone, x)) return r;
    const V3 spot = x.FromLocal(c.location);
    const double dx = spot.x - c.worldLocation.x, dy = spot.y - c.worldLocation.y, dz = spot.z - c.worldLocation.z;
    r.distUu = std::sqrt(dx * dx + dy * dy + dz * dz);
    r.verdict = std::isfinite(r.distUu) && r.distUu <= toleranceUu ? ClaimVerdict::Ok : ClaimVerdict::Far;
    return r;
}

// [net] claim_check: off (no check), log (check and log, never drop: the default, to measure), on (a Far claim is dropped).
enum class ClaimCheckMode : uint8_t { Off, Log, On };
inline bool ParseClaimCheckMode(const std::string& s, ClaimCheckMode& out) {
    if (s == "0" || s == "off") { out = ClaimCheckMode::Off; return true; }
    if (s == "log" || s.empty()) { out = ClaimCheckMode::Log; return true; }
    if (s == "1" || s == "on") { out = ClaimCheckMode::On; return true; }
    return false;
}
inline const char* ClaimCheckModeName(ClaimCheckMode m) { return m == ClaimCheckMode::Off ? "off" : m == ClaimCheckMode::On ? "on" : "log"; }

}  // namespace hsm::combat

// Protocol revision 31: writes the game makes onto another fighter, sent to that fighter's owner (sync audit T3). Owner
// sync simulates each fighter on one machine, so what a fighter's Blueprint writes onto a stand-in (a teammate helped
// up, a shove's push, a hold broken free) changes nothing that is seen: the owner has to make the write itself.
//   RemoteWrite  sender -> host -> target's owner, reliable. The sender owns `from`; the host checks that and forwards.
// Kinds:
//   Revive     Willie_BP Get_Up_Pal (W:4718) on the target: bleeding stopped, health at least 20, give-up cleared.
//   Push       a contact's impulse on the target's bone (kg cm/s, world), gathered over kPushWindowMs and capped: the
//              stand-in moved on the sender's screen, the real body never did (item 3).
//   GrabBreak  the held fighter (from) broke free of the target's hand `bone` (0 R, 1 L): the grabber lets go (item 7).
#pragma once
#include "hsm_protocol.h"
#include "sync/hsm_pose.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace hsm::sync {

enum class RemoteKind : uint8_t { Revive = 1, Push = 2, GrabBreak = 3 };
constexpr uint8_t kLastRemoteKind = 3;

constexpr double kPushMinImpulse = 400.0;     // kg cm/s: a brush, not a shove (a 10 kg torso at 0.4 m/s)
constexpr double kPushMaxImpulse = 15000.0;   // kg cm/s: a 75 kg body at 2 m/s, whatever the solver reported
constexpr double kPushWindowMs = 50.0;        // one shove's contacts over a few frames add up to one push
constexpr int kPushesPerSecond = 10;          // per target
constexpr double kReviveEveryMs = 1000.0;     // per target: a hold re-taken in a struggle revives once

struct RemoteWriteMsg {
    RemoteKind kind = RemoteKind::Revive;
    uint8_t from = 0, target = 0;
    uint8_t bone = 0;      // Push: the target's pose bone (a body); GrabBreak: the target's hand (0 R, 1 L)
    uint16_t seq = 0;
    V3 vec;                // Push: the impulse, world; zero otherwise
};

inline bool RemoteFinite(const V3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline double RemoteLen(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

inline std::vector<uint8_t> Encode(const RemoteWriteMsg& m) {
    Writer w; w.header(MsgType::RemoteWrite);
    w.u8(uint8_t(m.kind)); w.u8(m.from); w.u8(m.target); w.u8(m.bone); w.u16(m.seq);
    w.f32(float(m.vec.x)); w.f32(float(m.vec.y)); w.f32(float(m.vec.z));
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, RemoteWriteMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::RemoteWrite) return false;
    const uint8_t kind = r.u8();
    m.from = r.u8(); m.target = r.u8(); m.bone = r.u8(); m.seq = r.u16();
    m.vec.x = r.f32(); m.vec.y = r.f32(); m.vec.z = r.f32();
    if (!r.done() || kind < 1 || kind > kLastRemoteKind) return false;
    m.kind = RemoteKind(kind);
    if (m.from < 1 || m.from > kMaxWillies || m.target < 1 || m.target > kMaxWillies || m.from == m.target) return false;
    if (!RemoteFinite(m.vec)) return false;
    const bool zero = m.vec.x == 0 && m.vec.y == 0 && m.vec.z == 0;
    switch (m.kind) {
    case RemoteKind::Push: return m.bone < kPoseBones && kPoseHasBody[m.bone] && RemoteLen(m.vec) <= kPushMaxImpulse * 1.001;
    case RemoteKind::GrabBreak: return m.bone < 2 && zero;
    case RemoteKind::Revive: return m.bone == 0 && zero;
    }
    return false;
}

// The sender's side of pushes: a pair's contacts in one frame count once, by the strongest (one touch reaches Deal
// Complex Damage from both fighters' hit events, W:7900 and W:7970); the frames of a window add up, and a window
// kPushWindowMs old becomes one Push on the bone its strongest contact hit, at most kPushMaxImpulse long, if it reached
// kPushMinImpulse, and at most kPushesPerSecond per target.
class PushGather {
public:
    void Add(uint8_t from, uint8_t target, int bone, const V3& impulse, double nowMs, uint64_t frame) {
        if (bone < 0 || bone >= kPoseBones || !kPoseHasBody[size_t(bone)] || !RemoteFinite(impulse) || !std::isfinite(nowMs)) return;
        if (from < 1 || from > kMaxWillies || target < 1 || target > kMaxWillies || from == target) return;
        Window& w = open_[Key(from, target)];
        if (w.startMs < 0) { w.startMs = nowMs; w.frame = frame; }
        if (frame != w.frame) { Commit(w); w.frame = frame; }
        const double len = RemoteLen(impulse);
        if (len > w.frameLen) { w.frameLen = len; w.frameBest = impulse; }
        if (len > w.strongest) { w.strongest = len; w.bone = bone; }
    }
    std::vector<RemoteWriteMsg> Drain(double nowMs) {
        std::vector<RemoteWriteMsg> out;
        for (auto it = open_.begin(); it != open_.end();) {
            Window& w = it->second;
            if (nowMs - w.startMs < kPushWindowMs) { ++it; continue; }
            Commit(it->second);
            const uint8_t from = uint8_t(it->first >> 8), target = uint8_t(it->first & 0xFF);
            const double len = RemoteLen(w.sum);
            if (std::isfinite(len) && len >= kPushMinImpulse && w.bone >= 0 && Allow(target, nowMs)) {
                const double k = len > kPushMaxImpulse ? kPushMaxImpulse / len : 1.0;
                RemoteWriteMsg m;
                m.kind = RemoteKind::Push; m.from = from; m.target = target; m.bone = uint8_t(w.bone); m.seq = ++seq_;
                m.vec = V3{ w.sum.x * k, w.sum.y * k, w.sum.z * k };
                out.push_back(m);
            }
            it = open_.erase(it);
        }
        return out;
    }
    void Clear() { open_.clear(); recent_ = {}; }
    uint16_t NextSeq() { return ++seq_; }
private:
    struct Window { V3 sum, frameBest; double frameLen = 0, strongest = 0; int bone = -1; double startMs = -1; uint64_t frame = 0; };
    static void Commit(Window& w) {
        w.sum.x += w.frameBest.x; w.sum.y += w.frameBest.y; w.sum.z += w.frameBest.z;
        w.frameBest = V3{}; w.frameLen = 0;
    }
    static uint16_t Key(uint8_t from, uint8_t target) { return uint16_t((from << 8) | target); }
    bool Allow(uint8_t target, double nowMs) {
        std::array<double, kPushesPerSecond>& r = recent_[target];
        size_t oldest = 0;
        for (size_t i = 1; i < r.size(); ++i) if (r[i] < r[oldest]) oldest = i;
        if (r[oldest] > 0 && nowMs - r[oldest] < 1000.0) return false;
        r[oldest] = nowMs > 0 ? nowMs : 1e-9;
        return true;
    }
    std::map<uint16_t, Window> open_;
    std::array<std::array<double, kPushesPerSecond>, kMaxWillies + 1> recent_{};
    uint16_t seq_ = 0;
};

// The owner's side: a Revive at most once per kReviveEveryMs per target (a Push is bounded by Decode).
class RemoteGate {
public:
    bool Revive(uint8_t target, double nowMs) {
        if (target > kMaxWillies) return false;
        double& last = revived_[target];
        if (last > 0 && nowMs - last < kReviveEveryMs) return false;
        last = nowMs > 0 ? nowMs : 1e-9;
        return true;
    }
    void Clear() { revived_ = {}; }
private:
    std::array<double, kMaxWillies + 1> revived_{};
};

}  // namespace hsm::sync

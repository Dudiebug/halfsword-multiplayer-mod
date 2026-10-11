// Protocol revision 9: owner authority. Every machine simulates the fighters it owns (its own player; the host also
// its bots) and streams them; everyone else shows them as stand-ins (sync/hsm_standin.h). The host relays.
//   Pose     unreliable, 60 Hz per owned fighter: capsule root and yaw plus the HSMP pose codec (sync/hsm_pose_codec.h)
//   Vitals   unreliable, 15 Hz per owned fighter: health, parts, severed and dislocated parts, dead/fallen/KO
//   HitClaim reliable: an attacker's "Deal Complex Damage" on a stand-in, for the victim's owner to replay
//   HitFx    reliable: the same claim, forwarded by the host to everyone else, cosmetic only
#pragma once
#include "combat/hsm_claim.h"
#include "hsm_protocol.h"
#include "sync/hsm_pose_codec.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hsm {

constexpr float kMaxRootCoord = 1.0e7f;   // cm; a level is far smaller, anything beyond is garbage
constexpr size_t kPoseMsgHeaderBytes = 4 + 1 + 4 * 4 + 2;   // message header, entity, root and yaw, payload length
constexpr size_t kPoseMsgMaxBytes = kPoseMsgHeaderBytes + sync::kPoseFrameMaxBytes;
constexpr size_t kUnfragmentedBytes = 1200;   // Steam splits a larger unreliable message, and one lost piece loses it
static_assert(kPoseMsgMaxBytes <= kUnfragmentedBytes, "a pose must travel as one datagram");

struct PoseMsg {
    uint8_t entity = 0;
    float rootX = 0, rootY = 0, rootZ = 0;   // the owner's capsule (actor) location, cm
    float yawDeg = 0;                         // the owner's actor yaw
    std::vector<uint8_t> pose;                // sync::EncodePose bytes
};

inline std::vector<uint8_t> Encode(const PoseMsg& m) {
    Writer w; w.header(MsgType::Pose);
    w.u8(m.entity); w.f32(m.rootX); w.f32(m.rootY); w.f32(m.rootZ); w.f32(m.yawDeg);
    w.u16(uint16_t(m.pose.size()));                 // kPoseFrameMaxBytes fits (static_assert below)
    w.buf.insert(w.buf.end(), m.pose.begin(), m.pose.end());
    return w.buf;
}
// Also checks the pose payload decodes (frame, when given, receives it), so a relay never forwards garbage.
inline bool Decode(const uint8_t* d, size_t n, PoseMsg& m, sync::PoseFrame* frame = nullptr) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Pose) return false;
    m.entity = r.u8(); m.rootX = r.f32(); m.rootY = r.f32(); m.rootZ = r.f32(); m.yawDeg = r.f32();
    const size_t len = r.u16();
    if (!r.ok() || m.entity == 0 || m.entity > kMaxWillies) return false;
    if (std::fabs(m.rootX) > kMaxRootCoord || std::fabs(m.rootY) > kMaxRootCoord || std::fabs(m.rootZ) > kMaxRootCoord) return false;
    if (std::fabs(m.yawDeg) > 720.f) return false;
    if (len == 0 || len > sync::kPoseFrameMaxBytes || n != kPoseMsgHeaderBytes + len) return false;   // payload = the rest
    m.pose.assign(d + kPoseMsgHeaderBytes, d + n);
    sync::PoseFrame scratch;
    return sync::DecodePose(m.pose.data(), m.pose.size(), frame ? *frame : scratch);
}

static_assert(sync::kPoseFrameMaxBytes <= 0xFFFF, "pose payload length is a u16");

struct VitalsMsg {
    uint8_t entity = 0;
    uint8_t flags = 0;                      // bit0 dead, bit1 fallen, bit2 knocked out
    float health = 0, consciousness = 0, stamina = 0, bleeding = 0;
    std::array<float, 7> partHealth{};      // head, upper body, lower body, arm R, arm L, leg R, leg L
    uint32_t severed = 0;                   // bit per Enum_DismembermentPart value
    uint32_t dislocated = 0;                // bit per dislocatable joint (phase 4)
};
constexpr uint8_t kVitalsFlagsMask = 0x07;

inline std::vector<uint8_t> Encode(const VitalsMsg& m) {
    Writer w; w.header(MsgType::Vitals);
    w.u8(m.entity); w.u8(m.flags); w.f32(m.health); w.f32(m.consciousness); w.f32(m.stamina); w.f32(m.bleeding);
    for (float h : m.partHealth) w.f32(h);
    w.u32(m.severed); w.u32(m.dislocated);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, VitalsMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Vitals) return false;
    m.entity = r.u8(); m.flags = r.u8(); m.health = r.f32(); m.consciousness = r.f32(); m.stamina = r.f32(); m.bleeding = r.f32();
    for (float& h : m.partHealth) h = r.f32();
    m.severed = r.u32(); m.dislocated = r.u32();
    return r.done() && m.entity >= 1 && m.entity <= kMaxWillies && (m.flags & ~kVitalsFlagsMask) == 0;
}

// HitClaim and HitFx carry the same claim; the type says what the receiver may do with it.
struct HitClaimMsg { combat::HitClaim claim; };
struct HitFxMsg { combat::HitClaim claim; };

namespace detail {
inline std::vector<uint8_t> EncodeClaim(MsgType type, const combat::HitClaim& c) {
    Writer w; w.header(type); combat::PutClaim(w, c); return w.buf;
}
inline bool DecodeClaim(const uint8_t* d, size_t n, MsgType type, combat::HitClaim& c) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != type) return false;
    if (!combat::GetClaim(r, c)) return false;
    return c.attacker >= 1 && c.attacker <= kMaxWillies && c.victim >= 1 && c.victim <= kMaxWillies && c.attacker != c.victim;
}
}  // namespace detail

inline std::vector<uint8_t> Encode(const HitClaimMsg& m) { return detail::EncodeClaim(MsgType::HitClaim, m.claim); }
inline bool Decode(const uint8_t* d, size_t n, HitClaimMsg& m) { return detail::DecodeClaim(d, n, MsgType::HitClaim, m.claim); }
inline std::vector<uint8_t> Encode(const HitFxMsg& m) { return detail::EncodeClaim(MsgType::HitFx, m.claim); }
inline bool Decode(const uint8_t* d, size_t n, HitFxMsg& m) { return detail::DecodeClaim(d, n, MsgType::HitFx, m.claim); }

}  // namespace hsm

// Blood and wound paint in owner sync (protocol rev 10, MsgType::PaintFx).
//
// Stand-ins carry Invulnerable, and the game's paint and blood paths start with !Invulnerable (Box Vertex Paint,
// Willie_BP.cpp:1872; Get Damage, 3892), so a blow that lands on a stand-in shows no cut and no blood: only the
// victim's owner, whose own fighter really takes the damage, sees them. The owner therefore reports what its own
// fighter painted, and every other machine replays it on its stand-in of that fighter:
//   Blood  a "Spawn Blood" call (Willie_BP.cpp:20312; only caller Get Damage, 4196): the spawn transform, rate, bone,
//          body part, loop count, extra velocity, velocity and "Pover", plus the owner's Bleeding (Spawn Blood scales
//          its loop count by it). Replayed as the same call on the stand-in.
//   Wound  a wound painted by Get Damage (no Hit Box: a cube of WoundRadius at the hit) or by Box Vertex Paint (its
//          box): a box, the bone, the body part and Damage Rate (Additional). Replayed through the stand-in's own
//          Box Vertex Paint with Invulnerable lowered for that one call.
// Transforms and directions travel in the named bone's frame (as hit claims do), so the paint lands on the same spot of
// the stand-in's bone even though the stand-in trails its owner. Pure types, no engine headers.
//
// Wire layout, in order: header(PaintFx) entity:u8 kind:u8 seq:u16 bone:str(64) bodyPart:u8 local:Xform extent:V3
// rate:f64 damageRate:f64 loopCount:u8 extraVelocity:u8 velocity:V3 power:V3 bleeding:f64 gateLastRate:f64 gateSpRate:f64
// gatePart:u8.
#pragma once
#include "combat/hsm_claim.h"
#include "combat/hsm_sever.h"
#include "hsm_protocol.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hsm::combat {

enum class FxKind : uint8_t { Blood = 1, Wound = 2 };

constexpr uint8_t kFxBodyParts = 8;                 // BodyPart_Enum 0..7 (Willie_BP.cpp:1880, the paint bone switch)
constexpr uint8_t kFxMaxLoops = 10;                 // Get Damage clamps Spawn Blood's Loop Count to 1..10 (4197)
constexpr double kFxMaxOffset = kSeverMaxBoxOffset; // uu from the bone
constexpr double kFxMaxExtent = 500.0;              // uu, box half-extent
constexpr double kFxMaxRate = 1000.0;               // Spawn Rate (Get Damage passes at most 50 x Damage Rate) / Damage Rate
constexpr double kFxMaxVector = 1e7;                // velocity / power components
constexpr double kFxMaxBleeding = 1e4;
constexpr int kFxMaxPerSecond = 30;                 // per (fighter, kind), sender side

struct PaintFxMsg {
    uint8_t entity = 0;              // the owned fighter that was painted
    FxKind kind = FxKind::Blood;
    uint16_t seq = 0;                // per sender, for logs
    std::string bone;                // the frame bone (Blood: Spawn Blood's Bone; Wound: the hit bone)
    uint8_t bodyPart = 0;            // BodyPart_Enum: Blood's Body Partt, Wound's Last Hit Body Part
    Xform local;                     // Blood: the spawn transform; Wound: the box; in the bone's frame
    V3 extent;                       // Wound: box half-extent (each > 0); Blood: unused (0)
    double rate = 0;                 // Blood: Spawn Rate
    double damageRate = 0;           // Wound: Damage Rate (Additional)
    uint8_t loopCount = 1;           // Blood: Loop Count before the Bleeding scaling
    bool extraVelocity = false;      // Blood
    V3 velocity, power;              // Blood: Velocity and Pover, directions in the bone's frame
    double bleeding = 0;             // Blood: the owner's Bleeding at the call
    // Blood: Spawn Blood's own gates on the owner just before the call (Willie_BP.cpp:20328, 20368): Last Spawned Blood
    // Rate, Spawn Blood Sp Rate, Spawn Blood Body Part. Written onto the stand-in first, so it decides as the owner did.
    double gateLastRate = 0, gateSpRate = 0;
    uint8_t gatePart = 0;
};

namespace detail {
inline bool InBox(const V3& v, double lim) { return std::fabs(v.x) <= lim && std::fabs(v.y) <= lim && std::fabs(v.z) <= lim; }
inline bool InRange(double v, double lo, double hi) { return v >= lo && v <= hi; }
}  // namespace detail

inline std::vector<uint8_t> Encode(const PaintFxMsg& m) {
    Writer w;
    w.header(MsgType::PaintFx);
    w.u8(m.entity);
    w.u8(uint8_t(m.kind));
    w.u16(m.seq);
    w.str(m.bone, kMaxClaimString);
    w.u8(m.bodyPart);
    PutXform(w, m.local);
    PutV3(w, m.extent);
    PutF64(w, m.rate);
    PutF64(w, m.damageRate);
    w.u8(m.loopCount);
    w.u8(m.extraVelocity ? 1 : 0);
    PutV3(w, m.velocity);
    PutV3(w, m.power);
    PutF64(w, m.bleeding);
    PutF64(w, m.gateLastRate);
    PutF64(w, m.gateSpRate);
    w.u8(m.gatePart);
    return w.buf;
}

inline bool Decode(const uint8_t* d, size_t n, PaintFxMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n);
    MsgType t;
    if (!r.header(t) || t != MsgType::PaintFx) return false;
    PaintFxMsg o;
    o.entity = r.u8();
    const uint8_t kind = r.u8();
    o.seq = r.u16();
    o.bone = r.str(kMaxClaimString);
    o.bodyPart = r.u8();
    bool numOk = GetXform(r, o.local);
    numOk &= GetV3(r, o.extent);
    numOk &= GetF64(r, o.rate);
    numOk &= GetF64(r, o.damageRate);
    o.loopCount = r.u8();
    const uint8_t extra = r.u8();
    numOk &= GetV3(r, o.velocity);
    numOk &= GetV3(r, o.power);
    numOk &= GetF64(r, o.bleeding);
    numOk &= GetF64(r, o.gateLastRate);
    numOk &= GetF64(r, o.gateSpRate);
    o.gatePart = r.u8();
    if (!r.done() || !numOk) return false;
    if (o.entity < 1 || o.entity > kMaxWillies) return false;
    if (kind != uint8_t(FxKind::Blood) && kind != uint8_t(FxKind::Wound)) return false;
    if (extra > 1) return false;
    o.kind = FxKind(kind);
    o.extraVelocity = extra == 1;
    if (o.bone.empty() || o.bodyPart >= kFxBodyParts) return false;
    if (!detail::LocalXformOk(o.local)) return false;
    if (!detail::InBox(o.extent, kFxMaxExtent) || o.extent.x < 0 || o.extent.y < 0 || o.extent.z < 0) return false;
    if (o.kind == FxKind::Wound && !(o.extent.x > 0 && o.extent.y > 0 && o.extent.z > 0)) return false;
    if (!detail::InRange(o.rate, 0, kFxMaxRate) || !detail::InRange(o.damageRate, 0, kFxMaxRate)) return false;
    if (o.loopCount < 1 || o.loopCount > kFxMaxLoops) return false;
    if (!detail::InBox(o.velocity, kFxMaxVector) || !detail::InBox(o.power, kFxMaxVector)) return false;
    if (!detail::InRange(o.bleeding, 0, kFxMaxBleeding)) return false;
    if (!detail::InRange(o.gateLastRate, 0, kFxMaxRate) || !detail::InRange(o.gateSpRate, 0, kFxMaxRate)) return false;
    if (o.gatePart >= kFxBodyParts) return false;
    m = std::move(o);
    return true;
}

// A world transform into the bone's frame and back (scale carried as-is, as the hit claim's Hit Box).
inline Xform FxToBone(const Xform& bone, const Xform& world) {
    Xform l;
    l.pos = bone.ToLocal(world.pos);
    l.rot = combat::QuatMul(Conjugate(bone.rot), world.rot);
    l.scale = world.scale;
    return l;
}
inline Xform FxFromBone(const Xform& bone, const Xform& local) {
    Xform w;
    w.pos = bone.FromLocal(local.pos);
    w.rot = combat::QuatMul(bone.rot, local.rot);
    w.scale = local.scale;
    return w;
}

// Get Damage's own paint radius when it has no Hit Box: clamp((Damage Raw Scaled + |Impulse|) * 0.005, 3, 10).
inline double WoundRadius(double damageRawScaled, double impulseLen) {
    return std::clamp((damageRawScaled + impulseLen) * 0.005, 3.0, 10.0);
}

// Sender side: at most kFxMaxPerSecond reports per (fighter, kind) per one-second window.
class FxLimiter {
public:
    bool Allow(uint8_t entity, FxKind kind, double nowMs) {
        if (entity < 1 || entity > kMaxWillies) return false;
        Window& w = windows_[std::make_pair(entity, uint8_t(kind))];
        if (w.count == 0 || nowMs - w.startMs >= 1000.0) { w.startMs = nowMs; w.count = 0; }
        if (w.count >= kFxMaxPerSecond) return false;
        ++w.count;
        return true;
    }
    void Forget(uint8_t entity) {
        for (auto it = windows_.begin(); it != windows_.end();) it = it->first.first == entity ? windows_.erase(it) : std::next(it);
    }
    void Clear() { windows_.clear(); }

private:
    struct Window { double startMs = 0; int count = 0; };
    std::map<std::pair<uint8_t, uint8_t>, Window> windows_;   // at most kMaxWillies x 2 entries
};

}  // namespace hsm::combat

// Dismemberment and blood event sync (MsgType::CombatEvent, host -> client, reliable).
// Game-independent, unit-tested. The host reports which Willie parts became severed; clients replay the
// cut through the Willie's own functions. Wire layout follows hsm_protocol.h: little-endian, every decoder
// validates ranges and finiteness and returns false on malformed input.
#pragma once
#include "hsm_protocol.h"
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

constexpr uint8_t kCombatDismember = 0;   // CombatEvent::kind: parts newly severed on an entity
constexpr uint8_t kCombatBlood = 1;       // CombatEvent::kind: blood / vertex paint at a location
constexpr uint32_t kCombatPartBits = 15;  // Enum_DismembermentPart values 0..14 (Willie_BP.cpp Spawn Bone switch)
constexpr uint32_t kCombatPartMask = (1u << kCombatPartBits) - 1u;
constexpr float kCombatMaxCoord = 1e7f;   // same bound as Spawn positions (cm)

struct CombatEvent {
    uint8_t kind = kCombatDismember;   // 0 dismember, 1 blood
    uint8_t entity = 0;                // Willie entity id (never 0)
    uint32_t partsMask = 0;            // dismember: bit per Enum_DismembermentPart value, newly severed only
    float x = 0, y = 0, z = 0;         // world location (cm)
    uint8_t bone = 0;                  // blood: bone index chosen by the host
    uint8_t size = 0;                  // blood: size bucket
    uint8_t sharp = 0;                 // blood: 1 sharp (cut) hit, 0 blunt
};

inline std::vector<uint8_t> Encode(const CombatEvent& m) {
    Writer w; w.header(MsgType::CombatEvent);
    w.u8(m.kind); w.u8(m.entity); w.u32(m.partsMask); w.f32(m.x); w.f32(m.y); w.f32(m.z);
    w.u8(m.bone); w.u8(m.size); w.u8(m.sharp);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, CombatEvent& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::CombatEvent) return false;
    m.kind = r.u8(); m.entity = r.u8(); m.partsMask = r.u32(); m.x = r.f32(); m.y = r.f32(); m.z = r.f32();
    m.bone = r.u8(); m.size = r.u8(); m.sharp = r.u8();
    if (!r.done()) return false;   // truncation, NaN/Inf coordinates and trailing bytes all land here
    if (m.kind > kCombatBlood || m.entity == 0 || (m.partsMask & ~kCombatPartMask) != 0 || m.sharp > 1) return false;
    if (std::fabs(m.x) > kCombatMaxCoord || std::fabs(m.y) > kCombatMaxCoord || std::fabs(m.z) > kCombatMaxCoord) return false;
    return true;
}

// Severed-parts masks keyed by entity id. Used both as the high-water mark of what was already reported and
// as the host's current reading.
using SeveredMasks = std::map<uint8_t, uint32_t>;

// Emits one dismember event per entity whose current mask has bits that `reported` lacks, ordered by entity,
// then merges those bits into `reported`. Cleared bits never produce events, and a bit is reported at most
// once no matter how often the diff runs. Entity 0 and bits outside kCombatPartMask are ignored.
// The caller fills the location and sends the events (reliable channel).
inline std::vector<CombatEvent> DismemberDiff(SeveredMasks& reported, const SeveredMasks& current) {
    std::vector<CombatEvent> out;
    for (const auto& [entity, mask] : current) {
        if (entity == 0) continue;
        uint32_t& seen = reported[entity];
        uint32_t fresh = mask & kCombatPartMask & ~seen;
        if (fresh == 0) continue;
        CombatEvent e; e.kind = kCombatDismember; e.entity = entity; e.partsMask = fresh;
        out.push_back(e);
        seen |= fresh;
    }
    return out;
}

}  // namespace hsm

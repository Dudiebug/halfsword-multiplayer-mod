// ObjectEvent (protocol rev 26, plan M0): something happened to an object's body and every machine replays it.
// Reliable, owner -> host -> all (and direct on the mesh). Game-independent, unit-tested.
//   JointBroke  a constraint of the body snapped (a plank): every machine breaks the same one, by index.
//   Detached    the body left its actor and became its own object: otherId is the new object's id.
//   Attached    the body re-parented (a helmet put back on): otherId is the new parent object.
//   Destroyed   the object is gone.
//   Custom      a mod's own event under its modId (M5): up to kMaxCustomEventBytes of opaque data.
// Wire: 4 header + 8 objId + 1 body + 1 kind, then by kind:
//   JointBroke +1 (constraint index)           = 15 bytes
//   Detached / Attached +8 (otherId)           = 22 bytes
//   Destroyed +0                               = 14 bytes
//   Custom +4 (modId) +1 (length) +length      = 19 + length bytes (up to 51)
#pragma once
#include "session/hsm_object.h"
#include <cstdint>
#include <vector>

namespace hsm {

enum class ObjectEventKind : uint8_t { JointBroke = 1, Detached = 2, Attached = 3, Destroyed = 4, Custom = 5 };
constexpr uint8_t kMaxObjectEventKind = 5;
constexpr uint8_t kMaxConstraintIndex = 63;        // constraints per object, as many as bodies
constexpr size_t kMaxCustomEventBytes = 32;

struct ObjectEventMsg {
    uint64_t objId = 0;
    uint8_t body = 0;                              // body index the event is about, < kMaxObjectBodies
    ObjectEventKind kind = ObjectEventKind::Destroyed;
    uint8_t constraint = 0;                        // JointBroke only
    uint64_t otherId = 0;                          // Detached: the new object's id; Attached: the parent's id
    uint32_t modId = 0;                            // Custom only, never 0
    std::vector<uint8_t> data;                     // Custom only, at most kMaxCustomEventBytes
};

inline std::vector<uint8_t> Encode(const ObjectEventMsg& m) {
    Writer w; w.header(MsgType::ObjectEvent);
    w.u64(m.objId); w.u8(m.body); w.u8(uint8_t(m.kind));
    switch (m.kind) {
        case ObjectEventKind::JointBroke: w.u8(m.constraint); break;
        case ObjectEventKind::Detached: case ObjectEventKind::Attached: w.u64(m.otherId); break;
        case ObjectEventKind::Destroyed: break;
        case ObjectEventKind::Custom: {
            const size_t n = m.data.size() < kMaxCustomEventBytes ? m.data.size() : kMaxCustomEventBytes;
            w.u32(m.modId); w.u8(uint8_t(n)); w.buf.insert(w.buf.end(), m.data.begin(), m.data.begin() + long(n));
            break;
        }
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ObjectEventMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::ObjectEvent) return false;
    m = ObjectEventMsg{};
    m.objId = r.u64(); m.body = r.u8(); const uint8_t k = r.u8();
    if (!r.ok() || k < 1 || k > kMaxObjectEventKind) return false;
    m.kind = ObjectEventKind(k);
    switch (m.kind) {
        case ObjectEventKind::JointBroke: m.constraint = r.u8(); break;
        case ObjectEventKind::Detached: case ObjectEventKind::Attached: m.otherId = r.u64(); break;
        case ObjectEventKind::Destroyed: break;
        case ObjectEventKind::Custom: {
            m.modId = r.u32(); const size_t len = r.u8();
            if (!r.ok() || len > kMaxCustomEventBytes) return false;
            m.data.reserve(len);
            for (size_t i = 0; i < len; ++i) m.data.push_back(r.u8());
            break;
        }
    }
    if (!r.done() || !ValidObjectId(m.objId) || m.body >= kMaxObjectBodies) return false;
    switch (m.kind) {
        case ObjectEventKind::JointBroke: return m.constraint <= kMaxConstraintIndex;
        case ObjectEventKind::Detached: return ValidObjectId(m.otherId) && m.otherId != m.objId;
        case ObjectEventKind::Attached: return ValidObjectId(m.otherId) && m.otherId != m.objId;
        case ObjectEventKind::Destroyed: return true;
        case ObjectEventKind::Custom: return m.modId != 0;
    }
    return false;
}

}  // namespace hsm

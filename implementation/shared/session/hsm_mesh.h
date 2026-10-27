// Mesh (protocol rev 19), pure parts. Clients stream Pose/Vitals to each other as well as to the host; each client
// reports which slots it hears directly (MeshHeard) and the host stops relaying those slots' streams to it. A mask of 0
// is the 0.6.5 behaviour (the host relays everything), so a lost report or a dead direct path only costs bandwidth.
#pragma once
#include "session/hsm_authority.h"
#include <cstdint>

namespace hsm {

constexpr uint32_t kMeshHoldMs = 2000;        // a slot counts as heard this long after its last direct packet
constexpr uint32_t kMeshReportGapMs = 1000;   // at most one MeshHeard per second

// Client side: when each other slot last arrived directly, and when the mask last changed.
class MeshHearing {
public:
    void Note(uint8_t slot, uint32_t nowMs) { if (slot < kMaxFighters) { at_[slot] = nowMs; seen_[slot] = true; } }
    bool Heard(uint8_t slot, uint32_t nowMs) const { return slot < kMaxFighters && seen_[slot] && nowMs - at_[slot] <= kMeshHoldMs; }
    uint8_t Mask(uint32_t nowMs) const {
        uint8_t m = 0;
        for (uint8_t s = 0; s < kMaxFighters; ++s) if (Heard(s, nowMs)) m |= uint8_t(1u << s);
        return m;
    }
    // True when the mask differs from the last one sent and the report gap has passed; *mask is then what to send.
    bool TakeChange(uint32_t nowMs, uint8_t* mask) {
        const uint8_t m = Mask(nowMs);
        if (m == lastSent_ || (sentAny_ && nowMs - lastSentAt_ < kMeshReportGapMs)) return false;
        lastSent_ = m; lastSentAt_ = nowMs; sentAny_ = true; *mask = m;
        return true;
    }
    void Clear() { *this = MeshHearing{}; }
private:
    uint32_t at_[kMaxFighters] = {};
    bool seen_[kMaxFighters] = {};
    uint8_t lastSent_ = 0;
    uint32_t lastSentAt_ = 0;
    bool sentAny_ = false;
};

// Host side: per slot, the slots that client says it hears directly.
class MeshMasks {
public:
    void Set(uint8_t slot, uint8_t mask) {
        if (slot >= kMaxFighters) return;
        m_[slot] = uint8_t(mask & ((1u << kMaxFighters) - 1) & ~(1u << slot));
    }
    // A slot joined or left: its own report is void, and nobody hears it any more.
    void Reset(uint8_t slot) {
        if (slot >= kMaxFighters) return;
        m_[slot] = 0;
        for (uint8_t& x : m_) x = uint8_t(x & ~(1u << slot));
    }
    void Clear() { for (uint8_t& x : m_) x = 0; }
    bool Hears(uint8_t listener, uint8_t from) const { return listener < kMaxFighters && from < kMaxFighters && (m_[listener] >> from & 1); }
private:
    uint8_t m_[kMaxFighters] = {};
};

// RelayTargets for a Pose or Vitals: skips the slots that already hear fromSlot directly.
inline std::vector<uint8_t> RelayTargets(uint8_t fromSlot, const std::vector<uint8_t>& connectedSlots, const MeshMasks& masks) {
    std::vector<uint8_t> out;
    for (uint8_t s : RelayTargets(fromSlot, connectedSlots))
        if (!masks.Hears(s, fromSlot)) out.push_back(s);
    return out;
}

}  // namespace hsm

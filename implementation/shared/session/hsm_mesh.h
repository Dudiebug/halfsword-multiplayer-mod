// Mesh (protocol rev 19), pure parts. Clients stream Pose/Vitals to each other as well as to the host; each client
// reports which slots it hears directly (MeshHeard) and the host stops relaying those slots' streams to it. A mask of 0
// is the 0.6.5 behaviour (the host relays everything), so a lost report or a dead direct path only costs bandwidth.
#pragma once
#include "session/hsm_authority.h"
#include <algorithm>
#include <cstdint>
#include <vector>

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
    // The host's table for this client is new (a Welcome, a PeerMap): the next TakeChange reports the mask again.
    void Invalidate() { lastSent_ = 0xFF; sentAny_ = false; }
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

// Host side: the PeerMap for the clients. all = every joined client's (slot, SteamID); entries that cannot be a mesh peer
// (the host slot, a slot past the roster, no SteamID, a slot named twice) are left out, the rest go out in slot order.
inline PeerMapMsg MakePeerMap(const std::vector<PeerMapEntry>& all) {
    PeerMapMsg m;
    for (const PeerMapEntry& e : all) {
        if (e.slot == kHostSlot || e.slot >= kMaxPeerMap || e.steamId == 0 || m.peers.size() >= kMaxPeerMap) continue;
        const bool dup = std::any_of(m.peers.begin(), m.peers.end(), [&](const PeerMapEntry& x) { return x.slot == e.slot || x.steamId == e.steamId; });
        if (!dup) m.peers.push_back(e);
    }
    std::sort(m.peers.begin(), m.peers.end(), [](const PeerMapEntry& a, const PeerMapEntry& b) { return a.slot < b.slot; });
    return m;
}

// Client side: the other clients this machine exchanges Pose and Vitals with, as the host's last PeerMap named them, and
// which transport peer each one is. The host is never in it (it has its own peer), and neither is this machine.
class MeshPeers {
public:
    struct Change {
        std::vector<uint64_t> want;   // everyone the map names for this machine: allow each one (allowing twice is harmless)
        std::vector<uint64_t> drop;   // named before, not now: close their sessions
    };
    Change Apply(const PeerMapMsg& m, uint8_t mySlot, uint64_t mySteam, uint64_t hostSteam) {
        Change c;
        std::vector<PeerMapEntry> next;
        for (const PeerMapEntry& e : m.peers) {
            if (e.slot == mySlot || e.slot == kHostSlot || e.slot >= kMaxPeerMap || !e.steamId || e.steamId == mySteam || e.steamId == hostSteam) continue;
            if (std::any_of(next.begin(), next.end(), [&](const PeerMapEntry& x) { return x.slot == e.slot || x.steamId == e.steamId; })) continue;
            next.push_back(e); c.want.push_back(e.steamId);
        }
        for (const PeerMapEntry& o : map_)
            if (std::none_of(next.begin(), next.end(), [&](const PeerMapEntry& x) { return x.steamId == o.steamId; })) c.drop.push_back(o.steamId);
        map_ = std::move(next);
        std::erase_if(bound_, [&](const Link& b) { return !Has(b.steam); });
        return c;
    }
    // The transport's peer id for a SteamID Apply named (after AllowPeer).
    void Bind(uint64_t steamId, uint32_t peer) {
        if (!peer || !Has(steamId)) return;
        for (Link& b : bound_) if (b.steam == steamId) { b.peer = peer; return; }
        bound_.push_back({steamId, peer});
    }
    // The transport closed this peer (its Disconnected event): it names nobody any more, but the map still lists the member.
    void ForgetPeer(uint32_t peer) { std::erase_if(bound_, [&](const Link& b) { return b.peer == peer; }); }
    // A lobby member left: the transport closed that session, so a later map has to open it again.
    void Forget(uint64_t steamId) { std::erase_if(bound_, [&](const Link& b) { return b.steam == steamId; }); }
    // The slot a named mesh peer holds; 0xFF for any other peer, the host included.
    uint8_t SlotOfPeer(uint32_t peer) const {
        for (const Link& b : bound_)
            if (b.peer == peer)
                for (const PeerMapEntry& e : map_) if (e.steamId == b.steam) return e.slot;
        return 0xFF;
    }
    bool Has(uint64_t steamId) const { return std::any_of(map_.begin(), map_.end(), [&](const PeerMapEntry& x) { return x.steamId == steamId; }); }
    bool IsBound(uint64_t steamId) const { return std::any_of(bound_.begin(), bound_.end(), [&](const Link& b) { return b.steam == steamId; }); }
    size_t Named() const { return map_.size(); }
    std::vector<uint32_t> Peers() const {
        std::vector<uint32_t> out;
        for (const Link& b : bound_) out.push_back(b.peer);
        return out;
    }
    void Clear() { map_.clear(); bound_.clear(); }
private:
    struct Link { uint64_t steam; uint32_t peer; };
    std::vector<PeerMapEntry> map_;
    std::vector<Link> bound_;
};

}  // namespace hsm

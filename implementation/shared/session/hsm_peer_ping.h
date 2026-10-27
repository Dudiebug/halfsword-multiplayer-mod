// Per-player ping (protocol rev 22), pure parts. Every client pings the host and, on a Steam lobby, every other client
// (the PeerMap names them, mesh on or off) every kPeerPingGapMs and keeps the round trips of the last kPeerPingKeep
// pings per slot. It sends the host a PingTable now and then, so the host's log holds the whole lobby's ping matrix: how
// much a direct path (the mesh) or a relay through another player could save, measured before either is used.
#pragma once
#include "session/hsm_roster.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace hsm {

constexpr uint32_t kPeerPingGapMs = 2000;       // one ping per slot this often
constexpr uint32_t kPeerPingLostMs = 3000;      // unanswered this long: lost
constexpr uint32_t kPeerPingMaxRttMs = 10000;   // a "round trip" longer than this is a stale or forged pong
constexpr uint32_t kPingTableGapMs = 10000;     // a client's PingTable to the host this often
constexpr size_t kPeerPingKeep = 8;             // pings remembered per slot
constexpr uint16_t kPingUnknown = 0xFFFF;       // no answer yet / not measured

struct PingSummary {
    uint16_t medianMs = kPingUnknown, minMs = kPingUnknown, maxMs = kPingUnknown;
    uint8_t lossPct = 0;       // of the remembered pings old enough to count
    uint8_t answered = 0, counted = 0;
};

class PeerPings {
public:
    // Whether slot is due a ping at nowMs (never pinged, or kPeerPingGapMs since the last).
    bool Due(uint8_t slot, uint32_t nowMs) const {
        if (slot >= kMaxFighters) return false;
        const Slot& s = slots_[slot];
        return !s.any || nowMs - s.lastSentMs >= kPeerPingGapMs;
    }
    // A ping carrying nowMs went to slot. A second ping in the same millisecond is not remembered (its pong would be
    // ambiguous).
    void OnSent(uint8_t slot, uint32_t nowMs) {
        if (slot >= kMaxFighters) return;
        Slot& s = slots_[slot];
        if (s.any && s.lastSentMs == nowMs) return;
        s.any = true; s.lastSentMs = nowMs;
        s.rec[s.next] = Rec{nowMs, kPingUnknown, true};
        s.next = (s.next + 1) % kPeerPingKeep;
    }
    // A pong from slot echoing sentMs: kept only for a ping this machine sent and has not had answered. Returns the
    // round trip, or kPingUnknown when it was refused.
    uint16_t OnPong(uint8_t slot, uint32_t sentMs, uint32_t nowMs) {
        if (slot >= kMaxFighters) return kPingUnknown;
        const uint32_t rtt = nowMs - sentMs;
        if (rtt > kPeerPingMaxRttMs) return kPingUnknown;
        for (Rec& r : slots_[slot].rec)
            if (r.used && r.sentMs == sentMs && r.rttMs == kPingUnknown) { r.rttMs = uint16_t(rtt); return r.rttMs; }
        return kPingUnknown;
    }
    PingSummary Summary(uint8_t slot, uint32_t nowMs) const {
        PingSummary out;
        if (slot >= kMaxFighters) return out;
        std::vector<uint16_t> rtts;
        for (const Rec& r : slots_[slot].rec) {
            if (!r.used) continue;
            if (r.rttMs != kPingUnknown) rtts.push_back(r.rttMs);
            else if (nowMs - r.sentMs < kPeerPingLostMs) continue;   // may still come back
            ++out.counted;
        }
        out.answered = uint8_t(rtts.size());
        if (out.counted) out.lossPct = uint8_t((out.counted - out.answered) * 100 / out.counted);
        if (rtts.empty()) return out;
        std::sort(rtts.begin(), rtts.end());
        out.minMs = rtts.front(); out.maxMs = rtts.back(); out.medianMs = rtts[rtts.size() / 2];
        return out;
    }
    bool Pinged(uint8_t slot) const { return slot < kMaxFighters && slots_[slot].any; }
    void Forget(uint8_t slot) { if (slot < kMaxFighters) slots_[slot] = Slot{}; }
    void Clear() { slots_ = {}; }

private:
    struct Rec { uint32_t sentMs = 0; uint16_t rttMs = kPingUnknown; bool used = false; };
    struct Slot { std::array<Rec, kPeerPingKeep> rec{}; size_t next = 0; uint32_t lastSentMs = 0; bool any = false; };
    std::array<Slot, kMaxFighters> slots_{};
};

// One answer to a ping from slot `from`: at most one per kPeerPingGapMs / 2, so a peer cannot use this machine to
// flood (a ping is answered by a pong of the same size).
class PongLimiter {
public:
    bool Allow(uint8_t from, uint32_t nowMs) {
        if (from >= kMaxFighters) return false;
        if (seen_[from] && nowMs - at_[from] < kPeerPingGapMs / 2) return false;
        seen_[from] = true; at_[from] = nowMs;
        return true;
    }
    void Clear() { seen_ = {}; at_ = {}; }
private:
    std::array<bool, kMaxFighters> seen_{};
    std::array<uint32_t, kMaxFighters> at_{};
};

}  // namespace hsm

// Deterministic simulated one-way network link, for testing the pose pipeline without a second player (the
// "mirror" mode and the offline pipeline tests). Each packet is lost with probability lossPct/100; otherwise it
// arrives delayMs + U(-jitterMs, +jitterMs) after it was sent (never before it was sent), so jitter reorders packets
// as a real link does. Same config and seed = same losses and arrival times. Pure: no engine types, no exceptions,
// no heap allocation (fixed-capacity array member).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace hsm::sync {

// SplitMix64 (Steele, Lea, Flood 2014): small, fast, full-period 64-bit generator; enough for link simulation.
class SplitMix64 {
public:
    explicit SplitMix64(uint64_t seed) : s_(seed) {}
    uint64_t Next() {
        uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double Unit() { return static_cast<double>(Next() >> 11) * 0x1.0p-53; }   // uniform [0, 1)

private:
    uint64_t s_;
};

struct LinkConfig {
    double delayMs = 0.0;    // mean one-way delay
    double jitterMs = 0.0;   // each packet's delay is uniform in [delayMs - jitterMs, delayMs + jitterMs], floored at 0
    double lossPct = 0.0;    // independent loss per packet, percent
    uint64_t seed = 1;
};

struct LinkStats {
    uint64_t sent = 0;        // packets handed to Send
    uint64_t lost = 0;        // dropped by the simulated loss
    uint64_t overflow = 0;    // dropped because Cap packets were already in flight
    uint64_t delivered = 0;   // returned by Receive
};

// Non-finite or negative settings become 0; loss is clamped to [0, 100].
inline LinkConfig SanitizeLink(LinkConfig c) {
    auto fix = [](double v) { return std::isfinite(v) && v > 0.0 ? v : 0.0; };
    c.delayMs = fix(c.delayMs);
    c.jitterMs = fix(c.jitterMs);
    c.lossPct = std::min(fix(c.lossPct), 100.0);
    return c;
}

template <typename T, size_t Cap = 256>
class LinkSim {
public:
    explicit LinkSim(const LinkConfig& c = LinkConfig{}) : cfg_(SanitizeLink(c)), rng_(cfg_.seed) {}

    const LinkConfig& Config() const { return cfg_; }
    const LinkStats& Stats() const { return stats_; }
    size_t InFlight() const { return n_; }

    // Queues `packet` sent at local time nowMs. Returns false if it was lost or the link was full.
    bool Send(const T& packet, double nowMs) {
        ++stats_.sent;
        // Both draws are taken for every packet, so one packet's fate never shifts the next packet's numbers.
        const double lossDraw = rng_.Unit() * 100.0;
        const double jitterDraw = rng_.Unit() * 2.0 - 1.0;
        if (lossDraw < cfg_.lossPct) { ++stats_.lost; return false; }
        if (n_ == Cap) { ++stats_.overflow; return false; }
        Entry& e = q_[n_++];
        e.packet = packet;
        e.dueMs = nowMs + std::max(0.0, cfg_.delayMs + cfg_.jitterMs * jitterDraw);
        e.seq = nextSeq_++;
        return true;
    }

    // Takes the earliest-due packet that has arrived by nowMs (ties: the one sent first). False when none has.
    bool Receive(double nowMs, T& out) {
        size_t best = n_;
        for (size_t i = 0; i < n_; ++i) {
            if (q_[i].dueMs > nowMs) continue;
            if (best == n_ || q_[i].dueMs < q_[best].dueMs || (q_[i].dueMs == q_[best].dueMs && q_[i].seq < q_[best].seq)) best = i;
        }
        if (best == n_) return false;
        out = q_[best].packet;
        q_[best] = q_[--n_];
        ++stats_.delivered;
        return true;
    }

    // Drops everything in flight (counters kept).
    void Clear() { n_ = 0; }

private:
    struct Entry {
        T packet{};
        double dueMs = 0.0;
        uint64_t seq = 0;
    };
    LinkConfig cfg_;
    SplitMix64 rng_;
    LinkStats stats_{};
    std::array<Entry, Cap> q_{};
    size_t n_ = 0;
    uint64_t nextSeq_ = 0;
};

}  // namespace hsm::sync

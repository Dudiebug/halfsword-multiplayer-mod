// Test-only outgoing payload impairment. No game, ENet, or wall-clock dependency.
#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <utility>

namespace hsm {
struct ImpairmentSettings {
    int lagMs = 0;
    int jitterMs = 0;
    int lossPct = 0;
    bool Enabled() const { return lagMs > 0 || jitterMs > 0 || lossPct > 0; }
};

template<class Packet> class ImpairmentQueue {
public:
    ImpairmentQueue(ImpairmentSettings settings, uint32_t seed) : settings_(settings), rng_(seed) {}

    // nowMs must be monotonic. Equal deadlines retain insertion order.
    // Loss happens before queueing and never applies to reliable payloads.
    void Push(Packet packet, bool reliable, double nowMs) {
        if (!reliable && settings_.lossPct > 0 &&
            std::uniform_int_distribution<int>(0, 99)(rng_) < settings_.lossPct) return;
        int jitter = settings_.jitterMs > 0
            ? std::uniform_int_distribution<int>(-settings_.jitterMs, settings_.jitterMs)(rng_) : 0;
        double due = nowMs + std::max(0, settings_.lagMs + jitter);
        if (reliable) {
            due = std::max(due, lastReliableDueMs_);
            lastReliableDueMs_ = due;
        }
        pending_.emplace(due, std::move(packet));
    }

    template<class Send> void Drain(double nowMs, Send send) {
        while (!pending_.empty() && pending_.begin()->first <= nowMs) {
            auto first = pending_.begin();
            Packet packet = std::move(first->second);
            pending_.erase(first);
            send(std::move(packet));
        }
    }

    template<class Predicate> void RemoveIf(Predicate predicate) {
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (predicate(it->second)) it = pending_.erase(it);
            else ++it;
        }
    }

private:
    ImpairmentSettings settings_;
    std::mt19937 rng_;
    double lastReliableDueMs_ = std::numeric_limits<double>::lowest();
    std::multimap<double, Packet> pending_;
};
}  // namespace hsm

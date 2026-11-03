// The Pit Bridge Duels queue (Wave 3): the winner stays. The first two in the queue fight; the winner stays at the front, the
// loser goes to the back, so the next fight is the winner against the one waiting longest. A drawn fight (both down) sends both
// to the back in their order. A leaver is taken out; a late joiner goes to the back. Game-independent, unit-tested; Match owns
// one, the host's session executes what it decides.
//
//   streak   the wins in a row of the fighter who is staying (a fight he does not win, or a draw, ends it)
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hsm {

constexpr size_t kMaxQueuePlayers = 16;

class KingQueue {
public:
    // The players in a seed-shuffled order (the same seed, the same queue). False (and an empty queue) for more than
    // kMaxQueuePlayers, an entity 0 or one twice.
    bool Start(const std::vector<uint8_t>& players, uint32_t seed) {
        *this = KingQueue{};
        if (players.size() > kMaxQueuePlayers) return false;
        for (size_t i = 0; i < players.size(); ++i) {
            if (players[i] == 0) return false;
            for (size_t j = 0; j < i; ++j) if (players[j] == players[i]) return false;
        }
        order_ = players;
        std::sort(order_.begin(), order_.end(), [seed](uint8_t a, uint8_t b) {
            const uint32_t ka = Mix(seed ^ (uint32_t(a) * 0x9E3779B1u)), kb = Mix(seed ^ (uint32_t(b) * 0x9E3779B1u));
            return ka != kb ? ka < kb : a < b;
        });
        return true;
    }
    // The queue as the host sent it. False (and an empty queue) when it holds a 0, a player twice, too many players, or a
    // streak with nobody to hold it.
    bool Restore(const std::vector<uint8_t>& order, uint8_t streak) {
        *this = KingQueue{};
        if (order.size() > kMaxQueuePlayers || (streak > 0 && order.empty())) return false;
        for (size_t i = 0; i < order.size(); ++i) {
            if (order[i] == 0) return false;
            for (size_t j = 0; j < i; ++j) if (order[j] == order[i]) return false;
        }
        order_ = order; streak_ = streak; if (streak) last_ = order[0];
        return true;
    }

    const std::vector<uint8_t>& order() const { return order_; }
    bool Contains(uint8_t e) const { return std::find(order_.begin(), order_.end(), e) != order_.end(); }
    // The two who fight now (the first two). False with fewer than two.
    bool Pair(uint8_t& a, uint8_t& b) const {
        if (order_.size() < 2) return false;
        a = order_[0]; b = order_[1];
        return true;
    }
    // The one who stays: the front of the queue while he has a streak, else 0.
    uint8_t King() const { return streak_ ? order_.front() : uint8_t(0); }
    uint8_t streak() const { return streak_; }

    // The fight is won by one of the two. He stays at the front, the other goes to the back. False (no change) when
    // `winner` is not one of the first two.
    bool Report(uint8_t winner) {
        uint8_t a, b;
        if (!Pair(a, b) || (winner != a && winner != b)) return false;
        const uint8_t loser = winner == a ? b : a;
        std::vector<uint8_t> next{winner};
        next.insert(next.end(), order_.begin() + 2, order_.end());
        next.push_back(loser);
        order_ = std::move(next);
        streak_ = last_ != winner ? uint8_t(1) : streak_ < 255 ? uint8_t(streak_ + 1) : uint8_t(255);
        last_ = winner;
        return true;
    }
    // Nobody won: both go to the back, the first one first. False with fewer than two.
    bool Draw() {
        uint8_t a, b;
        if (!Pair(a, b)) return false;
        std::vector<uint8_t> next(order_.begin() + 2, order_.end());
        next.push_back(a); next.push_back(b);
        order_ = std::move(next);
        streak_ = 0; last_ = 0;
        return true;
    }
    // A late joiner (or one who came back) goes to the back. False when he is in the queue already, or it is full.
    bool Add(uint8_t e) {
        if (e == 0 || Contains(e) || order_.size() >= kMaxQueuePlayers) return false;
        order_.push_back(e);
        return true;
    }
    // A leaver. His streak ends with him. False when he was not in the queue.
    bool Remove(uint8_t e) {
        auto it = std::find(order_.begin(), order_.end(), e);
        if (it == order_.end()) return false;
        order_.erase(it);
        if (last_ == e) { streak_ = 0; last_ = 0; }
        return true;
    }

private:
    static uint32_t Mix(uint32_t x) { x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16; return x; }
    std::vector<uint8_t> order_;
    uint8_t streak_ = 0, last_ = 0;
};

// The queue on the wire (MatchInfo): the order and the streak hold together. Both empty is fine (no queue).
inline bool ValidQueueWire(const std::vector<uint8_t>& order, uint8_t streak) {
    KingQueue q;
    return q.Restore(order, streak);
}

}  // namespace hsm

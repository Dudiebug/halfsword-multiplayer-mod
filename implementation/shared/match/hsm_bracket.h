// Single-elimination bracket for the Tournament mode (Wave 3). Game-independent, unit-tested; Match owns one and the host's
// session executes what it decides. The players are entities (1..255); a bracket knows nothing of rounds or gear.
//
//   Start      players in any order -> seeded by `seed` (a deterministic shuffle: the same seed, the same bracket), placed
//              in the standard bracket (seed 1 meets the last seed, 2 the one before ...). A count that is no power of two
//              leaves empty places, which are byes: the top seeds meet them.
//   Current    the game to fight now: the first game, round by round, whose two places both hold a player
//   Report     the winner of the current game; a bye settles on its own
//   Remove     a player who left: his place becomes a bye from where he stands, so the opponent in his next unplayed game
//              advances (a forfeit in the game being fought, a bye in one not yet played); a player already out stays out
//   Restore    the same bracket on a client, from the wire (seeds in seed order, the winner of every game in play order)
//
// Node values: kUnknown (the game feeding the place is not decided), 0 (empty: a bye, or a player who left) or the entity.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kMaxBracketPlayers = 16;
constexpr uint8_t kBracketEmpty = 0xFF;   // wire: this game's winner place is empty (a bye with nobody behind it, or the player left)

// The places of a bracket of `size` (a power of two) in order, each holding the seed that sits there (1 = the top seed).
inline std::vector<int> BracketSeedPlaces(int size) {
    std::vector<int> places{1};
    while (int(places.size()) < size) {
        std::vector<int> next;
        const int sum = int(places.size()) * 2 + 1;
        for (int s : places) { next.push_back(s); next.push_back(sum - s); }
        places = std::move(next);
    }
    return places;
}
// The size of the bracket for n players: the next power of two, two at least.
inline int BracketSize(size_t n) { int p = 2; while (size_t(p) < n) p *= 2; return p; }

inline uint32_t BracketMix(uint32_t x) {
    x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;   // murmur3 finaliser
    return x;
}

class Bracket {
public:
    struct Game { int round = 0, index = 0; uint8_t a = 0, b = 0; };

    // False (and nothing built) for more than kMaxBracketPlayers, an entity 0 or 0xFF, or one twice.
    bool Start(const std::vector<uint8_t>& players, uint32_t seed) {
        *this = Bracket{};
        if (players.size() > kMaxBracketPlayers) return false;
        for (size_t i = 0; i < players.size(); ++i) {
            if (players[i] == 0 || players[i] == kBracketEmpty) return false;
            for (size_t j = 0; j < i; ++j) if (players[j] == players[i]) return false;
        }
        seeds_ = players;
        std::sort(seeds_.begin(), seeds_.end(), [seed](uint8_t a, uint8_t b) {
            const uint32_t ka = BracketMix(seed ^ (uint32_t(a) * 0x9E3779B1u)), kb = BracketMix(seed ^ (uint32_t(b) * 0x9E3779B1u));
            return ka != kb ? ka < kb : a < b;
        });
        Build();
        return true;
    }

    // The bracket as the host sent it: `seeds` in seed order (0: a player who left before his first game), `winners` the
    // winner of every game in play order (round by round; 0 undecided, kBracketEmpty an empty place, else the entity).
    // False (and an empty bracket) when it does not hold together.
    bool Restore(const std::vector<uint8_t>& seeds, const std::vector<uint8_t>& winners) {
        *this = Bracket{};
        if (seeds.size() < 2 || seeds.size() > kMaxBracketPlayers) return false;
        for (size_t i = 0; i < seeds.size(); ++i) {
            if (seeds[i] == kBracketEmpty) return false;
            for (size_t j = 0; j < i; ++j) if (seeds[i] && seeds[j] == seeds[i]) return false;
        }
        seeds_ = seeds;
        Build();
        if (winners.size() != size_t(Size() - 1)) { *this = Bracket{}; return false; }
        size_t k = 0;
        for (int r = 0; r < rounds_; ++r)
            for (int g = 0; g < (Size() >> (r + 1)); ++g, ++k) {
                Settle();
                const int16_t a = node_[size_t(r)][size_t(2 * g)], b = node_[size_t(r)][size_t(2 * g + 1)];
                int16_t& out = node_[size_t(r + 1)][size_t(g)];
                const uint8_t w = winners[k];
                if (w == 0) continue;                                    // undecided, or a bye Settle makes
                const bool bye = a == 0 || b == 0;
                if (a == kUnknown || b == kUnknown || (out != kUnknown && !bye)) { *this = Bracket{}; return false; }
                if (w == kBracketEmpty) { out = 0; continue; }           // the winner left after the game (a bye's too)
                if (a != w && b != w) { *this = Bracket{}; return false; }   // not one of the game's two (a bye's winner is the player behind it)
                out = int16_t(w);
            }
        Settle();
        return true;
    }

    bool valid() const { return !node_.empty(); }
    const std::vector<uint8_t>& seeds() const { return seeds_; }
    int Size() const { return node_.empty() ? 0 : int(node_[0].size()); }
    int Rounds() const { return rounds_; }
    int GamesInRound(int round) const { return round >= 0 && round < rounds_ ? Size() >> (round + 1) : 0; }

    // The winner of every game in play order, for the wire.
    std::vector<uint8_t> GameWinners() const {
        std::vector<uint8_t> w;
        for (int r = 0; r < rounds_; ++r)
            for (int g = 0; g < GamesInRound(r); ++g) {
                const int16_t v = node_[size_t(r + 1)][size_t(g)];
                w.push_back(v == kUnknown ? uint8_t(0) : v == 0 ? kBracketEmpty : uint8_t(v));
            }
        return w;
    }

    // The game to fight now. False when the bracket is decided (or empty).
    bool Current(Game& out) const { return Find(0, 0, out); }
    // The game after the current one that already has both players, for "next up". False when none is known yet.
    bool NextGame(Game& out) const {
        Game cur;
        if (!Current(cur)) return false;
        return Find(cur.round, cur.index + 1, out);
    }

    // The winner of the current game. False when there is none, or `winner` is not in it.
    bool Report(uint8_t winner) {
        Game g;
        if (!Current(g) || (winner != g.a && winner != g.b)) return false;
        node_[size_t(g.round + 1)][size_t(g.index)] = int16_t(winner);
        Settle();
        return true;
    }

    // A player left. His place becomes empty from the game he has not played yet (the opponent advances there); false when
    // he is not in the bracket, is out already, or is the champion.
    bool Remove(uint8_t e) {
        int r = 0, i = 0;
        if (!Position(e, r, i) || r >= rounds_) return false;
        if (node_[size_t(r + 1)][size_t(i / 2)] != kUnknown) return false;   // that game is played: he lost it
        node_[size_t(r)][size_t(i)] = 0;
        Settle();
        return true;
    }

    bool Done() const { return valid() && node_[size_t(rounds_)][0] != kUnknown; }
    uint8_t Champion() const { return Done() && node_[size_t(rounds_)][0] > 0 ? uint8_t(node_[size_t(rounds_)][0]) : uint8_t(0); }
    bool Contains(uint8_t e) const { int r, i; return Position(e, r, i); }
    // Still in the running: in the bracket and not beaten (the champion included).
    bool InRunning(uint8_t e) const {
        int r = 0, i = 0;
        if (!Position(e, r, i)) return false;
        return r >= rounds_ || node_[size_t(r + 1)][size_t(i / 2)] == kUnknown;
    }

    // How a game is called: the Final, Semi-final, Quarter-final, then "Round of 16".
    std::string StageName(int round) const {
        const int k = rounds_ - round;                      // 1 for the final
        if (round < 0 || k < 1) return "";
        if (k == 1) return "Final";
        if (k == 2) return "Semi-final";
        if (k == 3) return "Quarter-final";
        return "Round of " + std::to_string(Size() >> round);
    }
    // A game that is no bye: the places it needs both hold (or will hold) a player.
    bool IsBye(int round, int index) const {
        if (round < 0 || round >= rounds_ || index < 0 || index >= GamesInRound(round)) return false;
        const int16_t a = node_[size_t(round)][size_t(2 * index)], b = node_[size_t(round)][size_t(2 * index + 1)];
        return a == 0 || b == 0;
    }
    // The games of a round that are played, and this game's number among them (1-based; 0 for a bye).
    int PlayedGames(int round) const {
        int n = 0;
        for (int g = 0; g < GamesInRound(round); ++g) n += IsBye(round, g) ? 0 : 1;
        return n;
    }
    int GameNumber(int round, int index) const {
        if (IsBye(round, index)) return 0;
        int n = 0;
        for (int g = 0; g <= index; ++g) n += IsBye(round, g) ? 0 : 1;
        return n;
    }

private:
    static constexpr int16_t kUnknown = -1;

    void Build() {
        const int p = BracketSize(seeds_.size());
        rounds_ = 0;
        for (int s = p; s > 1; s /= 2) ++rounds_;
        node_.assign(size_t(rounds_ + 1), {});
        for (int r = 0; r <= rounds_; ++r) node_[size_t(r)].assign(size_t(p >> r), kUnknown);
        const std::vector<int> places = BracketSeedPlaces(p);
        for (int j = 0; j < p; ++j) {
            const size_t seed = size_t(places[size_t(j)] - 1);
            node_[0][size_t(j)] = seed < seeds_.size() ? int16_t(seeds_[seed]) : int16_t(0);
        }
        Settle();
    }
    // Byes settle: an empty place against a player gives him the game, two empty places an empty result. Round by round.
    void Settle() {
        for (int r = 0; r < rounds_; ++r)
            for (int g = 0; g < GamesInRound(r); ++g) {
                int16_t& out = node_[size_t(r + 1)][size_t(g)];
                if (out != kUnknown) continue;
                const int16_t a = node_[size_t(r)][size_t(2 * g)], b = node_[size_t(r)][size_t(2 * g + 1)];
                if (a == kUnknown || b == kUnknown) continue;
                if (a == 0 || b == 0) out = a == 0 ? b : a;
            }
    }
    bool Find(int fromRound, int fromIndex, Game& out) const {
        for (int r = fromRound; r < rounds_; ++r)
            for (int g = r == fromRound ? fromIndex : 0; g < GamesInRound(r); ++g) {
                if (node_[size_t(r + 1)][size_t(g)] != kUnknown) continue;
                const int16_t a = node_[size_t(r)][size_t(2 * g)], b = node_[size_t(r)][size_t(2 * g + 1)];
                if (a > 0 && b > 0) { out = {r, g, uint8_t(a), uint8_t(b)}; return true; }
            }
        return false;
    }
    // The highest place holding `e`.
    bool Position(uint8_t e, int& round, int& index) const {
        if (e == 0 || !valid()) return false;
        for (int r = rounds_; r >= 0; --r)
            for (size_t i = 0; i < node_[size_t(r)].size(); ++i)
                if (node_[size_t(r)][i] == int16_t(e)) { round = r; index = int(i); return true; }
        return false;
    }

    std::vector<uint8_t> seeds_;
    std::vector<std::vector<int16_t>> node_;   // node_[0]: the places in bracket order; node_[r + 1][g]: the winner of game g of round r
    int rounds_ = 0;
};

// The wire's bracket (MatchInfo): `seeds` and `winners` hold together. Both empty is fine (no bracket).
inline bool ValidBracketWire(const std::vector<uint8_t>& seeds, const std::vector<uint8_t>& winners) {
    if (seeds.empty() && winners.empty()) return true;
    Bracket b;
    return b.Restore(seeds, winners);
}

}  // namespace hsm

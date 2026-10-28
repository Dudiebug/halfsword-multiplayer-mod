// Detour routing, step R0 (log only). From the lobby's ping matrix (every client's PingTable, session/hsm_peer_ping.h)
// the host picks, for every pair of clients, the fastest way to get one's poses to the other: straight (the mesh),
// through the host (the relay every build has), or through a third client. R0 only logs the choice; nothing is sent
// differently. The plan is steady: a new route must beat the current one clearly, twice in a row, and then holds.
#pragma once
#include "session/hsm_authority.h"
#include "session/hsm_peer_ping.h"
#include <array>
#include <cstdint>
#include <vector>

namespace hsm {

constexpr uint8_t kRouteDirect = 0xFE;            // A -> B straight
constexpr uint8_t kRouteHost = kHostSlot;          // A -> host -> B; any other value is the client slot relaying
constexpr uint32_t kRouteNone = 0xFFFFFFFFu;       // the cost of a path that cannot be used
constexpr uint32_t kRouteRelayMs = 8;              // a relay forwards on its next network poll: about half a frame
constexpr uint32_t kRouteLossMsPerPct = 2;         // a lost pose is a late pose
constexpr uint8_t kRouteMaxLossPct = 30;           // a hop losing more than this is never used
constexpr uint32_t kRouteMinGainMs = 10;           // a new route must save this much...
constexpr uint32_t kRouteMinGainPct = 15;          // ...and this share of the current cost
constexpr int kRouteConfirm = 2;                   // in this many plans in a row
constexpr uint32_t kRouteHoldMs = 10000;           // then the pair keeps its route this long
constexpr uint8_t kRouteMaxRelayPairs = 2;         // pairs one client relays for (both directions of a pair count once)

// What each slot measured to each other slot (round trip, loss). A hop's cost uses both ends' figures.
class RouteLinks {
public:
    void Note(uint8_t from, uint8_t to, uint16_t rttMs, uint8_t lossPct) {
        if (from >= kMaxFighters || to >= kMaxFighters || from == to) return;
        m_[from][to] = Link{rttMs, lossPct, true};
    }
    // A new table from `from` replaces everything it said before.
    void ClearRow(uint8_t from) { if (from < kMaxFighters) m_[from] = {}; }
    void Forget(uint8_t slot) {
        if (slot >= kMaxFighters) return;
        m_[slot] = {};
        for (auto& row : m_) row[slot] = Link{};
    }
    void Clear() { m_ = {}; }

    // One-way cost of the hop a <-> b in ms: half the mean known round trip plus the loss charge; kRouteNone when
    // neither end has an answer or either end loses too much.
    uint32_t HopMs(uint8_t a, uint8_t b) const {
        if (a >= kMaxFighters || b >= kMaxFighters || a == b) return kRouteNone;
        const Link& x = m_[a][b];
        const Link& y = m_[b][a];
        uint32_t sum = 0, known = 0, loss = 0;
        for (const Link* l : {&x, &y}) {
            if (!l->seen) continue;
            if (l->lossPct > loss) loss = l->lossPct;
            if (l->rttMs != kPingUnknown) { sum += l->rttMs; ++known; }
        }
        if (!known || loss > kRouteMaxLossPct) return kRouteNone;
        return sum / known / 2 + loss * kRouteLossMsPerPct;
    }

private:
    struct Link { uint16_t rttMs = kPingUnknown; uint8_t lossPct = 0; bool seen = false; };
    std::array<std::array<Link, kMaxFighters>, kMaxFighters> m_{};
};

inline uint32_t RouteAdd(uint32_t a, uint32_t b) { return a == kRouteNone || b == kRouteNone ? kRouteNone : a + b; }

// The one-way cost from a to b along route (kRouteDirect, kRouteHost or a relaying client slot).
inline uint32_t RoutePathMs(const RouteLinks& l, uint8_t a, uint8_t b, uint8_t route) {
    if (route == kRouteDirect) return l.HopMs(a, b);
    if (route != kRouteHost && (route >= kMaxFighters || route == a || route == b)) return kRouteNone;
    return RouteAdd(RouteAdd(l.HopMs(a, route), l.HopMs(route, b)), kRouteRelayMs);
}

enum class RouteWhy : uint8_t { New, Faster, Lost };

struct RouteChange {
    uint8_t a = 0, b = 0;                  // a < b
    uint8_t from = kRouteHost, to = kRouteHost;
    uint32_t directMs = kRouteNone, hostMs = kRouteNone, toMs = kRouteNone;
    RouteWhy why = RouteWhy::New;
};

class RoutePlanner {
public:
    explicit RoutePlanner(uint8_t maxRelayPairs = kRouteMaxRelayPairs) : maxRelay_(maxRelayPairs) {}

    // One plan over the client slots in the lobby (the host's slot is ignored). Returns the pairs whose route changed.
    std::vector<RouteChange> Plan(const RouteLinks& l, const std::vector<uint8_t>& clients, uint32_t nowMs) {
        std::array<bool, kMaxFighters> in{};
        for (uint8_t s : clients) if (s != kHostSlot && s < kMaxFighters) in[s] = true;
        std::vector<RouteChange> out;
        for (uint8_t a = 1; a < kMaxFighters; ++a) {
            for (uint8_t b = uint8_t(a + 1); b < kMaxFighters; ++b) {
                if (!in[a] || !in[b]) continue;
                Pair& p = p_[a][b];
                const uint32_t direct = l.HopMs(a, b), host = RoutePathMs(l, a, b, kRouteHost);
                const uint8_t plain = direct != kRouteNone && direct <= host ? kRouteDirect : kRouteHost;
                auto change = [&](uint8_t to, RouteWhy why) {
                    out.push_back(RouteChange{a, b, p.route, to, direct, host, RoutePathMs(l, a, b, to), why});
                    p.route = to; p.confirm = 0;
                    if (why != RouteWhy::New) { p.held = true; p.holdUntil = nowMs + kRouteHoldMs; }
                };
                if (!p.known) { p.known = true; change(plain, RouteWhy::New); continue; }
                const uint32_t cur = RoutePathMs(l, a, b, p.route);
                const bool relayGone = p.route != kRouteDirect && p.route != kRouteHost && !in[p.route];
                if ((cur == kRouteNone || relayGone) && p.route != kRouteHost) {   // the host relay always stays
                    if (plain != p.route) change(plain, RouteWhy::Lost);
                    continue;
                }
                uint8_t best = plain;
                uint32_t bestMs = plain == kRouteDirect ? direct : host;
                for (uint8_t c = 1; c < kMaxFighters; ++c) {
                    if (!in[c] || c == a || c == b) continue;
                    if (Load(c) - (p.route == c ? 1 : 0) >= maxRelay_) continue;
                    const uint32_t ms = RoutePathMs(l, a, b, c);
                    if (ms < bestMs) { best = c; bestMs = ms; }
                }
                if (p.held && int32_t(nowMs - p.holdUntil) >= 0) p.held = false;
                if (best == p.route || p.held) { p.confirm = 0; continue; }
                const uint32_t pct = cur == kRouteNone ? 0 : cur * kRouteMinGainPct / 100;
                const uint32_t need = pct > kRouteMinGainMs ? pct : kRouteMinGainMs;
                if (bestMs == kRouteNone || (cur != kRouteNone && (bestMs >= cur || cur - bestMs < need))) { p.confirm = 0; continue; }
                if (p.pending != best) { p.pending = best; p.confirm = 0; }
                if (++p.confirm >= kRouteConfirm) change(best, RouteWhy::Faster);
            }
        }
        return out;
    }

    // The route a <-> b now follows (either order); kRouteHost for a pair never planned.
    uint8_t RouteOf(uint8_t a, uint8_t b) const {
        if (a > b) { const uint8_t t = a; a = b; b = t; }
        if (b >= kMaxFighters || a == b || !p_[a][b].known) return kRouteHost;
        return p_[a][b].route;
    }
    // Pairs relayed through client c.
    int Load(uint8_t c) const {
        int n = 0;
        for (uint8_t a = 1; a < kMaxFighters; ++a)
            for (uint8_t b = uint8_t(a + 1); b < kMaxFighters; ++b)
                if (p_[a][b].known && p_[a][b].route == c) ++n;
        return n;
    }
    // Slot left: its own pairs start over when it is back. Pairs relayed through it fall back in the next Plan.
    void Forget(uint8_t slot) {
        if (slot >= kMaxFighters) return;
        for (uint8_t o = 0; o < kMaxFighters; ++o) {
            if (o < slot) p_[o][slot] = Pair{};
            else if (o > slot) p_[slot][o] = Pair{};
        }
    }
    void Clear() { p_ = {}; }

private:
    struct Pair {
        bool known = false, held = false;
        uint8_t route = kRouteHost, pending = kRouteHost;
        int confirm = 0;
        uint32_t holdUntil = 0;
    };
    std::array<std::array<Pair, kMaxFighters>, kMaxFighters> p_{};   // [a][b], a < b
    int maxRelay_;
};

}  // namespace hsm

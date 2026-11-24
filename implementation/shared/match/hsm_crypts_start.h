// Shared Crypts maps in Players vs Bots (docs/PLAN_SHARED_CRYPTS.md): the players start together at the entrance and the
// bots wait in the rooms beyond, so the team goes through the dungeon together and fights its way in. A spawn point's
// depth is its room's distance, in joins, from the layout's first room (RowDepths); the points of depth kStartDepth or
// less are the players' side, the deeper ones the bots' (DepthSides). Each bot takes the next point of a spread walk
// over its side that keeps clear of the players (SpreadPick). Game-independent.
#pragma once
#include "match/hsm_crypts_layout.h"
#include "match/hsm_match.h"
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace hsm::crypts {

inline constexpr int kStartDepth = 1;   // the entrance room and the rooms joined to it

// Each row's depth: 0 for a row with no parent, else its parent's plus 1; -1 when the parent is not an earlier row.
inline std::vector<int> RowDepths(const Layout& l) {
    std::vector<int> depth(l.count, -1);
    for (size_t i = 0; i < l.count; ++i) {
        const int p = l.rows[i].parent;
        if (p < 0) depth[i] = 0;
        else if (size_t(p) < i && depth[size_t(p)] >= 0) depth[i] = depth[size_t(p)] + 1;
    }
    return depth;
}

// The sides of a Players vs Bots start, one per spawn point: kHumanTeam at depth startDepth or less, kBotTeam deeper,
// kNoTeam for a point of no known depth (-1), which neither side takes. Empty when either side would have no point
// (a one-room layout): the caller keeps its own split.
inline std::vector<int8_t> DepthSides(const std::vector<int>& pointDepths, int startDepth = kStartDepth) {
    std::vector<int8_t> side(pointDepths.size(), kNoTeam);
    size_t players = 0, bots = 0;
    for (size_t i = 0; i < pointDepths.size(); ++i) {
        if (pointDepths[i] < 0) continue;
        if (pointDepths[i] <= startDepth) { side[i] = kHumanTeam; ++players; }
        else { side[i] = kBotTeam; ++bots; }
    }
    if (players == 0 || bots == 0) return {};
    return side;
}

// The walk's step over m points: the whole number nearest m times the golden ratio's 0.618 that shares no factor with m,
// so m steps in a row visit every point once and two steps in a row land far apart in the list.
inline size_t SpreadStride(size_t m) {
    if (m <= 2) return 1;
    size_t s = size_t(double(m) * 0.6180339887498949 + 0.5);
    while (std::gcd(s, m) != 1) ++s;   // stops by m - 1 at the latest
    return s;
}

// The n-th bot's point: the n-th step of the spread walk over the side's points that have no enemy within
// kSafeRespawnCm (a later wave does not land on players who went in). With no such point, PickRespawn's (the farthest).
inline size_t SpreadPick(const std::vector<Point3>& points, const std::vector<int8_t>& sides, int team, size_t n,
                         const std::vector<Point3>& enemies) {
    std::vector<size_t> clear;
    for (size_t i = 0; i < points.size() && i < sides.size(); ++i) {
        if (sides[i] != team) continue;
        bool near = false;
        for (const Point3& e : enemies) near = near || Dist2(points[i], e) < kSafeRespawnCm * kSafeRespawnCm;
        if (!near) clear.push_back(i);
    }
    if (clear.empty()) return PickRespawn(points, sides, team, enemies);
    const size_t m = clear.size();
    return clear[(n % m) * SpreadStride(m) % m];
}

}  // namespace hsm::crypts

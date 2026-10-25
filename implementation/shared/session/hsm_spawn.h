// Spawn-point selection. Game-independent, unit-tested.
// Picks spread-out points with farthest-point sampling (deterministic: starts at candidate 0). When there are
// more fighters than candidates, candidates are reused with an increasing ring number so the caller can offset
// them (ring * radius around the point) instead of stacking fighters.
// SpreadPositions / FreeSpot turn picks into places no two fighters share: a hexagonal spread of slots around a point,
// never closer than kMinFighterGapCm. SaneStandOffset and StandableRing check the floor traces of spawner-less maps;
// NavigableRing finds their points on the navmesh first, the way the Abyss places its own enemies.
#pragma once
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace hsm {

struct Point3 { double x = 0, y = 0, z = 0; };
struct SpawnPick { size_t index = 0; int ring = 0; };

inline double Dist2(const Point3& a, const Point3& b) { double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z; return dx * dx + dy * dy + dz * dz; }

inline std::vector<SpawnPick> SelectSpread(const std::vector<Point3>& candidates, size_t count) {
    std::vector<SpawnPick> picks;
    if (candidates.empty() || count == 0) return picks;
    std::vector<bool> used(candidates.size(), false);
    std::vector<size_t> chosen;
    size_t distinct = count < candidates.size() ? count : candidates.size();
    chosen.push_back(0); used[0] = true;
    while (chosen.size() < distinct) {
        size_t best = 0; double bestScore = -1;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (used[i]) continue;
            double nearest = INFINITY;
            for (size_t c : chosen) nearest = std::fmin(nearest, Dist2(candidates[i], candidates[c]));
            if (nearest > bestScore) { bestScore = nearest; best = i; }   // ties keep the lowest index
        }
        chosen.push_back(best); used[best] = true;
    }
    for (size_t k = 0; k < count; ++k) picks.push_back({chosen[k % chosen.size()], int(k / chosen.size())});
    return picks;
}

constexpr double kMinFighterGapCm = 250.0;   // a fallen Willie is about 200 cm long (test 5: 120 cm left bodies inside each other)
constexpr size_t kSpreadSlots = 37;          // rings 0..3 of SpreadSlot: what FreeSpot searches
constexpr double kStandDefaultCm = 110.0;    // actor height above its floor where the stock spawner put the host (Yard: 110-114)
constexpr double kStandMaxCm = 200.0;
constexpr double kMaxStepCm = 100.0;         // StandableRing: a floor this far above or below the centre's is a ledge or a pit
constexpr double kNavToleranceCm = 150.0;    // NavigableRing: how far from its ring point a navmesh answer may be

// Slot k of a hexagonal spread around c: slot 0 is c, ring n >= 1 holds 6n slots on a circle of n * gap, so any two
// slots are at least `gap` apart (neighbours on ring n: 2 n gap sin(pi / 6n) >= gap). Same height as c.
inline Point3 SpreadSlot(const Point3& c, size_t k, double gap) {
    if (k == 0) return c;
    size_t n = 1;
    while (1 + 3 * n * (n + 1) <= k) ++n;
    const size_t j = k - (1 + 3 * n * (n - 1));
    const double angle = double(j) * (6.283185307179586 / double(6 * n));
    return {c.x + std::cos(angle) * gap * double(n), c.y + std::sin(angle) * gap * double(n), c.z};
}

inline double FlatDist2(const Point3& a, const Point3& b) { double dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy; }

// The first spread slot around c, from `first` on, that no point of `taken` is within `gap` of, horizontally (a body
// lying on top of the spot counts); slot `first` itself when none of the kSpreadSlots slots is free.
inline Point3 FreeSpot(const Point3& c, const std::vector<Point3>& taken, size_t first, double gap) {
    const double g2 = gap * gap - 1e-6;
    for (size_t i = 0; i < kSpreadSlots; ++i) {
        const Point3 p = SpreadSlot(c, first + i, gap);
        bool clear = true;
        for (const Point3& t : taken) if (FlatDist2(p, t) < g2) { clear = false; break; }
        if (clear) return p;
    }
    return SpreadSlot(c, first, gap);
}

// Places for `count` fighters: SelectSpread's picks in order, each on the first slot around its candidate (from its
// ring on) that keeps it `gap` from the places before it. Candidates far enough apart are used as they are.
inline std::vector<Point3> SpreadPositions(const std::vector<Point3>& candidates, size_t count, double gap = kMinFighterGapCm) {
    std::vector<Point3> out;
    for (const SpawnPick& pk : SelectSpread(candidates, count)) out.push_back(FreeSpot(candidates[pk.index], out, size_t(pk.ring), gap));
    return out;
}

// A measured stand height (the host's actor z minus the floor traced under it) fit for every spawn. Negative (the
// trace hit something above the floor: test 5's Abyss read -96 cm) or not finite: kStandDefaultCm, so a spawn is never
// lowered into the floor; above kStandMaxCm: kStandMaxCm.
inline double SaneStandOffset(double raw) {
    if (!std::isfinite(raw) || raw < 0) return kStandDefaultCm;
    return raw > kStandMaxCm ? kStandMaxCm : raw;
}

// Spawner-less maps: `n` points on a circle of `radius` around `center` where a fighter can stand: floorAt (a trace
// down at the point, nullopt for no hit) finds a floor within kMaxStepCm of the centre's own, and no fighter in
// `fighters` is within `clear` of the point, horizontally. Each point is returned at its traced floor height.
template <class FloorAt>
std::vector<Point3> StandableRing(const Point3& center, double centerFloorZ, FloorAt floorAt, const std::vector<Point3>& fighters,
                                  double radius, int n = 8, double clear = 150.0) {
    std::vector<Point3> out;
    for (int i = 0; i < n; ++i) {
        const double angle = double(i) * (6.283185307179586 / double(n));
        Point3 p{center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius, center.z};
        const std::optional<double> z = floorAt(p);
        if (!z || !std::isfinite(*z) || std::fabs(*z - centerFloorZ) > kMaxStepCm) continue;
        bool occupied = false;
        for (const Point3& f : fighters) if (FlatDist2(p, f) < clear * clear) { occupied = true; break; }
        if (occupied) continue;
        p.z = *z;
        out.push_back(p);
    }
    return out;
}

// Spawner-less maps with a navmesh (the Abyss). The level script places each enemy at a random navigable point near
// an aim point (K2_GetRandomLocationInNavigableRadius, then Spawn_NPC: Abyss_Map_Open_EA ReceiveTick); here the aim
// points are the ring's `n` points around `center`. navPoint(aim, tolerance) is that query (nullopt: no navmesh
// there). An answer is kept when it is within `tolerance` of its aim, floorAt finds a floor under it within
// kMaxStepCm of the centre's own, no fighter is within `clear` of it and no point kept before it within
// kMinFighterGapCm (horizontally). Each point is returned at its traced floor height.
template <class NavPoint, class FloorAt>
std::vector<Point3> NavigableRing(const Point3& center, double centerFloorZ, NavPoint navPoint, FloorAt floorAt,
                                  const std::vector<Point3>& fighters, double radius, double tolerance = kNavToleranceCm,
                                  int n = 8, double clear = 150.0) {
    std::vector<Point3> out;
    for (int i = 0; i < n; ++i) {
        const double angle = double(i) * (6.283185307179586 / double(n));
        const Point3 aim{center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius, center.z};
        const std::optional<Point3> at = navPoint(aim, tolerance);
        if (!at || !std::isfinite(at->x) || !std::isfinite(at->y) || !std::isfinite(at->z)) continue;
        if (FlatDist2(*at, aim) > tolerance * tolerance) continue;
        const std::optional<double> z = floorAt(*at);
        if (!z || !std::isfinite(*z) || std::fabs(*z - centerFloorZ) > kMaxStepCm) continue;
        bool taken = false;
        for (const Point3& f : fighters) if (FlatDist2(*at, f) < clear * clear) { taken = true; break; }
        for (const Point3& k : out) if (FlatDist2(*at, k) < kMinFighterGapCm * kMinFighterGapCm) { taken = true; break; }
        if (taken) continue;
        out.push_back({at->x, at->y, *z});
    }
    return out;
}

}  // namespace hsm

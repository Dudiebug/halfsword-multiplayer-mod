// Spawn-point selection. Game-independent, unit-tested.
// Picks spread-out points with farthest-point sampling (deterministic: starts at candidate 0). When there are
// more fighters than candidates, candidates are reused with an increasing ring number so the caller can offset
// them (ring * radius around the point) instead of stacking fighters.
#pragma once
#include <cmath>
#include <cstddef>
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

}  // namespace hsm

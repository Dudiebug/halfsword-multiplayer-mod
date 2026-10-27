// role=server has no host fighter to read a spawn centre from. The anchor is the stock pawn's position when the level
// made one (captured before the server strips it), else a PlayerStart near the map's spawners, else a spawner.
// Game-independent, unit-tested.
#pragma once
#include "session/hsm_spawn.h"
#include <optional>
#include <vector>

namespace hsm {

enum class AnchorKind : uint8_t { StockPawn, PlayerStart, Spawner, None };
struct SpawnAnchor { Point3 pos; AnchorKind kind = AnchorKind::None; };

namespace anchor_detail {
inline Point3 Centroid(const std::vector<Point3>& v) {
    Point3 c;
    for (const Point3& p : v) { c.x += p.x; c.y += p.y; c.z += p.z; }
    const double n = double(v.size());
    return {c.x / n, c.y / n, c.z / n};
}
// Nearest to `to`; ties keep the lowest index. v must not be empty.
inline const Point3& Nearest(const std::vector<Point3>& v, const Point3& to) {
    size_t best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (Dist2(v[i], to) < Dist2(v[best], to)) best = i;
    return v[best];
}
}  // namespace anchor_detail

inline SpawnAnchor ChooseAnchor(const std::optional<Point3>& stockPawn, const std::vector<Point3>& playerStarts, const std::vector<Point3>& spawners) {
    if (stockPawn) return {*stockPawn, AnchorKind::StockPawn};
    if (!playerStarts.empty()) {
        if (spawners.empty()) return {playerStarts.front(), AnchorKind::PlayerStart};
        return {anchor_detail::Nearest(playerStarts, anchor_detail::Centroid(spawners)), AnchorKind::PlayerStart};
    }
    if (!spawners.empty()) return {anchor_detail::Nearest(spawners, anchor_detail::Centroid(spawners)), AnchorKind::Spawner};
    return {};
}

// A PlayerStart's or spawner's height is not a Willie's stand height; only a measured stock pawn gives one.
inline double StandOffsetFor(AnchorKind kind, double raw) { return kind == AnchorKind::StockPawn ? SaneStandOffset(raw) : kStandDefaultCm; }

}  // namespace hsm

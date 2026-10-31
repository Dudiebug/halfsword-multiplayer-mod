// The Abyss in a session (sync audit 36, 37). Game-independent, unit-tested.
// The level script (Abyss_Map_Open_EA ReceiveTick) builds everything round GetPlayerCharacter(0) only:
// - floor tiles on a 2000 uu grid, the 3x3 round player 0's cell (Round(x / 2000)), each spawned at z 0 and dropped
//   at 4000 uu or more from player 0. A remote fighter a tile away has no floor on this machine and falls.
// - its own enemies, whenever player 0 is at least 250 + 250 x (enemies) from "Last Spawn Location" (xy). In a session
//   the host spawns every enemy, so each machine pins that location to its player 0 every frame and the level never
//   spawns one.
// PlanAbyssFloor says which tiles of the mod's own to add and drop so every fighter but player 0 stands on floor.
#pragma once
#include <cmath>
#include <compare>
#include <optional>
#include <set>
#include <vector>

namespace hsm {

constexpr double kAbyssTileCm = 2000;
constexpr double kAbyssOwnTileCm = 3000;    // a level tile this far from player 0 gets one of ours under it too: the level
                                            // drops its own at 4000, and a fighter must never stand on a gap
constexpr double kAbyssOwnTileDropCm = 2500;   // ours go again when a level tile is there and player 0 this close

struct TileCell {
    int x = 0, y = 0;
    auto operator<=>(const TileCell&) const = default;
};
struct FloorPoint { double x = 0, y = 0; };

// The level's Round (FMath::RoundToInt: half up).
inline TileCell AbyssCellOf(double x, double y) { return {int(std::floor(x / kAbyssTileCm + 0.5)), int(std::floor(y / kAbyssTileCm + 0.5))}; }
inline FloorPoint AbyssCellCenter(TileCell c) { return {c.x * kAbyssTileCm, c.y * kAbyssTileCm}; }

struct FloorPlan { std::vector<TileCell> add, drop; };

// me: player 0 here (none: no level tile can be relied on). others: every other fighter. level: cells with a tile the
// level spawned; ours: cells with one of ours. Every cell of the 3x3 round each other fighter gets one of ours unless a
// level tile is there and player 0 is near enough that the level keeps it (hysteresis between kAbyssOwnTileDropCm and
// kAbyssOwnTileCm); ours no fighter needs are dropped.
inline FloorPlan PlanAbyssFloor(std::optional<FloorPoint> me, const std::vector<FloorPoint>& others, const std::set<TileCell>& level,
                                const std::set<TileCell>& ours) {
    std::set<TileCell> needed;
    for (const FloorPoint& p : others) {
        const TileCell c = AbyssCellOf(p.x, p.y);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) needed.insert({c.x + dx, c.y + dy});
    }
    auto fromMe = [&](TileCell c) {
        if (!me) return HUGE_VAL;
        const FloorPoint o = AbyssCellCenter(c);
        return std::hypot(o.x - me->x, o.y - me->y);
    };
    FloorPlan plan;
    for (TileCell c : needed) {
        const bool levelHas = level.count(c) != 0;
        if (ours.count(c)) { if (levelHas && fromMe(c) < kAbyssOwnTileDropCm) plan.drop.push_back(c); }
        else if (!levelHas || fromMe(c) >= kAbyssOwnTileCm) plan.add.push_back(c);
    }
    for (TileCell c : ours) if (!needed.count(c)) plan.drop.push_back(c);
    return plan;
}

}  // namespace hsm

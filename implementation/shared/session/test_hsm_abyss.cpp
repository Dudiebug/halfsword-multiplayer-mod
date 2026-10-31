// The Abyss floor under remote fighters (sync audit 36). Run via implementation/shared/run_tests.sh test_hsm_abyss.
#include "session/hsm_abyss.h"
#include <algorithm>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Has(const std::vector<TileCell>& v, TileCell c) { return std::find(v.begin(), v.end(), c) != v.end(); }

int main() {
    // The level's grid: Round(x / 2000), half up.
    CHECK((AbyssCellOf(0, 0) == TileCell{0, 0}) && (AbyssCellOf(999, -999) == TileCell{0, 0}));
    CHECK((AbyssCellOf(1000, -1000) == TileCell{1, 0}) && (AbyssCellOf(-1001, 3000) == TileCell{-1, 2}));

    // Alone: nothing of ours.
    std::set<TileCell> level;
    for (int x = -1; x <= 1; ++x) for (int y = -1; y <= 1; ++y) level.insert({x, y});
    FloorPlan p = PlanAbyssFloor(FloorPoint{0, 0}, {}, level, {});
    CHECK(p.add.empty() && p.drop.empty());

    // A remote fighter next to player 0: the level's own 3x3 holds them, ours fill the cells it lacks.
    p = PlanAbyssFloor(FloorPoint{0, 0}, {{1500, 0}}, level, {});
    CHECK(p.add.size() == 3 && Has(p.add, {2, -1}) && Has(p.add, {2, 0}) && Has(p.add, {2, 1}) && p.drop.empty());

    // Far away: all nine of theirs are ours.
    p = PlanAbyssFloor(FloorPoint{0, 0}, {{20000, 20000}}, level, {});
    CHECK(p.add.size() == 9 && Has(p.add, {10, 10}) && p.drop.empty());

    // A level tile 3000 uu or more from player 0 is about to go (the level drops at 4000): ours goes under it too.
    p = PlanAbyssFloor(FloorPoint{-1000, 0}, {{1000, 0}}, level, {});
    CHECK(Has(p.add, {1, 0}) && Has(p.add, {1, 1}) && !Has(p.add, {0, 0}));

    // Hysteresis: ours stays until player 0 is within 2500 uu of a level tile in that cell.
    std::set<TileCell> ours{{1, 0}};
    CHECK(PlanAbyssFloor(FloorPoint{-700, 0}, {{1000, 0}}, level, ours).drop.empty());          // 2700 uu: kept
    p = PlanAbyssFloor(FloorPoint{0, 0}, {{1000, 0}}, level, ours);
    CHECK(p.drop.size() == 1 && Has(p.drop, {1, 0}) && !Has(p.add, {1, 0}));                     // 2000 uu: the level's does
    CHECK(PlanAbyssFloor(FloorPoint{0, 0}, {{1000, 0}}, {}, ours).drop.empty());                 // no level tile: ours stays

    // Nobody needs it any more: dropped. Two fighters share cells: one tile each.
    p = PlanAbyssFloor(FloorPoint{0, 0}, {}, level, {{5, 5}, {1, 0}});
    CHECK(p.drop.size() == 2 && p.add.empty());
    p = PlanAbyssFloor(FloorPoint{0, 0}, {{20000, 0}, {21000, 0}}, level, {});
    CHECK(p.add.size() == 12);

    // No player 0 (a dedicated server, or between lives): every needed cell gets ours.
    p = PlanAbyssFloor(std::nullopt, {{0, 0}}, level, {});
    CHECK(p.add.size() == 9);

    if (g_fail == 0) std::printf("test_hsm_abyss: all passed\n");
    return g_fail == 0 ? 0 : 1;
}

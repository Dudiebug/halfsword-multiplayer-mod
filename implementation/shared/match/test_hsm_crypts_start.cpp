// Unit tests for hsm_crypts_start.h: a shared Crypts map's Players vs Bots start, the players at the entrance and the
// bots spread through the rooms beyond (docs/PLAN_CRYPTS_COOP.md). Run via implementation/shared/run_tests.sh test_hsm_crypts_start.
#include "match/hsm_crypts_start.h"
#include <algorithm>
#include <cstdio>
#include <numeric>
#include <set>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECKF(c, i) do { if (!(c)) { std::printf("FAIL %s:%d: %s (case %d)\n", __FILE__, __LINE__, #c, int(i)); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::crypts;

// The walked run of the 2026-11-13 probe log (eleven rooms from the entrance to the treasury and the exit corridor) and a one-room
// table, built by hand now that the lobby has no layout maps.
static const Row kDescentRows[] = {
    {"Entrance_2", -1, -1, 0},
    {"Corridor_3", 0, 1, 0},
    {"Corridor_1_Shortcut", 0, 2, 1},
    {"Chamber_1", 1, 1, 1},
    {"Stairs_1_Shortcut", 2, 0, 0},
    {"Corridor_1", 3, 0, 0},
    {"Chamber_5", 5, 1, 1},
    {"Stairs_2", 6, 0, 0},
    {"Hall_1", 7, 1, 3},
    {"Treasury_1", 8, 0, 0},
    {"Corridor_3_Exit", 8, 2, 0},
};
static const Row kOneRoom[] = {{"Sepulchre", -1, -1, -1}};
static const Layout kDescent{"Descent", "Descent", {}, kDescentRows, uint8_t(sizeof(kDescentRows) / sizeof(Row))};
static const Layout kPurgatory{"Purgatory", "Purgatory", {}, kOneRoom, 1};

// The Descent, worked out by hand: the entrance, its two corridors, then one room further each join down to the Hall,
// with the Treasury and the exit corridor off the Hall.
static void TestDepthsDescent() {
    const std::vector<int> want = {0, 1, 1, 2, 2, 3, 4, 5, 6, 7, 7};
    CHECK(RowDepths(kDescent) == want);
    CHECK(RowDepths(kPurgatory) == std::vector<int>{0});
}

// A parent that is not an earlier row has no depth, nor have the rows below it.
static void TestDepthsBroken() {
    const Row rows[] = {{"Entrance_2", -1, -1, 0}, {"Corridor_1", 2, 0, 0}, {"Chamber_1", 1, 0, 0}, {"Corridor_3", 0, 1, 0}, {"Stairs_1", 4, 0, 0}};
    const Layout l{"Broken", "Broken", {}, rows, 5};
    CHECK(RowDepths(l) == (std::vector<int>{0, -1, -1, 1, -1}));
    const Layout none{"None", "None", {}, nullptr, 0};
    CHECK(RowDepths(none).empty());
}

static void TestDepthSides() {
    const std::vector<int8_t> s = DepthSides({0, 1, 2, -1, 7, 1});
    CHECK(s == (std::vector<int8_t>{kHumanTeam, kHumanTeam, kBotTeam, kNoTeam, kBotTeam, kHumanTeam}));
    CHECK(DepthSides({0, 0, 1}).empty());              // nothing deeper: no bot side
    CHECK(DepthSides({2, 3, 4}).empty());              // nothing at the entrance: no player side
    CHECK(DepthSides({-1, -1}).empty());
    CHECK(DepthSides({}).empty());
    CHECK(DepthSides({0, 2, 3}, 2) == (std::vector<int8_t>{kHumanTeam, kHumanTeam, kBotTeam}));   // the start depth moves the line
    CHECK(DepthSides({0, 1}, 0) == (std::vector<int8_t>{kHumanTeam, kBotTeam}));
}

// The Descent splits into an entrance side and a deeper side, by the rooms' spawn counts.
static void TestLayoutsSplit() {
    auto sides = [](const Layout& l, int& players, int& bots) {
        const std::vector<int> depth = RowDepths(l);
        std::vector<int> pointDepths;
        for (size_t i = 0; i < l.count; ++i)
            if (const RoomClass* r = FindRoom(l.rows[i].room)) pointDepths.insert(pointDepths.end(), r->spawns, depth[i]);
        players = bots = 0;
        for (int8_t s : DepthSides(pointDepths)) { players += s == kHumanTeam; bots += s == kBotTeam; }
    };
    int players = 0, bots = 0;
    sides(kDescent, players, bots);
    CHECK(players == 7 + 3 + 3 && bots == SpawnCount(kDescent) - 13);
    sides(kPurgatory, players, bots);
    CHECK(players == 0 && bots == 0);                  // one room: no split
}

static void TestStride() {
    CHECK(SpreadStride(0) == 1 && SpreadStride(1) == 1 && SpreadStride(2) == 1);
    CHECK(SpreadStride(12) == 7);
    CHECK(SpreadStride(10) == 7);                       // 6 shares 2 with 10
    CHECK(SpreadStride(100) == 63);                     // 62 shares 2 with 100
    for (size_t m = 3; m <= 300; ++m) {
        const size_t s = SpreadStride(m);
        CHECKF(std::gcd(s, m) == 1 && s > 1 && s < m, m);
    }
}

static std::vector<Point3> Line(size_t n) {
    std::vector<Point3> p;
    for (size_t i = 0; i < n; ++i) p.push_back({double(i) * 1000.0, 0, 0});
    return p;
}

// m picks in a row on a side of m points visit each once, never a point of another side or of no side.
static void TestSpreadVisitsAll() {
    for (size_t m : {size_t(1), size_t(2), size_t(7), size_t(12), size_t(31)}) {
        std::vector<Point3> pts = Line(m + 3);
        std::vector<int8_t> sides(m + 3, kBotTeam);
        sides[0] = kHumanTeam; sides[1] = kNoTeam; sides[m + 2] = kHumanTeam;
        std::set<size_t> seen;
        for (size_t n = 0; n < m; ++n) {
            const size_t i = SpreadPick(pts, sides, kBotTeam, n, {});
            CHECKF(sides[i] == kBotTeam, int(m));
            seen.insert(i);
        }
        CHECKF(seen.size() == m, int(m));
        CHECKF(SpreadPick(pts, sides, kBotTeam, m, {}) == SpreadPick(pts, sides, kBotTeam, 0, {}), int(m));   // then round again
    }
    // Two picks in a row land apart: 12 points 10 m apart, the stride of 7 puts the next bot 50 m or more away.
    std::vector<Point3> pts = Line(12);
    std::vector<int8_t> sides(12, kBotTeam);
    for (size_t n = 0; n + 1 < 12; ++n) {
        const size_t a = SpreadPick(pts, sides, kBotTeam, n, {}), b = SpreadPick(pts, sides, kBotTeam, n + 1, {});
        CHECKF((a > b ? a - b : b - a) >= 5, int(n));
    }
}

// A point with a player within kSafeRespawnCm is passed over; with every point near one, the farthest (PickRespawn's).
static void TestSpreadKeepsClear() {
    std::vector<Point3> pts = Line(6);                  // 0, 10, 20, 30, 40, 50 m
    std::vector<int8_t> sides(6, kBotTeam);
    const std::vector<Point3> players = {{0, 0, 0}};   // at the first point: 0 and 10 m are within 15 m
    std::set<size_t> seen;
    for (size_t n = 0; n < 8; ++n) seen.insert(SpreadPick(pts, sides, kBotTeam, n, players));
    CHECK(seen == (std::set<size_t>{2, 3, 4, 5}));
    const std::vector<Point3> edge = {{2000 - kSafeRespawnCm, 0, 0}};   // exactly kSafeRespawnCm from the 20 m point: clear
    seen.clear();
    for (size_t n = 0; n < 8; ++n) seen.insert(SpreadPick(pts, sides, kBotTeam, n, edge));
    CHECK(seen.count(2) == 1 && seen.count(0) == 0 && seen.count(1) == 0);
    std::vector<Point3> crowd;
    for (const Point3& p : pts) crowd.push_back({p.x, 500, 0});   // 5 m off every point
    crowd.pop_back();                                  // ...but the last
    crowd.back().y = 1400;                             // and 14 m off the fifth
    CHECK(SpreadPick(pts, sides, kBotTeam, 0, crowd) == 5 && SpreadPick(pts, sides, kBotTeam, 3, crowd) == 5);
    crowd.push_back({5000, 500, 0});                   // now every point has a player near: the farthest from them
    CHECK(SpreadPick(pts, sides, kBotTeam, 0, crowd) == PickRespawn(pts, sides, kBotTeam, crowd));
    CHECK(SpreadPick(pts, sides, kBotTeam, 0, crowd) == 4);
    std::vector<int8_t> human(6, kHumanTeam);         // no point on the bots' side: PickRespawn's answer
    CHECK(SpreadPick(pts, human, kBotTeam, 2, {}) == PickRespawn(pts, human, kBotTeam, {}));
}

int main() {
    TestDepthsDescent();
    TestDepthsBroken();
    TestDepthSides();
    TestLayoutsSplit();
    TestStride();
    TestSpreadVisitsAll();
    TestSpreadKeepsClear();
    if (g_fail) { std::printf("test_hsm_crypts_start: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypts_start: all passed\n");
    return 0;
}

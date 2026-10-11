// AC-3: spawn selection returns distinct, maximally spread points for 1..6 fighters and degrades
// deterministically (reuse with ring offsets) when there are fewer candidates.
#include "session/hsm_spawn.h"
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestLine() {
    std::vector<Point3> line; for (int i = 0; i <= 10; ++i) line.push_back({double(i) * 100, 0, 0});
    auto p = SelectSpread(line, 3);
    CHECK(p.size() == 3 && p[0].index == 0 && p[1].index == 10 && p[2].index == 5);   // ends first, then middle
    for (const SpawnPick& s : p) CHECK(s.ring == 0);
}

static void TestDistinctAndSpread() {
    // 4 corners of a square plus 4 points clustered near the center: 4 fighters must take the 4 corners.
    std::vector<Point3> c = {{0, 0, 0}, {1000, 1000, 0}, {1000, 0, 0}, {0, 1000, 0}, {490, 500, 0}, {510, 500, 0}, {500, 490, 0}, {500, 510, 0}};
    for (size_t n = 1; n <= 6; ++n) {
        auto p = SelectSpread(c, n);
        std::set<size_t> idx; for (auto& s : p) idx.insert(s.index);
        CHECK(p.size() == n && idx.size() == n);                                         // distinct when enough candidates
    }
    auto four = SelectSpread(c, 4);
    std::set<size_t> corners; for (auto& s : four) corners.insert(s.index);
    CHECK(corners == std::set<size_t>({0, 1, 2, 3}));
}

static void TestFewerCandidatesAndDeterminism() {
    std::vector<Point3> two = {{0, 0, 0}, {500, 0, 0}};
    auto p = SelectSpread(two, 5);
    CHECK(p.size() == 5);
    CHECK(p[0].index == 0 && p[0].ring == 0 && p[1].index == 1 && p[1].ring == 0);
    CHECK(p[2].index == 0 && p[2].ring == 1 && p[3].index == 1 && p[3].ring == 1 && p[4].index == 0 && p[4].ring == 2);
    auto again = SelectSpread(two, 5);
    for (size_t i = 0; i < p.size(); ++i) CHECK(p[i].index == again[i].index && p[i].ring == again[i].ring);
    CHECK(SelectSpread({}, 3).empty() && SelectSpread(two, 0).empty());
}

int main() {
    TestLine(); TestDistinctAndSpread(); TestFewerCandidatesAndDeterminism();
    std::printf(g_fail ? "%d FAILED\n" : "all spawn tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

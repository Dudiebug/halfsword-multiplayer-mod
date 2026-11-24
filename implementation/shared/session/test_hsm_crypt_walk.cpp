// Unit tests for hsm_crypt_walk.h (walking the host's dungeon). Run via implementation/shared/run_tests.sh test_hsm_crypt_walk.
#include "session/hsm_crypt_walk.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::crypt;

static void TestBox() {
    WorldBox b; b.centre = {1000, 2000, 100}; b.hx = 300; b.hy = 100; b.hz = 150; b.yaw = 0;
    CHECK(InBox(b, {1000, 2000, 100}) && InBox(b, {1299, 2099, 249}));
    CHECK(!InBox(b, {1301, 2000, 100}) && !InBox(b, {1000, 2101, 100}) && !InBox(b, {1000, 2000, 251}));
    CHECK(InBox(b, {1301, 2000, 100}, 5));                                   // a margin
    b.yaw = 90;                                                              // the long side now runs along y
    CHECK(InBox(b, {1000, 2299, 100}) && !InBox(b, {1299, 2000, 100}));
    b.yaw = -90; CHECK(InBox(b, {1000, 1701, 100}) && !InBox(b, {1299, 2000, 100}));
    b.yaw = 45; CHECK(InBox(b, {1000 + 200, 2000 + 200, 100}) && !InBox(b, {1000 + 200, 2000 - 200, 100}));
    WorldBox none; CHECK(!InBox(none, {0, 0, 0}));                           // an unread box holds nothing
}

static void TestOnce() {
    OnceTracker t;
    CHECK(t.First(3, BoxKind::SpawnCharacters, 1) && !t.First(3, BoxKind::SpawnCharacters, 1));
    CHECK(t.First(3, BoxKind::SpawnLevel, 1) && t.First(3, BoxKind::SpawnCharacters, 2) && t.First(4, BoxKind::SpawnCharacters, 1));
    CHECK(t.Seen(3, BoxKind::SpawnLevel, 1) && !t.Seen(9, BoxKind::Box, 1) && t.Size() == 4);
    t.ForgetRoom(3); CHECK(t.Size() == 1 && t.First(3, BoxKind::SpawnLevel, 1));
    t.Clear(); CHECK(t.Size() == 0);
}

static void TestStandCheck() {
    auto flat = [](const Point3&) { return std::optional<double>(0.0); };
    auto clear = [](const Point3&, const Point3&) { return true; };
    CHECK(StandCheck(0.0, {0, 0, 0}, {300, 0, 0}, flat, clear) == StandRefusal::None);
    auto hole = [](const Point3& p) { return p.x > 320 ? std::optional<double>() : std::optional<double>(0.0); };   // no floor at the +x foot
    CHECK(StandCheck(0.0, {0, 0, 0}, {300, 0, 0}, hole, clear) == StandRefusal::NoFloor);
    auto ledge = [](const Point3& p) { return std::optional<double>(p.x > 300 ? 500.0 : 0.0); };
    CHECK(StandCheck(0.0, {0, 0, 0}, {300, 0, 0}, ledge, clear) == StandRefusal::FloorStep);
    auto wall = [](const Point3&, const Point3&) { return false; };
    CHECK(StandCheck(0.0, {0, 0, 0}, {300, 0, 0}, flat, wall) == StandRefusal::Wall);
    CHECK(StandCheck(std::nullopt, {0, 0, 0}, {300, 0, 0}, flat, clear) == StandRefusal::None);   // no centre floor: any floor
    for (StandRefusal r : {StandRefusal::None, StandRefusal::NoFloor, StandRefusal::FloorStep, StandRefusal::Wall, StandRefusal::OutOfArea})
        CHECK(StandRefusalName(r)[0] != '?');
}

static void TestPick() {
    const Point3 anchor{0, 0, 0};
    std::vector<Point3> spots{{1000, 0, 0}, {300, 0, 0}, {320, 0, 0}, {-600, 0, 0}, {0, 700, 0}};
    int refusedN = 0;
    auto ok = [](const Point3&) { return StandRefusal::None; };
    auto refused = [&](const Point3&, StandRefusal) { ++refusedN; };
    auto out = PickEntranceSpots(spots, anchor, 3, {anchor}, kMinFighterGapCm, ok, refused);
    CHECK(out.size() == 3 && out[0].x == 300 && out[1].x == -600 && out[2].y == 700);   // nearest first; 320 is too close to 300
    for (size_t i = 0; i < out.size(); ++i) for (size_t j = i + 1; j < out.size(); ++j) CHECK(FlatDist2(out[i], out[j]) >= kMinFighterGapCm * kMinFighterGapCm - 1);
    // a refused spot is skipped and reported
    auto no300 = [](const Point3& p) { return p.x == 300 ? StandRefusal::Wall : StandRefusal::None; };
    out = PickEntranceSpots(spots, anchor, 1, {anchor}, kMinFighterGapCm, no300, refused);
    CHECK(out.size() == 1 && out[0].x == 320 && refusedN == 1);
    // more players than spots: the rest spread round the anchor, nobody stacks
    out = PickEntranceSpots({{400, 0, 0}}, anchor, 5, {anchor}, kMinFighterGapCm, ok, refused);
    CHECK(out.size() == 5 && out[0].x == 400);
    for (size_t i = 0; i < out.size(); ++i) for (size_t j = i + 1; j < out.size(); ++j) CHECK(FlatDist2(out[i], out[j]) >= kMinFighterGapCm * kMinFighterGapCm - 1);
    // no spots at all: still spread
    out = PickEntranceSpots({}, anchor, 3, {anchor}, kMinFighterGapCm, ok, refused);
    CHECK(out.size() == 3 && FlatDist2(out[0], out[1]) > 1 && FlatDist2(out[1], out[2]) > 1 && FlatDist2(out[0], anchor) > 1);
}

int main() {
    TestBox(); TestOnce(); TestStandCheck(); TestPick();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypt_walk: ok\n");
    return 0;
}

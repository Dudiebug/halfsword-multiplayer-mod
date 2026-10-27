// role=server spawn anchor: pawn first, then the PlayerStart nearest the spawners, then a spawner.
#include "session/hsm_spawn_anchor.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    const std::vector<Point3> starts{{1000, 0, 0}, {10, 0, 0}, {-5, 0, 0}};
    const std::vector<Point3> spawners{{0, 0, 0}, {20, 0, 0}};
    auto a = ChooseAnchor(Point3{7, 8, 9}, starts, spawners);                       // the pawn wins over everything
    CHECK(a.kind == AnchorKind::StockPawn && a.pos.x == 7 && a.pos.z == 9);
    a = ChooseAnchor(std::nullopt, starts, spawners);                               // centroid (10,0,0): start index 1
    CHECK(a.kind == AnchorKind::PlayerStart && a.pos.x == 10);
    a = ChooseAnchor(std::nullopt, starts, {});                                     // no spawners: the first start
    CHECK(a.kind == AnchorKind::PlayerStart && a.pos.x == 1000);
    a = ChooseAnchor(std::nullopt, {}, spawners);                                   // tie between the two: lowest index
    CHECK(a.kind == AnchorKind::Spawner && a.pos.x == 0);
    a = ChooseAnchor(std::nullopt, {}, {{0, 0, 0}, {10, 0, 0}, {40, 0, 0}});        // centroid (16.7,0,0): nearest is 10
    CHECK(a.kind == AnchorKind::Spawner && a.pos.x == 10);
    a = ChooseAnchor(std::nullopt, {}, {});
    CHECK(a.kind == AnchorKind::None);
    a = ChooseAnchor(std::nullopt, {{5, 5, 5}, {5, 5, 5}}, {{5, 5, 5}});            // identical points are fine
    CHECK(a.kind == AnchorKind::PlayerStart && a.pos.y == 5);

    CHECK(StandOffsetFor(AnchorKind::StockPawn, 112) == 112);
    CHECK(StandOffsetFor(AnchorKind::StockPawn, -96) == kStandDefaultCm);           // a bad trace is still sanitised
    CHECK(StandOffsetFor(AnchorKind::StockPawn, 900) == kStandMaxCm);
    CHECK(StandOffsetFor(AnchorKind::PlayerStart, 150) == kStandDefaultCm);
    CHECK(StandOffsetFor(AnchorKind::Spawner, 150) == kStandDefaultCm);
    if (!g_fail) std::printf("test_hsm_spawn_anchor: ok\n");
    return g_fail ? 1 : 0;
}

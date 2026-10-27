// role=server spawn anchor: pawn, spawners, PlayerStart, navmesh, floor, origin, in that order; the timing of the passes.
#include "session/hsm_spawn_anchor.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

namespace {
struct Calls { int spawners = 0, starts = 0, nav = 0, floor = 0; };
// Sources that answer with the given points (empty/nullopt: nothing there), counting how often each was read.
AnchorSources Make(Calls& n, std::vector<Point3> sp, std::vector<Point3> ps, std::optional<Point3> nav, std::optional<Point3> fl) {
    AnchorSources s;
    s.spawners = [&n, sp] { ++n.spawners; return sp; };
    s.playerStarts = [&n, ps] { ++n.starts; return ps; };
    s.navmesh = [&n, nav] { ++n.nav; return nav; };
    s.floor = [&n, fl] { ++n.floor; return fl; };
    return s;
}
}  // namespace

int main() {
    const std::vector<Point3> starts{{1000, 0, 0}, {10, 0, 0}, {-5, 0, 0}};
    const std::vector<Point3> spawners{{0, 0, 0}, {20, 0, 0}};
    auto a = ChooseAnchor(Point3{7, 8, 9}, starts, spawners);                       // the pawn wins over everything
    CHECK(a.kind == AnchorKind::StockPawn && a.pos.x == 7 && a.pos.z == 9);
    a = ChooseAnchor(std::nullopt, starts, spawners);                               // a spawner beats a PlayerStart; tie: lowest index
    CHECK(a.kind == AnchorKind::Spawner && a.pos.x == 0);
    a = ChooseAnchor(std::nullopt, starts, {});                                     // no spawners: the first start
    CHECK(a.kind == AnchorKind::PlayerStart && a.pos.x == 1000);
    a = ChooseAnchor(std::nullopt, {}, {{0, 0, 0}, {10, 0, 0}, {40, 0, 0}});        // centroid (16.7,0,0): nearest is 10
    CHECK(a.kind == AnchorKind::Spawner && a.pos.x == 10);
    a = ChooseAnchor(std::nullopt, {}, {});
    CHECK(a.kind == AnchorKind::None);
    a = ChooseAnchor(std::nullopt, {{5, 5, 5}, {5, 5, 5}}, {{5, 5, 5}});            // identical points are fine
    CHECK(a.kind == AnchorKind::Spawner && a.pos.y == 5);

    CHECK(StandOffsetFor(AnchorKind::StockPawn, 112) == 112);
    CHECK(StandOffsetFor(AnchorKind::StockPawn, -96) == kStandDefaultCm);           // a bad trace is still sanitised
    CHECK(StandOffsetFor(AnchorKind::StockPawn, 900) == kStandMaxCm);
    for (AnchorKind k : {AnchorKind::PlayerStart, AnchorKind::Spawner, AnchorKind::Navmesh, AnchorKind::Floor, AnchorKind::Origin})
        CHECK(StandOffsetFor(k, 150) == kStandDefaultCm);

    // The source order, and that a later source is only read when the earlier ones were empty.
    Calls n;
    a = SearchAnchor(Make(n, spawners, starts, Point3{1, 1, 1}, Point3{2, 2, 2}), false);
    CHECK(a.kind == AnchorKind::Spawner && n.spawners == 1 && n.starts == 0 && n.nav == 0 && n.floor == 0);
    n = {};
    a = SearchAnchor(Make(n, {}, starts, Point3{1, 1, 1}, Point3{2, 2, 2}), false);
    CHECK(a.kind == AnchorKind::PlayerStart && a.pos.x == 1000 && n.starts == 1 && n.nav == 0 && n.floor == 0);
    n = {};
    a = SearchAnchor(Make(n, {}, {}, Point3{1, 1, 1}, Point3{2, 2, 2}), false);
    CHECK(a.kind == AnchorKind::Navmesh && a.pos.x == 1 && n.nav == 1 && n.floor == 0);
    n = {};
    a = SearchAnchor(Make(n, {}, {}, std::nullopt, Point3{2, 2, 2}), false);
    CHECK(a.kind == AnchorKind::Floor && a.pos.x == 2 && n.nav == 1 && n.floor == 1);
    n = {};
    a = SearchAnchor(Make(n, {}, {}, std::nullopt, std::nullopt), false);           // nothing yet: try again next pass
    CHECK(a.kind == AnchorKind::None);
    a = SearchAnchor(Make(n, {}, {}, std::nullopt, std::nullopt), true);            // the wait is over: the origin
    CHECK(a.kind == AnchorKind::Origin && a.pos.x == 0 && a.pos.y == 0 && a.pos.z == 0);
    a = SearchAnchor(Make(n, spawners, {}, std::nullopt, std::nullopt), true);      // giving up never overrides a real source
    CHECK(a.kind == AnchorKind::Spawner);
    a = SearchAnchor(AnchorSources{}, false);                                       // missing sources are skipped
    CHECK(a.kind == AnchorKind::None);
    a = SearchAnchor(AnchorSources{}, true);
    CHECK(a.kind == AnchorKind::Origin);

    // Timing: nothing before 3 s, the first pass at 3 s, then one a second; the origin from 20 s on.
    AnchorSearch t;
    t.OnLevel(10000);
    CHECK(!t.Due(10000) && !t.Due(12999) && t.Due(13000));
    CHECK(!t.GiveUp(13000) && !t.GiveUp(29999) && t.GiveUp(30000));
    t.Passed(13000);
    CHECK(!t.Due(13000) && !t.Due(13999) && t.Due(14000) && t.passes() == 1);
    t.Passed(14000);
    CHECK(!t.Due(14500) && t.Due(15000) && t.passes() == 2);
    t.OnLevel(50000);                                                               // a new level starts over
    CHECK(!t.Due(50000) && t.Due(53000) && t.passes() == 0 && !t.GiveUp(50000));
    t.OnLevel(0xFFFFFF00u);                                                         // the millisecond clock wrapping mid-wait
    CHECK(!t.Due(0xFFFFFF00u + 100) && t.Due(0xFFFFFF00u + 3000) && t.GiveUp(0xFFFFFF00u + 20000));
    if (!g_fail) std::printf("test_hsm_spawn_anchor: ok\n");
    return g_fail ? 1 : 0;
}

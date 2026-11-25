// Unit tests for hsm_crypt_spawn.h (the host spawns the shared Crypts' enemies for every player).
// Run via implementation/shared/run_tests.sh test_hsm_crypt_spawn.
#include "session/hsm_crypt_spawn.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::crypt;

// The game's rule, as the room's own event has it: only player 0 (the host) counts.
static int GameRuleSpawns(const std::vector<Point3>& pts, const Point3& host, bool hostSeesAll) {
    int n = 0;
    for (const Point3& p : pts) if (!(Dist(p, host) <= kSpawnClearCm || hostSeesAll)) ++n;
    return n;
}

// The 0.8.0 host log: the client walks into a room, the host 2.5 m behind it in the doorway sees the whole room. The game's rule
// spawns nothing; the plan, which asks every player, also refuses at first (both stand there), then spawns once the
// tries run out, at the point farthest from both.
static void TestLoggedCase() {
    const std::vector<Point3> pts = {{1000, 0, 0}, {1400, 300, 0}, {600, -200, 0}};
    const Point3 host{-250, 0, 0}, client{0, 0, 0};
    CHECK(GameRuleSpawns(pts, host, true) == 0);
    auto seeAll = [](size_t, size_t) { return true; };
    const std::vector<bool> passed(3, true);
    RoomSpawn r; r.started = true;
    SpawnPlan p;
    SpawnStep s = SpawnStep::Retry;
    uint32_t now = 0;
    int tries = 0;
    while (r.Due(now)) { p = PlanSpawns(pts, passed, {host, client}, 0, seeAll); s = NextStep(r, p, 0, now); ++tries; now += kSpawnRetryMs; }
    CHECK(tries == kSpawnTries && s == SpawnStep::Fallback && p.use.empty() && p.seen == 3);
    CHECK(FallbackPoint(pts, p, {host, client}) == std::optional<size_t>(1));   // (1400, 300) is farthest from both
}

// The client alone in a far room, the host out of sight: the host's position no longer decides, every player's does.
static void TestEveryPlayerCounts() {
    const std::vector<Point3> pts = {{5000, 0, 0}, {5200, 400, 0}, {5600, 0, 0}};
    const std::vector<bool> passed(3, true);
    auto none = [](size_t, size_t) { return false; };
    SpawnPlan p = PlanSpawns(pts, passed, {{0, 0, 0}, {5050, 0, 0}}, 0, none);   // the client is within 5 m of the first two
    CHECK(p.use.size() == 1 && p.use[0] == 2 && p.nearby == 2 && p.refused.size() == 2);
    p = PlanSpawns(pts, passed, {{0, 0, 0}}, 0, none);                           // the host alone, far off: all three
    CHECK(p.use.size() == 3 && p.refused.empty());
    auto clientSees = [](size_t i, size_t j) { return j == 1 && i == 2; };         // the client (player 1) sees point 2
    p = PlanSpawns(pts, passed, {{0, 0, 0}, {9000, 0, 0}}, 0, clientSees);
    CHECK(p.use.size() == 2 && p.seen == 1 && p.refused.size() == 1 && p.refused[0] == 2);
    CHECK(Dist({0, 0, 0}, {300, 400, 0}) == 500);
    p = PlanSpawns({{300, 400, 0}}, {true}, {{0, 0, 0}}, 0, none);                 // exactly 5 m: near, as the game's <= test
    CHECK(p.use.empty() && p.nearby == 1);
}

static void TestRollAndCap() {
    const std::vector<Point3> pts = {{5000, 0, 0}, {5200, 0, 0}, {5400, 0, 0}, {5600, 0, 0}};
    auto none = [](size_t, size_t) { return false; };
    SpawnPlan p = PlanSpawns(pts, {true, false, true, false}, {{0, 0, 0}}, 0, none);
    CHECK(p.use.size() == 2 && p.use[1] == 2 && p.chance == 2);
    p = PlanSpawns(pts, {true, true}, {{0, 0, 0}}, 0, none);                       // a point without a roll is not used
    CHECK(p.use.size() == 2 && p.chance == 2);
    p = PlanSpawns(pts, std::vector<bool>(4, true), {{0, 0, 0}}, 3, none);          // 3 alive: room for 2
    CHECK(p.use.size() == 2 && p.capped == 2);
    p = PlanSpawns(pts, std::vector<bool>(4, true), {{0, 0, 0}}, 5, none);
    CHECK(p.use.empty() && p.capped == 4);
    p = PlanSpawns(pts, std::vector<bool>(4, true), {{0, 0, 0}}, 7, none);          // over the cap (the game's own spawns)
    CHECK(p.use.empty() && p.capped == 4);
}

static void TestNextStep() {
    RoomSpawn r;
    CHECK(!r.Due(0));                              // not started: never due
    r.started = true;
    CHECK(r.Due(0));
    SpawnPlan used; used.use = {0};
    CHECK(NextStep(r, used, 1, 0) == SpawnStep::Done && r.done && !r.Due(100000));
    r = RoomSpawn{}; r.started = true;             // planned, but the director spawned nothing (its own cap check, a failed call)
    CHECK(NextStep(r, used, 0, 0) == SpawnStep::Retry && !r.done);
    for (int i = 2; i < kSpawnTries; ++i) CHECK(NextStep(r, used, 0, 0) == SpawnStep::Retry);
    CHECK(NextStep(r, used, 0, 0) == SpawnStep::Fallback && r.done);
    r = RoomSpawn{}; r.started = true;
    SpawnPlan rolledOut; rolledOut.chance = 3;     // every roll failed: an empty room, as in the game; not retried
    CHECK(NextStep(r, rolledOut, 0, 0) == SpawnStep::Done && r.tries == 1);
    r = RoomSpawn{}; r.started = true;
    SpawnPlan full; full.capped = 2;               // only the cap in the way: retried, then given up (no fallback over the cap)
    SpawnStep s = SpawnStep::Done;
    uint32_t now = 1000;
    for (int i = 0; i < kSpawnTries; ++i) {
        CHECK(r.Due(now));
        s = NextStep(r, full, 0, now);
        if (s == SpawnStep::Retry) { CHECK(r.next == now + kSpawnRetryMs && !r.Due(now + kSpawnRetryMs - 1)); now = r.next; }
    }
    CHECK(s == SpawnStep::GiveUp && r.done && r.tries == kSpawnTries);
    r = RoomSpawn{}; r.started = true;
    SpawnPlan seen; seen.refused = {1}; seen.seen = 1;
    for (int i = 1; i < kSpawnTries; ++i) CHECK(NextStep(r, seen, 0, 0) == SpawnStep::Retry);
    CHECK(NextStep(r, seen, 0, 0) == SpawnStep::Fallback && r.done);
    CHECK(r.Due(0xFFFFFFF0u) == false);
    RoomSpawn wrap; wrap.started = true; wrap.next = 0xFFFFFF00u;   // the clock wraps
    CHECK(!wrap.Due(0xFFFFF000u) && wrap.Due(0x10u));
}

static void TestFallback() {
    SpawnPlan p;
    CHECK(!FallbackPoint({{0, 0, 0}}, p, {{0, 0, 0}}));           // nothing refused: no fallback
    p.refused = {0, 2, 7};                                          // 7 is out of range: skipped
    const std::vector<Point3> pts = {{100, 0, 0}, {0, 0, 0}, {-900, 0, 0}};
    // Both refused points are 100 cm from their nearest player: the first one stays (a tie does not move it).
    CHECK(FallbackPoint(pts, p, {{0, 0, 0}, {-1000, 0, 0}}) == std::optional<size_t>(0));
    CHECK(FallbackPoint(pts, p, {{0, 0, 0}, {-1050, 0, 0}}) == std::optional<size_t>(2));   // -900 is now 150 from the second
    CHECK(FallbackPoint(pts, p, {{2000, 0, 0}}) == std::optional<size_t>(2));   // one player far east: the west point
    CHECK(FallbackPoint(pts, p, {}) == std::optional<size_t>(0));               // nobody: the first refused one
}

static void TestTeam() {
    auto first = [](int) { return 0; };
    auto last = [](int n) { return n - 1; };
    CHECK(RoomTeam(0, false, first) == 0 && RoomTeam(0, false, last) == 3);
    CHECK(RoomTeam(1, false, first) == 0 && RoomTeam(2, false, last) == 3);
    CHECK(RoomTeam(3, false, last) == 4 && RoomTeam(4, false, [](int) { return 2; }) == 2);
    CHECK(RoomTeam(5, false, first) == 3 && RoomTeam(5, false, last) == 4 && RoomTeam(5, false, [](int) { return 2; }) == 5);
    CHECK(RoomTeam(6, false, first) == 2 && RoomTeam(8, false, last) == 4 && RoomTeam(7, false, [](int) { return 1; }) == 5);
    CHECK(RoomTeam(9, false, first) == 0 && RoomTeam(-1, false, first) == 0);
    CHECK(RoomTeam(0, true, first) == 2 && RoomTeam(2, true, first) == 2 && RoomTeam(3, true, first) == 4 && RoomTeam(5, true, first) == 4);
    CHECK(RoomTeam(6, true, first) == 5 && RoomTeam(8, true, first) == 5 && RoomTeam(9, true, first) == 0);
    CHECK(RoomTeam(3, false, [](int) { return 99; }) == 0);   // a bad pick falls back to the first entry
    int asked = 0;
    RoomTeam(5, false, [&](int n) { asked = n; return 0; });
    CHECK(asked == 4);
}

static void TestInFront() {
    CHECK(InFront({0, 0, 0}, 0, {10, 0, 0}) && !InFront({0, 0, 0}, 0, {-10, 0, 0}) && !InFront({0, 0, 0}, 0, {0, 50, 0}));
    CHECK(InFront({100, 100, 0}, 90, {100, 200, 0}) && !InFront({100, 100, 0}, 90, {100, 0, 0}));
    CHECK(InFront({0, 0, 0}, 180, {-10, 0, 0}) && InFront({0, 0, 0}, -90, {0, -10, 0}));
}

int main() {
    TestLoggedCase();
    TestEveryPlayerCounts();
    TestRollAndCap();
    TestNextStep();
    TestFallback();
    TestTeam();
    TestInFront();
    if (g_fail) { std::printf("test_hsm_crypt_spawn: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypt_spawn: OK\n");
    return 0;
}

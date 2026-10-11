// Spectate target cycling: empty and lone rosters, auto-advance when the target dies or leaves, team preference,
// free-for-all grouping, and wrap-around in both directions.
#include "match/hsm_spectate.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Is(std::optional<uint8_t> v, int entity) { return v && int(*v) == entity; }

static void TestEmpty() {
    SpectateCycler c;
    CHECK(!c.Target());
    c.Refresh(1, kNoTeam, {});
    CHECK(!c.Target() && !c.Next() && !c.Prev());
}

static void TestOnlyMe() {
    SpectateCycler c;
    c.Refresh(1, 0, {{1, 0, true}});                            // only the spectator, alive
    CHECK(!c.Target() && !c.Next() && !c.Prev());
    c.Refresh(1, 0, {{1, 0, true}, {2, 1, false}});             // the other fighter is dead
    CHECK(!c.Target() && !c.Next());
}

static void TestTargetDies() {
    SpectateCycler c;
    c.Refresh(1, kNoTeam, {{2, kNoTeam, true}, {3, kNoTeam, true}, {4, kNoTeam, true}});
    CHECK(Is(c.Target(), 2));
    c.Refresh(1, kNoTeam, {{2, kNoTeam, false}, {3, kNoTeam, true}, {4, kNoTeam, true}});   // 2 dies
    CHECK(Is(c.Target(), 3));
    c.Refresh(1, kNoTeam, {{2, kNoTeam, false}, {3, kNoTeam, false}, {4, kNoTeam, true}});  // 3 dies
    CHECK(Is(c.Target(), 4));
    c.Refresh(1, kNoTeam, {{2, kNoTeam, true}, {3, kNoTeam, false}});                       // 4 leaves; 2 is back up front
    CHECK(Is(c.Target(), 2));
    c.Refresh(1, kNoTeam, {{2, kNoTeam, false}});                                           // nobody alive
    CHECK(!c.Target() && !c.Next());
    c.Refresh(1, kNoTeam, {{2, kNoTeam, true}});                                            // someone respawns
    CHECK(Is(c.Target(), 2));
}

static void TestTargetStaysWhileAlive() {
    SpectateCycler c;
    c.Refresh(1, kNoTeam, {{2, kNoTeam, true}, {3, kNoTeam, true}, {4, kNoTeam, true}});
    CHECK(Is(c.Next(), 3));
    c.Refresh(1, kNoTeam, {{2, kNoTeam, true}, {3, kNoTeam, true}, {4, kNoTeam, true}, {6, kNoTeam, true}});   // joins
    CHECK(Is(c.Target(), 3));
    CHECK(Is(c.Next(), 4));
    CHECK(Is(c.Next(), 6));
}

static void TestTeamPreference() {
    SpectateCycler c;
    // Spectator on team 0. Living teammate 7 wins over lower ids; dead teammate 5 is skipped; team-less 9 counts as other.
    c.Refresh(1, 0, {{2, 1, true}, {7, 0, true}, {9, kNoTeam, true}, {5, 0, false}});
    CHECK(Is(c.Target(), 7));
    CHECK(Is(c.Next(), 2));                 // other living fighters in id order
    CHECK(Is(c.Next(), 9));
    CHECK(Is(c.Next(), 7));                 // wraps back to the teammate
    CHECK(Is(c.Prev(), 9));

    // Teammates die one by one: the camera stays on teammates before it moves to the other team.
    SpectateCycler t;
    t.Refresh(1, 0, {{3, 0, true}, {7, 0, true}, {2, 1, true}});
    CHECK(Is(t.Target(), 3));
    t.Refresh(1, 0, {{7, 0, true}, {2, 1, true}});           // 3 dies
    CHECK(Is(t.Target(), 7));
    t.Refresh(1, 0, {{2, 1, true}});                         // 7 dies: no teammates left
    CHECK(Is(t.Target(), 2));

    // Team-less spectator: nobody is a teammate, even fighters that also have no team.
    SpectateCycler f;
    f.Refresh(5, kNoTeam, {{5, kNoTeam, true}, {3, 1, true}, {4, kNoTeam, true}});
    CHECK(Is(f.Target(), 3));
    CHECK(Is(f.Next(), 4));
}

static void TestWrap() {
    SpectateCycler c;
    c.Refresh(1, kNoTeam, {{2, kNoTeam, true}, {3, kNoTeam, false}, {4, kNoTeam, true}});   // 3 is dead and skipped
    CHECK(Is(c.Target(), 2));
    CHECK(Is(c.Prev(), 4));                 // backward wraps to the last
    CHECK(Is(c.Next(), 2));                 // forward wraps to the first
    CHECK(Is(c.Next(), 4));
    CHECK(Is(c.Next(), 2));

    SpectateCycler one;
    one.Refresh(1, kNoTeam, {{2, kNoTeam, true}});
    CHECK(Is(one.Next(), 2) && Is(one.Prev(), 2));
}

int main() {
    TestEmpty();
    TestOnlyMe();
    TestTargetDies();
    TestTargetStaysWhileAlive();
    TestTeamPreference();
    TestWrap();
    std::printf(g_fail ? "%d FAILED\n" : "all spectate tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

// Unit tests for hsm_lockon.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_lockon.cpp
#include "hsm_lockon.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::lockon;

static bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void TestActive() {
    CHECK(Active(true, true, false, 0.5));
    CHECK(!Active(false, true, false, 0.5));    // camera lock off
    CHECK(!Active(true, false, false, 0.5));    // not guarding
    CHECK(!Active(true, true, true, 0.5));      // fallen
    CHECK(!Active(true, true, false, 0.0));     // strength 0 (the setting)
}

static void TestForward() {
    V3 f = Forward({0, 0});   CHECK(Near(f.x, 1) && Near(f.y, 0) && Near(f.z, 0));
    f = Forward({90, 0});     CHECK(Near(f.x, 0) && Near(f.y, 1));
    f = Forward({0, 90});     CHECK(Near(f.z, 1));
}

static void TestPick() {
    const V3 body{0, 0, 100}, eye{0, 0, 160};
    const V3 fwd = Forward({0, 0});
    std::vector<Candidate> c;
    CHECK(Pick(body, eye, fwd, c) == 0);                                   // nobody
    c.push_back({2, {350, 0, 100}});
    CHECK(Pick(body, eye, fwd, c) == 2);                                   // dead ahead, inside the sweep
    c = {{2, {100, 0, 100}}};
    CHECK(Pick(body, eye, fwd, c) == 2);                                   // close ahead still touches the sweep (it starts 200 ahead, radius 200 + slack)
    c = {{2, {-100, 0, 100}}};
    CHECK(Pick(body, eye, fwd, c) == 0);                                   // behind: out
    c = {{2, {900, 0, 100}}};
    CHECK(Pick(body, eye, fwd, c) == 0);                                   // too far
    c = {{2, {350, 600, 100}}};
    CHECK(Pick(body, eye, fwd, c) == 0);                                   // off to the side, out of the sweep
    c = {{2, {350, 150, 100}}, {3, {350, -40, 100}}, {4, {350, 200, 100}}};
    CHECK(Pick(body, eye, fwd, c) == 3);                                   // the one the camera looks at most straight
    c = {{5, {350, 0, 100}}, {4, {350, 0, 100}}};
    CHECK(Pick(body, eye, fwd, c) == 4);                                   // a tie: the lower entity, the same on every run
    c = {{0, {350, 0, 100}}};
    CHECK(Pick(body, eye, fwd, c) == 0);                                   // entity 0 is no fighter
    c = {{2, {0, 350, 100}}};
    CHECK(Pick(body, eye, Forward({90, 0}), c) == 2);                      // facing +Y
}

static void TestLookAt() {
    Rot r = LookAt({0, 0, 0}, {10, 0, 0});     CHECK(Near(r.yaw, 0) && Near(r.pitch, 0));
    r = LookAt({0, 0, 0}, {0, 10, 0});         CHECK(Near(r.yaw, 90));
    r = LookAt({0, 0, 0}, {-10, 0, 0});        CHECK(Near(std::fabs(r.yaw), 180));
    r = LookAt({0, 0, 0}, {10, 0, 10});        CHECK(Near(r.pitch, 45));
}

static void TestSlew() {
    const V3 head{0, 0, 160}, at{300, 300, 160};                           // 45 degrees to the left of +X
    Rot cur{0, 0};
    for (int i = 0; i < 600; ++i) cur = Slew(cur, head, at, 1.0 / 60.0, 0.5);
    CHECK(Near(cur.yaw, 45, 0.01) && Near(cur.pitch, 0, 0.01));            // it gets there
    Rot one = Slew({0, 0}, head, at, 1.0 / 60.0, 0.5);
    CHECK(one.yaw > 0 && one.yaw < 45);                                    // and not in one frame
    Rot strong = Slew({0, 0}, head, at, 1.0 / 60.0, 1.0);
    CHECK(strong.yaw > one.yaw);                                           // a higher Lock On Strength is quicker
    Rot tight = Slew({0, 0}, head, {20, 20, 160}, 1.0 / 60.0, 0.5);
    CHECK(tight.yaw < one.yaw);                                             // close by it turns slower (the ramp 0.1..4)
    Rot wrap = Slew({179, 0}, {0, 0, 0}, {-300, -10, 0}, 1.0 / 60.0, 0.5);
    CHECK(wrap.yaw > 179 || wrap.yaw < -170 || Near(std::fabs(wrap.yaw), 180, 1.0));   // the short way round, not back across zero
    CHECK(Near(Slew({10, 5}, head, at, 0.0, 0.5).yaw, 10));                // no time, no turn
}

static void TestTracker() {
    Tracker t;
    CHECK(!t.Due(false, 16));                                              // lock off: nothing
    CHECK(t.Due(true, 16));                                                // switched on: a pulse at once
    t.Found(3); CHECK(t.target() == 3);
    CHECK(!t.Due(true, 50) && t.target() == 3);                            // the target holds between pulses
    CHECK(!t.Due(true, 40));
    CHECK(t.Due(true, 16));                                                // 106 ms since the last: pulse
    CHECK(!t.Due(false, 16) && t.target() == 0);                           // lock off drops the target
    CHECK(t.Due(true, 1));                                                 // and the next switch-on pulses at once again
    t.Found(4); t.Drop(); CHECK(t.target() == 0);
}

int main() {
    TestActive(); TestForward(); TestPick(); TestLookAt(); TestSlew(); TestTracker();
    std::printf(g_fail ? "%d FAILED\n" : "all lockon tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

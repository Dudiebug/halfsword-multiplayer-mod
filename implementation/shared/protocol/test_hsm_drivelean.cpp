// Unit tests for hsm_drivelean.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_drivelean.cpp
#include "hsm_drivelean.h"
#include "hsm_input.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static constexpr size_t RG = size_t(Axis::RightGuard), LG = size_t(Axis::LeftGuard), MF = size_t(Axis::MoveForward);
static constexpr size_t RA = size_t(Axis::RightArm), TM = size_t(Axis::TurnMouse);

static void TestGuardHeld() {
    int w1 = 1, w2 = 2;
    LeanFeed f; f.Target(&w1);
    std::array<float, kAxisCount> ax{}; std::array<bool, kAxisCount> skip{};
    ax[RG] = 1.f;
    f.Trim(ax, skip); CHECK(!skip[RG]);               // the press goes to the game
    ax[RG] = 1.f; skip = {};
    f.Trim(ax, skip); CHECK(skip[RG]);                // held: the DoOnce returns at once, not fed
    ax[RG] = 0.8f; skip = {};
    f.Trim(ax, skip); CHECK(skip[RG]);                // a trigger's analogue wobble above 0.5: the same
    ax[RG] = 0.2f; skip = {};
    f.Trim(ax, skip); CHECK(!skip[RG]);               // across 0.5: the release branch must run
    ax[RG] = 0.f; skip = {};
    f.Trim(ax, skip); CHECK(skip[RG]);                // below again: gate closed
    ax[RG] = 1.f; skip = {};
    f.Trim(ax, skip); CHECK(!skip[RG]);               // pressed again: fed
    f.Target(&w2);                                           // a new fighter: all handlers unrun
    skip = {};
    CHECK(f.Trim(ax, skip) == 0 && !skip[RG]);
}

static void TestOthersUntouched() {
    int w = 0;
    LeanFeed f; f.Target(&w);
    std::array<float, kAxisCount> ax{}; std::array<bool, kAxisCount> skip{};
    ax[MF] = 1.f; ax[RA] = 1.f; ax[TM] = 3.f; ax[LG] = 1.f;
    for (int i = 0; i < 3; ++i) {
        skip = {};
        f.Trim(ax, skip);
        CHECK(!skip[MF] && !skip[RA] && !skip[TM]);          // movement, arms and look are fed as before
    }
    CHECK(skip[LG]);                                         // the left guard went quiet after its first feed
}

static void TestQuietThenPress() {
    int w = 0;
    LeanFeed f; f.Target(&w);
    std::array<float, kAxisCount> ax{}; std::array<bool, kAxisCount> skip{};
    skip[LG] = true;                                         // quiet: the game holds 0
    CHECK(f.Trim(ax, skip) == 0 && skip[LG]);
    ax[LG] = 1.f; skip = {};
    f.Trim(ax, skip); CHECK(!skip[LG]);               // up from quiet: fed
    // with the real applier: held guard over many frames, fed once
    InputApplier a; a.Target(&w);
    InputFrame fr; fr.seq = 1; fr.axes[RG] = 1.f;
    CHECK(a.Receive(fr));
    LeanFeed g; g.Target(&w);
    unsigned fed = 0, skipped = 0;
    for (int i = 0; i < 100; ++i) {
        InputApplier::Step s = a.Tick(16.f);
        g.Trim(s.axes, s.skip);
        if (s.skip[RG]) ++skipped; else ++fed;
    }
    CHECK(fed == 1 && skipped == 99);
}

static void TestRotationGate() {
    ControlRotationGate g;
    int c1 = 1, c2 = 2;
    CHECK(g.Needed(&c1, 90.f, 0.f, 1000));                   // first
    CHECK(!g.Needed(&c1, 90.f, 0.f, 1016));                  // unchanged
    CHECK(g.Needed(&c1, 91.f, 0.f, 1032));                   // yaw moved
    CHECK(g.Needed(&c1, 91.f, 1.f, 1048));                   // pitch moved
    CHECK(g.Needed(&c2, 91.f, 1.f, 1064));                   // another controller
    CHECK(!g.Needed(&c2, 91.f, 1.f, 1500));
    CHECK(g.Needed(&c2, 91.f, 1.f, 1564));                   // heartbeat after 500 ms
    CHECK(!g.Needed(&c2, 91.f, 1.f, 1600));
    CHECK(g.Needed(&c2, 91.f, 1.f, 1000));                   // the clock went back: set again
}

int main() {
    TestGuardHeld(); TestOthersUntouched(); TestQuietThenPress(); TestRotationGate();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

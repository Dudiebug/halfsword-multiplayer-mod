// Unit tests for hsm_draw.h. Run via implementation/shared/run_tests.sh test_hsm_draw.
#include "match/hsm_draw.h"
#include <cstdio>
#include <functional>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static DownSample Standing(uint8_t e) { DownSample s; s.entity = e; s.pelvisAboveFeetCm = 95; s.handR = {1000, 0, 120}; s.handL = {1000, 30, 120}; return s; }
static DownSample Lying(uint8_t e) { DownSample s = Standing(e); s.pelvisAboveFeetCm = 15; s.handR.z = 20; s.handL.z = 20; return s; }

// Samples every 250 ms from `from` to `to` (exclusive); the first tick with a reason returns it, with its time.
static DrawReason Run(DrawCheck& d, uint32_t from, uint32_t to, const std::function<std::vector<DownSample>(uint32_t)>& at,
                      uint32_t* when = nullptr) {
    for (uint32_t t = from; t < to; t += 250) {
        DrawReason r = d.Update(at(t), t);
        if (r != DrawReason::None) { if (when) *when = t; return r; }
    }
    return DrawReason::None;
}

// Test 2026-10-25: both fighters on the ground, unable to act: drawn kDownMs + kDrawMs after they are both down.
static void TestBothDownDraws() {
    DrawCheck d; d.Reset(0);
    uint32_t when = 0;
    CHECK(Run(d, 0, 60000, [](uint32_t) { return std::vector<DownSample>{Lying(1), Lying(2)}; }, &when) == DrawReason::BothDown);
    CHECK(when == DrawCheck::kDownMs + DrawCheck::kDrawMs);
    CHECK(d.Incapacitated(1, when) && d.Incapacitated(2, when));
    CHECK(d.Update({Lying(1), Lying(2)}, when + 250) == DrawReason::None);   // once
    d.Reset(when + 500);
    CHECK(!d.Down(1) && d.Update({Lying(1), Lying(2)}, when + 750) == DrawReason::None);   // a new round starts afresh
    uint32_t again = 0;
    CHECK(Run(d, when + 1000, when + 60000, [](uint32_t) { return std::vector<DownSample>{Lying(1), Lying(2)}; }, &again) == DrawReason::BothDown);
    CHECK(again == when + 750 + DrawCheck::kDownMs + DrawCheck::kDrawMs);           // and can be drawn again
    CHECK(DrawCheck::kDownMs == 8000 && DrawCheck::kDrawMs == 12000);
}

// One fighter down, the other standing over it: never a draw (the standing one may finish it).
static void TestOneDownNever() {
    DrawCheck d; d.Reset(0);
    CHECK(Run(d, 0, 80000, [](uint32_t) { return std::vector<DownSample>{Lying(1), Standing(2)}; }) == DrawReason::None);
    CHECK(d.Incapacitated(1, 79750) && !d.Down(2));
    DrawCheck alone; alone.Reset(0);                                    // the other one dead: the round is the match's business
    CHECK(Run(alone, 0, 80000, [](uint32_t) { return std::vector<DownSample>{Lying(1)}; }) == DrawReason::None);
}

// A fighter fighting from the ground (a hand moving a metre) or landing a hit is not incapacitated.
static void TestActingFromTheGround() {
    DrawCheck d; d.Reset(0);
    auto waving = [](uint32_t t) {
        DownSample a = Lying(1);
        if ((t / 5000) % 2) a.handR.x += DrawCheck::kHandMoveCm + 1;   // swings every 5 s
        return std::vector<DownSample>{a, Lying(2)};
    };
    CHECK(Run(d, 0, 80000, waving) == DrawReason::None);
    DrawCheck small; small.Reset(0);
    auto twitch = [](uint32_t t) {
        DownSample a = Lying(1);
        if ((t / 1000) % 2) a.handL.y += DrawCheck::kHandMoveCm - 1;   // within a metre of where it was
        return std::vector<DownSample>{a, Lying(2)};
    };
    uint32_t when = 0;
    CHECK(Run(small, 0, 80000, twitch, &when) == DrawReason::BothDown && when == 20000);

    DrawCheck moved; moved.Reset(0);
    auto once = [](uint32_t t) {
        DownSample a = Lying(1);
        if (t >= 5000) a.handR.y += DrawCheck::kHandMoveCm + 1;          // one reach at 5 s, then still again
        return std::vector<DownSample>{a, Lying(2)};
    };
    CHECK(Run(moved, 0, 80000, once, &when) == DrawReason::BothDown && when == 5000 + DrawCheck::kDownMs + DrawCheck::kDrawMs);
    DrawCheck left; left.Reset(0);
    auto leftReach = [](uint32_t t) {
        DownSample a = Lying(1);
        if (t >= 6000) a.handL.z += DrawCheck::kHandMoveCm + 1;          // the left hand counts as much as the right
        return std::vector<DownSample>{a, Lying(2)};
    };
    CHECK(Run(left, 0, 80000, leftReach, &when) == DrawReason::BothDown && when == 6000 + DrawCheck::kDownMs + DrawCheck::kDrawMs);

    DrawCheck hit; hit.Reset(0);
    auto both = [](uint32_t) { return std::vector<DownSample>{Lying(1), Lying(2)}; };
    CHECK(Run(hit, 0, 15000, both) == DrawReason::None);
    hit.OnHit(2, 15000);
    CHECK(!hit.Incapacitated(2, 15000) && hit.Incapacitated(1, 15000));
    CHECK(Run(hit, 15000, 80000, both, &when) == DrawReason::BothDown && when == 15000 + DrawCheck::kDownMs + DrawCheck::kDrawMs);
}

// A stagger up for less than kUpGraceMs keeps the fighter down; standing for longer gets it up.
static void TestHysteresis() {
    DrawCheck d; d.Reset(0);
    auto flicker = [](uint32_t t) {
        DownSample a = (t % 4000) < 750 ? Standing(1) : Lying(1);       // up 750 ms in every 4 s
        return std::vector<DownSample>{a, Lying(2)};
    };
    uint32_t when = 0;
    CHECK(Run(d, 250, 80000, flicker, &when) == DrawReason::BothDown && when == 750 + DrawCheck::kDownMs + DrawCheck::kDrawMs);   // down from the first lie
    DrawCheck up; up.Reset(0);
    auto rises = [](uint32_t t) {
        DownSample a = (t % 6000) < 1500 ? Standing(1) : Lying(1);      // up 1.5 s in every 6 s: never down 8 s
        return std::vector<DownSample>{a, Lying(2)};
    };
    CHECK(Run(up, 0, 80000, rises) == DrawReason::None);
    CHECK(DrawCheck::kUpGraceMs == 1000);
}

// The game's Fallen and a knock-out count as down whatever the pelvis height; the pelvis threshold is strict.
static void TestDownSigns() {
    DrawCheck d; d.Reset(0);
    auto flags = [](uint32_t) {
        DownSample a = Standing(1); a.fallen = true;
        DownSample b = Standing(2); b.knockedOut = true;
        return std::vector<DownSample>{a, b};
    };
    CHECK(Run(d, 0, 30000, flags) == DrawReason::BothDown);
    DrawCheck edge; edge.Reset(0);
    auto atThreshold = [](uint32_t) {
        DownSample a = Lying(1); a.pelvisAboveFeetCm = DrawCheck::kLowPelvisCm;   // exactly 70: not low
        return std::vector<DownSample>{a, Lying(2)};
    };
    CHECK(Run(edge, 0, 60000, atThreshold) == DrawReason::None && !edge.Down(1));
    DrawCheck below; below.Reset(0);
    auto under = [](uint32_t) {
        DownSample a = Lying(1); a.pelvisAboveFeetCm = DrawCheck::kLowPelvisCm - 0.5;   // kneeling
        return std::vector<DownSample>{a, Lying(2)};
    };
    CHECK(Run(below, 0, 60000, under) == DrawReason::BothDown);
}

// The hard cap: no hit for kStallMs with every live fighter below kStallHealth.
static void TestStall() {
    auto hurt = [](float h1, float h2) {
        return [=](uint32_t) { DownSample a = Standing(1), b = Standing(2); a.health = h1; b.health = h2; return std::vector<DownSample>{a, b}; };
    };
    DrawCheck d; d.Reset(1000);
    uint32_t when = 0;
    CHECK(Run(d, 1000, 200000, hurt(30, 39.5f), &when) == DrawReason::Stalled && when == 1000 + DrawCheck::kStallMs);
    DrawCheck hit; hit.Reset(0);
    CHECK(Run(hit, 0, 60000, hurt(30, 30)) == DrawReason::None);
    hit.OnHit(1, 60000);
    CHECK(Run(hit, 60000, 300000, hurt(30, 30), &when) == DrawReason::Stalled && when == 60000 + DrawCheck::kStallMs);
    DrawCheck healthy; healthy.Reset(0);
    CHECK(Run(healthy, 0, 300000, hurt(30, DrawCheck::kStallHealth)) == DrawReason::None);   // one at 40: not stalled
    DrawCheck one; one.Reset(0);
    CHECK(Run(one, 0, 300000, [](uint32_t) { DownSample a = Standing(1); a.health = 5; return std::vector<DownSample>{a}; }) == DrawReason::None);
    DrawCheck fresh;                                                    // never Reset: the clock starts at the first Update
    CHECK(Run(fresh, 500000, 500000 + DrawCheck::kStallMs, hurt(10, 10)) == DrawReason::None);
}

// A fighter that leaves (dies, disconnects) is forgotten: coming back it starts afresh.
static void TestForgetting() {
    DrawCheck d; d.Reset(0);
    CHECK(Run(d, 0, 10000, [](uint32_t) { return std::vector<DownSample>{Lying(1), Lying(2)}; }) == DrawReason::None);
    CHECK(d.Incapacitated(2, 9750));
    d.Update({Lying(1)}, 10000);
    CHECK(!d.Down(2) && d.Down(1));
    uint32_t when = 0;
    CHECK(Run(d, 10250, 80000, [](uint32_t) { return std::vector<DownSample>{Lying(1), Lying(2)}; }, &when) == DrawReason::BothDown);
    CHECK(when == 10250 + DrawCheck::kDownMs + DrawCheck::kDrawMs);
}

int main() {
    TestBothDownDraws();
    TestOneDownNever();
    TestActingFromTheGround();
    TestHysteresis();
    TestDownSigns();
    TestStall();
    TestForgetting();
    if (g_fail) { std::printf("test_hsm_draw: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_draw: ok\n");
    return 0;
}

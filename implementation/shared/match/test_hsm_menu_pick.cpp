// Which main-menu widget the Multiplayer button belongs on. The game's settings screens remove the main menu and their
// Back button creates a new one (UI_GameSettings.cpp:123, 345); the old instance stays alive until garbage collection,
// so the button must follow the instance that is in the viewport, not the first one found or the one held before.
#include "match/hsm_menu_pick.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static const int kOld = 1, kNew = 2, kOther = 3;
static const void* Id(const int& x) { return &x; }

static void TestHeldStays() {
    CHECK(PickMenu(Id(kOld), true, {{Id(kOld), true}}) == Id(kOld));
    CHECK(PickMenu(Id(kOld), true, {{Id(kNew), true}, {Id(kOld), true}}) == Id(kOld));   // another one shown: keep ours
    CHECK(PickMenu(Id(kOld), true, {}) == Id(kOld));                                     // not rescanned
}

static void TestBackFromSettings() {
    // the held menu was removed, the Back button made a new one
    CHECK(PickMenu(Id(kOld), false, {{Id(kOld), false}, {Id(kNew), true}}) == Id(kNew));
    CHECK(PickMenu(Id(kOld), false, {{Id(kNew), true}, {Id(kOld), false}}) == Id(kNew));
}

static void TestNothingShown() {
    CHECK(PickMenu(Id(kOld), false, {{Id(kOld), false}}) == nullptr);                    // settings open: wait
    CHECK(PickMenu(nullptr, false, {}) == nullptr);
    CHECK(PickMenu(nullptr, false, {{Id(kOld), false}, {Id(kNew), false}}) == nullptr);
}

static void TestFirstPick() {
    CHECK(PickMenu(nullptr, false, {{Id(kOld), false}, {Id(kNew), true}}) == Id(kNew)); // not simply the first found
    CHECK(PickMenu(nullptr, false, {{Id(kNew), true}, {Id(kOther), true}}) == Id(kNew));
    CHECK(PickMenu(nullptr, false, {{nullptr, true}, {Id(kNew), true}}) == Id(kNew));    // a null entry is never picked
}

static void TestHeldBackInViewport() {
    // the same instance re-added to the viewport (no new one made): it is ours again
    CHECK(PickMenu(Id(kOld), false, {{Id(kOld), true}}) == Id(kOld));
}

int main() {
    TestHeldStays();
    TestBackFromSettings();
    TestNothingShown();
    TestFirstPick();
    TestHeldBackInViewport();
    if (g_fail) { std::printf("test_hsm_menu_pick: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_menu_pick: all passed\n");
    return 0;
}

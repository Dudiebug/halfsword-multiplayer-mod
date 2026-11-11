// The mod's pause panel: its buttons and their order, the second click, and when QUIT TO DESKTOP may close the game.
#include "match/hsm_pause_panel.h"
#include <cstdio>
#include <string_view>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestOrderAndLabels() {
    const char* want[kPauseButtonCount] = {"RESUME", "MULTIPLAYER MENU", "LOADOUT", "SETTINGS", "MOD SETTINGS", "REPORT A BUG",
                                           "LEAVE MATCH", "QUIT TO MAIN MENU", "QUIT TO DESKTOP"};
    const auto order = PauseButtonOrder();
    for (size_t i = 0; i < kPauseButtonCount; ++i) {
        CHECK(std::string_view(PauseButtonLabel(order[i])) == want[i]);
        CHECK(size_t(order[i]) == i);                        // the enum runs in the panel's order
    }
}

static void TestShown() {
    for (bool report : {false, true}) {
        CHECK(!PauseButtonShown(PauseButton::Loadout, report));   // hidden until a later change opens the game's loadout screen
        CHECK(PauseButtonShown(PauseButton::Resume, report));
        CHECK(PauseButtonShown(PauseButton::Multiplayer, report));
        CHECK(PauseButtonShown(PauseButton::Settings, report));
        CHECK(PauseButtonShown(PauseButton::ModSettings, report));
        CHECK(PauseButtonShown(PauseButton::LeaveMatch, report));
        CHECK(PauseButtonShown(PauseButton::QuitMainMenu, report));
        CHECK(PauseButtonShown(PauseButton::QuitDesktop, report));
    }
    CHECK(PauseButtonShown(PauseButton::ReportBug, true));
    CHECK(!PauseButtonShown(PauseButton::ReportBug, false));      // reports off, or not in a session fight
    CHECK(!kLoadoutButton);
}

static void TestConfirmLabels() {
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::LeaveMatch)) == "CLICK AGAIN TO LEAVE");
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::QuitMainMenu)) == "CLICK AGAIN TO QUIT");
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::QuitDesktop)) == "CLICK AGAIN TO QUIT");
    for (PauseButton b : {PauseButton::Resume, PauseButton::Multiplayer, PauseButton::Loadout, PauseButton::Settings,
                          PauseButton::ModSettings, PauseButton::ReportBug})
        CHECK(PauseButtonConfirm(b) == nullptr);
}

static void TestSecondClick() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::LeaveMatch, 1000));          // armed
    CHECK(c.Armed(PauseButton::LeaveMatch, 1000));
    CHECK(c.Click(PauseButton::LeaveMatch, 1500));           // done
    CHECK(!c.Armed(PauseButton::LeaveMatch, 1500));
    CHECK(!c.Click(PauseButton::LeaveMatch, 2000));          // armed again from scratch
    for (PauseButton b : {PauseButton::QuitMainMenu, PauseButton::QuitDesktop}) {
        PauseConfirm q;
        CHECK(!q.Click(b, 10));
        CHECK(q.Click(b, 20));
    }
}

static void TestOtherButtonDisarms() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::QuitDesktop, 1000));
    CHECK(!c.Click(PauseButton::QuitMainMenu, 1100));        // a different button arms itself, the first is off
    CHECK(!c.Armed(PauseButton::QuitDesktop, 1100));
    CHECK(c.Armed(PauseButton::QuitMainMenu, 1100));
    CHECK(!c.Click(PauseButton::QuitDesktop, 1200));         // so QUIT TO DESKTOP needs its own two clicks
    CHECK(c.Click(PauseButton::QuitDesktop, 1300));
    CHECK(!c.Click(PauseButton::LeaveMatch, 2000));
    CHECK(!c.Click(PauseButton::Settings, 2100));            // a button with no confirm disarms too
    CHECK(!c.Armed(PauseButton::LeaveMatch, 2100));
    CHECK(!c.Click(PauseButton::LeaveMatch, 2200));          // armed anew, not confirmed
}

static void TestConfirmExpires() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::LeaveMatch, 1000));
    CHECK(c.Armed(PauseButton::LeaveMatch, 1000 + kPauseConfirmMs - 1));
    CHECK(!c.Armed(PauseButton::LeaveMatch, 1000 + kPauseConfirmMs));
    CHECK(!c.Click(PauseButton::LeaveMatch, 1000 + kPauseConfirmMs));      // too late: armed again, not done
    CHECK(c.Click(PauseButton::LeaveMatch, 1000 + kPauseConfirmMs + 10));
    PauseConfirm d;
    CHECK(!d.Click(PauseButton::LeaveMatch, 0xFFFFFFF0u));
    CHECK(d.Armed(PauseButton::LeaveMatch, 0x00000010u));                  // across the tick count's wrap
    CHECK(d.Click(PauseButton::LeaveMatch, 0x00000010u));
    CHECK(kPauseConfirmMs == 4000);
}

static void TestClear() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::QuitDesktop, 1000));
    c.Clear();                                               // the panel closed
    CHECK(!c.Armed(PauseButton::QuitDesktop, 1001));
    CHECK(!c.Click(PauseButton::QuitDesktop, 1002));
}

static void TestQuitDesktop() {
    CHECK(QuitDesktopDue(false, 0));                         // the save is back already: close now
    CHECK(!QuitDesktopDue(true, 0));                         // the save guard still holds: wait
    CHECK(!QuitDesktopDue(true, kQuitDesktopMaxMs - 1));
    CHECK(QuitDesktopDue(true, kQuitDesktopMaxMs));          // the cap
    CHECK(QuitDesktopDue(false, kQuitDesktopMaxMs + 5000));
    CHECK(kQuitDesktopMaxMs == 8000);
}

int main() {
    TestOrderAndLabels(); TestShown(); TestConfirmLabels(); TestSecondClick(); TestOtherButtonDisarms(); TestConfirmExpires();
    TestClear(); TestQuitDesktop();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_pause_panel ok");
    return 0;
}

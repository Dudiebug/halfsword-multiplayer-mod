// The mod's pause panel: its ten buttons and their order, the title, what each button says and hints, the second click, and
// when QUIT TO DESKTOP may close the game.
#include "match/hsm_pause_panel.h"
#include "match/hsm_ui_text.h"
#include "session/hsm_loadout_change.h"
#include <cstdio>
#include <string>
#include <string_view>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestOrderAndLabels() {
    const char* want[kPauseButtonCount] = {"RESUME", "PLAYERS", "LOADOUT", "CHANGE LOADOUT", "GAME SETTINGS", "MOD SETTINGS",
                                           "REPORT A BUG", "BACK TO LOBBY", "LEAVE TO MAIN MENU", "QUIT TO DESKTOP"};
    CHECK(kPauseButtonCount == 10);
    const auto order = PauseButtonOrder();
    CHECK(order.size() == 10);
    for (size_t i = 0; i < kPauseButtonCount; ++i) {
        CHECK(std::string_view(PauseButtonLabel(order[i])) == want[i]);
        CHECK(size_t(order[i]) == i);                        // the enum runs in the panel's order
    }
}

// The title never says PAUSED (the world keeps running), and the sub-pages lead back by name.
static void TestTitleAndFixedLines() {
    CHECK(std::string_view(kPauseTitle) == "MENU");
    CHECK(std::string_view(kPauseTitleNote) == "The fight goes on behind this menu. You can still be hit.");
    CHECK(std::string_view(kPauseBackLabel) == "BACK TO MENU");
    CHECK(std::string_view(kPauseDefaultHint) == "Esc closes this.");
}

// The leaving buttons stand apart from the rest: nothing above RESUME, a wide gap before BACK TO LOBBY, a normal one elsewhere.
static void TestGaps() {
    CHECK(PauseButtonGapBefore(PauseButton::Resume) == 0.f);
    CHECK(PauseButtonGapBefore(PauseButton::BackToLobby) == 22.f);
    for (PauseButton b : {PauseButton::Players, PauseButton::Loadout, PauseButton::ChangeLoadout, PauseButton::GameSettings,
                          PauseButton::ModSettings, PauseButton::ReportBug, PauseButton::LeaveToMainMenu, PauseButton::QuitDesktop})
        CHECK(PauseButtonGapBefore(b) == 8.f);
}

static PauseRowIn Row(bool report, bool trial, bool host, bool changeShown = false, bool changeAvailable = false, const char* why = "", bool cryptsRun = false,
                      bool lobby = true, bool coop = false) {
    PauseRowIn in;
    in.reportOn = report; in.loadoutTrial = trial; in.host = host; in.cryptsRun = cryptsRun; in.lobby = lobby; in.coop = coop;
    in.changeShown = changeShown; in.changeAvailable = changeAvailable; in.changeWhy = why;
    return in;
}

// Which buttons show, which are dimmed, and the line each has under the panel.
static void TestRows() {
    for (bool report : {false, true}) for (bool trial : {false, true}) for (bool host : {false, true}) {
        const PauseRowIn in = Row(report, trial, host);
        CHECK(PauseRowOf(PauseButton::Loadout, in).shown == trial);                // only with loadout_trial=1
        CHECK(PauseRowOf(PauseButton::Loadout, in).enabled);
        CHECK(!PauseRowOf(PauseButton::ChangeLoadout, in).shown);                  // not in a match: no CHANGE LOADOUT
        for (PauseButton b : {PauseButton::Resume, PauseButton::Players, PauseButton::GameSettings, PauseButton::ModSettings,
                              PauseButton::ReportBug, PauseButton::BackToLobby, PauseButton::LeaveToMainMenu, PauseButton::QuitDesktop}) {
            CHECK(PauseRowOf(b, in).shown);                                        // REPORT A BUG is dimmed when off, never hidden
        }
        for (PauseButton b : {PauseButton::Resume, PauseButton::GameSettings, PauseButton::ModSettings, PauseButton::LeaveToMainMenu,
                              PauseButton::QuitDesktop, PauseButton::Players, PauseButton::Loadout}) {
            CHECK(PauseRowOf(b, in).enabled);
            CHECK(PauseRowOf(b, in).why.text.empty());
        }
        CHECK(PauseRowOf(PauseButton::ReportBug, in).enabled == report);
        CHECK(PauseRowOf(PauseButton::BackToLobby, in).enabled);                   // no Crypts run: the host's Travel, a guest's bench (PR5b)
        const PauseRowIn run = Row(report, trial, host, false, false, "", true);
        CHECK(!PauseRowOf(PauseButton::BackToLobby, run).enabled);                 // a Crypts run: nobody's
        CHECK(PauseRowOf(PauseButton::BackToLobby, run).shown);                    // dimmed, never hidden
        for (PauseButton b : {PauseButton::Resume, PauseButton::GameSettings, PauseButton::ModSettings, PauseButton::LeaveToMainMenu,
                              PauseButton::QuitDesktop, PauseButton::Players, PauseButton::Loadout})
            CHECK(PauseRowOf(b, run).enabled);                                     // the run touches no other button
        CHECK(PauseRowOf(PauseButton::ReportBug, run).enabled == report);
        const PauseRowIn noLobby = Row(report, trial, host, false, false, "", false, false);
        CHECK(!PauseRowOf(PauseButton::BackToLobby, noLobby).enabled && PauseRowOf(PauseButton::BackToLobby, noLobby).shown);   // no lobby kept: nobody's
        const PauseRowIn coop = Row(report, trial, host, false, false, "", false, true, true);
        CHECK(PauseRowOf(PauseButton::BackToLobby, coop).enabled == host);         // Progression co-op: the host's Travel works, a guest's bench does not
    }
    const PauseRowIn none = Row(true, false, false);
    for (PauseButton b : {PauseButton::Resume, PauseButton::GameSettings, PauseButton::ModSettings, PauseButton::LeaveToMainMenu,
                          PauseButton::QuitDesktop, PauseButton::ReportBug})
        CHECK(PauseRowOf(b, none).hint.empty());                                   // the name says it all
}

static void TestHints() {
    const PauseRowIn host = Row(true, true, true, true, true);
    CHECK(PauseRowOf(PauseButton::Players, host).hint == "Mute, kick or vote, and the network.");
    CHECK(PauseRowOf(PauseButton::Loadout, host).hint == "A trial: it may not work here.");
    CHECK(PauseRowOf(PauseButton::ChangeLoadout, host).hint == "Sends the loadout you edited to the host.");
    CHECK(PauseRowOf(PauseButton::BackToLobby, host).hint == "Ends the match for everyone. Nobody leaves the lobby.");
    CHECK(PauseRowOf(PauseButton::BackToLobby, host).hint == kBackToLobbyHostHint);
    CHECK(PauseRowOf(PauseButton::ReportBug, host).hint.empty());                  // reports on: the name says it
    const PauseRowIn off = Row(false, false, false);
    CHECK(PauseRowOf(PauseButton::ReportBug, off).hint == "Bug reports are off on this PC.");
    // A guest's BACK TO LOBBY (session/hsm_bench.h): it leaves this match alone and waits in the lobby.
    CHECK(PauseRowOf(PauseButton::BackToLobby, off).hint == "Leaves this match. You wait in the lobby for the host's next one.");
    CHECK(PauseRowOf(PauseButton::BackToLobby, off).hint == kBackToLobbyGuestHint);
    const PauseRowIn run = Row(true, true, true, true, true, "", true);
    CHECK(PauseRowOf(PauseButton::BackToLobby, run).hint == "Not during a Crypts run. Use LEAVE TO MAIN MENU.");
    CHECK(PauseRowOf(PauseButton::BackToLobby, run).hint == notices::Of(notices::Id::LobbyNotInCrypts).text);
    const PauseRowIn guest = Row(true, true, false, true, true, "", true);       // a guest in a run: the same rule as the host's
    CHECK(PauseRowOf(PauseButton::BackToLobby, guest).hint == "Not during a Crypts run. Use LEAVE TO MAIN MENU.");
    CHECK(PauseRowOf(PauseButton::BackToLobby, Row(true, true, true, true, true, "", false, false)).hint == "There is no lobby to go back to. Use LEAVE TO MAIN MENU.");
    CHECK(PauseRowOf(PauseButton::BackToLobby, Row(true, true, false, true, true, "", false, true, true)).hint == "Not during a Progression fight. Use LEAVE TO MAIN MENU.");
    CHECK(PauseRowOf(PauseButton::BackToLobby, Row(true, true, true, true, true, "", false, true, true)).hint == kBackToLobbyHostHint);   // the host's Travel works there
}

// Every line under the panel fits kHintMax (match/hsm_ui_text.h), whichever switches are on: the hints the panel owns, the
// countdowns, and CHANGE LOADOUT's reasons, which are the rule's own words (session/hsm_loadout_change.h) made a sentence.
static void TestHintsFit() {
    for (bool report : {false, true}) for (bool trial : {false, true}) for (bool host : {false, true}) for (bool run : {false, true}) {
        for (bool lobby : {false, true}) for (bool coop : {false, true})
            for (PauseButton b : PauseButtonOrder()) CHECK(HintFits(PauseRowOf(b, Row(report, trial, host, true, true, "", run, lobby, coop)).hint));
        for (uint8_t v = 0; v < kLoadoutVerdictCount; ++v) {
            const PauseRow r = PauseRowOf(PauseButton::ChangeLoadout, Row(report, trial, host, true, false, LoadoutVerdictText(LoadoutVerdict(v))));
            CHECK(HintFits(r.hint) && !r.hint.empty());
            CHECK(r.hint == r.why.text);                                           // the hint and the click's answer say the same
        }
    }
    for (PauseButton b : {PauseButton::BackToLobby, PauseButton::LeaveToMainMenu, PauseButton::QuitDesktop})
        for (uint32_t ms : {0u, 1u, 999u, 1000u, kPauseConfirmMs}) CHECK(HintFits(PauseArmedHint(b, ms)) && HintFits(PauseArmedHint(b, ms, false)));
    CHECK(HintFits(kBackToLobbyHostHint) && HintFits(kBackToLobbyGuestHint));
    CHECK(HintFits(kPauseDefaultHint) && HintFits(kPauseTitleNote));
}

// What a click on a dimmed button says: a notice with its own words, never the button doing something.
static void TestWhy() {
    const PauseRow rep = PauseRowOf(PauseButton::ReportBug, Row(false, false, false));
    CHECK(!rep.enabled);
    CHECK(rep.why.title == "REPORTS" && rep.why.key == "pause-why" && !rep.why.text.empty());
    CHECK(rep.why.text == notices::Of(notices::Id::ReportsOff).text);
    const PauseRow lob = PauseRowOf(PauseButton::BackToLobby, Row(true, false, false, false, false, "", false, false));   // no lobby kept
    CHECK(!lob.enabled);
    CHECK(lob.why.title == "LOBBY" && lob.why.key == "pause-why");
    CHECK(lob.why.text == "There is no lobby to go back to. Use LEAVE TO MAIN MENU." && lob.why.text == lob.hint);
    const PauseRow ok = PauseRowOf(PauseButton::BackToLobby, Row(true, false, true));
    CHECK(ok.enabled && ok.why.text.empty() && ok.why.title.empty());
    const PauseRow guestOk = PauseRowOf(PauseButton::BackToLobby, Row(true, false, false));   // a guest in a plain match: it works
    CHECK(guestOk.enabled && guestOk.why.text.empty() && guestOk.why.title.empty());
    // A guest in Progression co-op: dimmed with its own words, on the same key.
    const PauseRow coop = PauseRowOf(PauseButton::BackToLobby, Row(true, false, false, false, false, "", false, true, true));
    CHECK(!coop.enabled && coop.why.title == "LOBBY" && coop.why.key == "pause-why");
    CHECK(coop.why.text == "Not during a Progression fight. Use LEAVE TO MAIN MENU." && coop.why.text == coop.hint);
    // The host in a Crypts run: dimmed, with its own words, on the key the other dimmed buttons use (a second click replaces the first).
    const PauseRow run = PauseRowOf(PauseButton::BackToLobby, Row(true, false, true, false, false, "", true));
    CHECK(!run.enabled);
    CHECK(run.why.title == "LOBBY" && run.why.key == "pause-why");
    CHECK(run.why.text == "Not during a Crypts run. Use LEAVE TO MAIN MENU." && run.why.text == run.hint);
    // A guest in a run: the same words as the host's (PR5b).
    const PauseRow guest = PauseRowOf(PauseButton::BackToLobby, Row(true, false, false, false, false, "", true));
    CHECK(!guest.enabled && guest.why.text == "Not during a Crypts run. Use LEAVE TO MAIN MENU." && guest.why.text == guest.hint);
}

// CHANGE LOADOUT: shown in a match; dimmed with the rule's own line as the hint and as the click's answer.
static void TestChangeLoadout() {
    const PauseRow ok = PauseRowOf(PauseButton::ChangeLoadout, Row(true, false, false, true, true));
    CHECK(ok.shown && ok.enabled && ok.why.text.empty());
    const PauseRow no = PauseRowOf(PauseButton::ChangeLoadout, Row(true, false, false, true, false, "The host deals everyone's kit"));
    CHECK(no.shown && !no.enabled);
    CHECK(no.hint == "The host deals everyone's kit.");                             // a full stop where the rule's line had none
    CHECK(no.why.title == "LOADOUT" && no.why.key == "pause-why" && no.why.text == "The host deals everyone's kit.");
    const PauseRow dotted = PauseRowOf(PauseButton::ChangeLoadout, Row(true, false, false, true, false, "Not during the fight."));
    CHECK(dotted.hint == "Not during the fight." && dotted.why.text == "Not during the fight.");
    CHECK(!PauseRowOf(PauseButton::ChangeLoadout, Row(true, false, false, false, true)).shown);
}

// The LOADOUT trial: Esc always gets the player out; the screen's own exit counts once the opening frames are over.
static void TestLoadoutVisit() {
    CHECK(LoadoutVisitEnds(true, false, 0) == LoadoutVisitEnd::None && LoadoutVisitEnds(true, false, 60000) == LoadoutVisitEnd::None);
    CHECK(LoadoutVisitEnds(true, true, 0) == LoadoutVisitEnd::Esc && LoadoutVisitEnds(false, true, 0) == LoadoutVisitEnd::Esc);
    CHECK(LoadoutVisitEnds(false, true, 60000) == LoadoutVisitEnd::Esc);                    // Esc is named even when the screen is gone
    CHECK(LoadoutVisitEnds(false, false, 0) == LoadoutVisitEnd::None);                      // the frame it was made: not in the viewport yet
    CHECK(LoadoutVisitEnds(false, false, kLoadoutVisitGraceMs - 1) == LoadoutVisitEnd::None);
    CHECK(LoadoutVisitEnds(false, false, kLoadoutVisitGraceMs) == LoadoutVisitEnd::ScreenClosed);
    CHECK(LoadoutVisitEnds(false, false, 60000) == LoadoutVisitEnd::ScreenClosed);
    CHECK(kLoadoutVisitGraceMs == 500);
    CHECK(std::string_view(LoadoutVisitEndText(LoadoutVisitEnd::Esc)) == "Esc");
    CHECK(std::string_view(LoadoutVisitEndText(LoadoutVisitEnd::ScreenClosed)) == "the screen's own exit");
    CHECK(std::string_view(LoadoutVisitEndText(LoadoutVisitEnd::None)) == "still open");
}

static void TestConfirmLabels() {
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::BackToLobby)) == "CLICK AGAIN TO END THE MATCH");
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::BackToLobby, true)) == "CLICK AGAIN TO END THE MATCH");
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::BackToLobby, false)) == "CLICK AGAIN TO GO BACK");   // a guest's click leaves its own match only
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::LeaveToMainMenu)) == "CLICK AGAIN TO LEAVE");
    CHECK(std::string_view(PauseButtonConfirm(PauseButton::QuitDesktop)) == "CLICK AGAIN TO QUIT");
    for (PauseButton b : {PauseButton::Resume, PauseButton::Players, PauseButton::Loadout, PauseButton::ChangeLoadout,
                          PauseButton::GameSettings, PauseButton::ModSettings, PauseButton::ReportBug})
        CHECK(PauseButtonConfirm(b) == nullptr && PauseButtonConfirm(b, false) == nullptr);
    for (PauseButton b : {PauseButton::LeaveToMainMenu, PauseButton::QuitDesktop})
        CHECK(std::string_view(PauseButtonConfirm(b, false)) == PauseButtonConfirm(b, true) && std::string_view(PauseButtonLeaves(b, false)) == PauseButtonLeaves(b, true));   // only BACK TO LOBBY reads by role
    CHECK(std::string_view(PauseButtonLeaves(PauseButton::BackToLobby)) == "end the match for everyone");
    CHECK(std::string_view(PauseButtonLeaves(PauseButton::BackToLobby, true)) == "end the match for everyone");
    CHECK(std::string_view(PauseButtonLeaves(PauseButton::BackToLobby, false)) == "go back to the lobby");
    CHECK(std::string_view(PauseButtonLeaves(PauseButton::LeaveToMainMenu)) == "leave");
    CHECK(std::string_view(PauseButtonLeaves(PauseButton::QuitDesktop)) == "close the game");
}

static void TestSecondClick() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::LeaveToMainMenu, 1000));     // armed
    CHECK(c.Armed(PauseButton::LeaveToMainMenu, 1000));
    CHECK(c.Click(PauseButton::LeaveToMainMenu, 1500));      // done
    CHECK(!c.Armed(PauseButton::LeaveToMainMenu, 1500));
    CHECK(!c.Click(PauseButton::LeaveToMainMenu, 2000));     // armed again from scratch
    for (PauseButton b : {PauseButton::BackToLobby, PauseButton::LeaveToMainMenu, PauseButton::QuitDesktop}) {
        PauseConfirm q;
        CHECK(!q.Click(b, 10));
        CHECK(q.Click(b, 20));
    }
}

static void TestOtherButtonDisarms() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::QuitDesktop, 1000));
    CHECK(!c.Click(PauseButton::LeaveToMainMenu, 1100));     // a different button arms itself, the first is off
    CHECK(!c.Armed(PauseButton::QuitDesktop, 1100));
    CHECK(c.Armed(PauseButton::LeaveToMainMenu, 1100));
    CHECK(!c.Click(PauseButton::QuitDesktop, 1200));         // so QUIT TO DESKTOP needs its own two clicks
    CHECK(c.Click(PauseButton::QuitDesktop, 1300));
    CHECK(!c.Click(PauseButton::BackToLobby, 2000));
    CHECK(!c.Click(PauseButton::GameSettings, 2100));        // a button with no confirm disarms too
    CHECK(!c.Armed(PauseButton::BackToLobby, 2100));
    CHECK(!c.Click(PauseButton::BackToLobby, 2200));         // armed anew, not confirmed
}

static void TestConfirmExpires() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::LeaveToMainMenu, 1000));
    CHECK(c.Armed(PauseButton::LeaveToMainMenu, 1000 + kPauseConfirmMs - 1));
    CHECK(!c.Armed(PauseButton::LeaveToMainMenu, 1000 + kPauseConfirmMs));
    CHECK(!c.Click(PauseButton::LeaveToMainMenu, 1000 + kPauseConfirmMs));      // too late: armed again, not done
    CHECK(c.Click(PauseButton::LeaveToMainMenu, 1000 + kPauseConfirmMs + 10));
    PauseConfirm d;
    CHECK(!d.Click(PauseButton::LeaveToMainMenu, 0xFFFFFFF0u));
    CHECK(d.Armed(PauseButton::LeaveToMainMenu, 0x00000010u));                  // across the tick count's wrap
    CHECK(d.Click(PauseButton::LeaveToMainMenu, 0x00000010u));
    CHECK(kPauseConfirmMs == 4000);
}

static void TestClear() {
    PauseConfirm c;
    CHECK(!c.Click(PauseButton::QuitDesktop, 1000));
    c.Clear();                                               // the panel closed
    CHECK(!c.Armed(PauseButton::QuitDesktop, 1001));
    CHECK(!c.Waiting(1001));
    CHECK(!c.Click(PauseButton::QuitDesktop, 1002));
}

// Who waits and how long yet: the panel's countdown line is built from these.
static void TestWaiting() {
    PauseConfirm c;
    CHECK(!c.Waiting(0) && c.LeftMs(0) == 0);
    CHECK(!c.Click(PauseButton::BackToLobby, 1000));
    CHECK(c.Waiting(1000) && c.Which() == PauseButton::BackToLobby);
    CHECK(c.LeftMs(1000) == kPauseConfirmMs);
    CHECK(c.LeftMs(1000 + 1500) == kPauseConfirmMs - 1500);
    CHECK(c.LeftMs(1000 + kPauseConfirmMs - 1) == 1);
    CHECK(!c.Waiting(1000 + kPauseConfirmMs) && c.LeftMs(1000 + kPauseConfirmMs) == 0);
    CHECK(!c.Click(PauseButton::Resume, 2000) && !c.Waiting(2000));           // a plain click disarms
}

// The countdown reads whole seconds, rounded up, never under 1.
static void TestArmedHint() {
    CHECK(PauseArmedHint(PauseButton::LeaveToMainMenu, 4000) == "Click again within 4 s to leave. Any other click cancels.");
    CHECK(PauseArmedHint(PauseButton::QuitDesktop, 3001) == "Click again within 4 s to close the game. Any other click cancels.");
    CHECK(PauseArmedHint(PauseButton::BackToLobby, 3000) == "Click again within 3 s to end the match for everyone. Any other click cancels.");
    CHECK(PauseArmedHint(PauseButton::BackToLobby, 3000, false) == "Click again within 3 s to go back to the lobby. Any other click cancels.");
    CHECK(PauseArmedHint(PauseButton::LeaveToMainMenu, 1001) == "Click again within 2 s to leave. Any other click cancels.");
    CHECK(PauseArmedHint(PauseButton::LeaveToMainMenu, 1) == "Click again within 1 s to leave. Any other click cancels.");
    CHECK(PauseArmedHint(PauseButton::LeaveToMainMenu, 0) == "Click again within 1 s to leave. Any other click cancels.");
}

// The line under the panel: the armed button's countdown on that button; a hovered button's own hint; the countdown stays
// while the pointer is elsewhere; otherwise the default.
static void TestHintLine() {
    PauseConfirm c;
    CHECK(PauseHintLine(c, 0, false, PauseButton::Resume, "") == "Esc closes this.");
    CHECK(PauseHintLine(c, 0, true, PauseButton::Resume, "") == "Esc closes this.");
    CHECK(PauseHintLine(c, 0, true, PauseButton::Players, "Mute, kick or vote, and the network.") == "Mute, kick or vote, and the network.");
    CHECK(PauseHintLine(c, 0, false, PauseButton::Players, "Mute, kick or vote, and the network.") == "Esc closes this.");   // a hint needs the pointer on it
    CHECK(!c.Click(PauseButton::LeaveToMainMenu, 1000));
    const std::string armed = "Click again within 3 s to leave. Any other click cancels.";
    CHECK(PauseHintLine(c, 2000, true, PauseButton::LeaveToMainMenu, "") == armed);                  // on the armed button
    CHECK(PauseHintLine(c, 2000, true, PauseButton::LeaveToMainMenu, "its own hint") == armed);      // the countdown wins there
    CHECK(PauseHintLine(c, 2000, false, PauseButton::Resume, "") == armed);                          // pointer off the buttons
    CHECK(PauseHintLine(c, 2000, true, PauseButton::Resume, "") == armed);                           // on a button with no hint
    CHECK(PauseHintLine(c, 2000, true, PauseButton::Players, "Mute, kick or vote, and the network.") == "Mute, kick or vote, and the network.");
    CHECK(PauseHintLine(c, 1000 + kPauseConfirmMs, false, PauseButton::Resume, "") == "Esc closes this.");   // the wait ran out
    // BACK TO LOBBY waiting for its second click reads by role.
    PauseConfirm b;
    CHECK(!b.Click(PauseButton::BackToLobby, 5000));
    CHECK(PauseHintLine(b, 6000, true, PauseButton::BackToLobby, "x") == "Click again within 3 s to end the match for everyone. Any other click cancels.");
    CHECK(PauseHintLine(b, 6000, true, PauseButton::BackToLobby, "x", false) == "Click again within 3 s to go back to the lobby. Any other click cancels.");
    CHECK(PauseHintLine(b, 6000, false, PauseButton::Resume, "", false) == "Click again within 3 s to go back to the lobby. Any other click cancels.");
}

static void TestLookLabel() {
    using style::MenuLook;
    CHECK(PauseLookLabel(MenuLook::Classic, false) == "MENU LOOK: CLASSIC");
    CHECK(PauseLookLabel(MenuLook::Game, false) == "MENU LOOK: GAME ART");
    CHECK(PauseLookLabel(MenuLook::Dark, false) == "MENU LOOK: DARK");
    CHECK(PauseLookLabel(MenuLook::Dark, true) == "MENU LOOK: DARK (after restart)");
    CHECK(PauseLookLabel(MenuLook::Classic, true) == "MENU LOOK: CLASSIC (after restart)");
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
    TestOrderAndLabels(); TestTitleAndFixedLines(); TestGaps(); TestRows(); TestHints(); TestHintsFit(); TestWhy(); TestChangeLoadout(); TestLoadoutVisit();
    TestConfirmLabels(); TestSecondClick(); TestOtherButtonDisarms(); TestConfirmExpires(); TestClear(); TestWaiting(); TestArmedHint();
    TestHintLine(); TestLookLabel(); TestQuitDesktop();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_pause_panel ok");
    return 0;
}

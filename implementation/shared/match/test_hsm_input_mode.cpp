// Input-mode policy around the stock pause menu.
#include "match/hsm_input_mode.h"
#include <cstdio>
#include <string_view>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static InputModeState S(bool typing, bool menu, bool was, bool respawn, bool fix) {
    InputModeState s; s.typing = typing; s.menuOpen = menu; s.wasMenuOpen = was; s.respawnHappened = respawn; s.delayedFixDue = fix;
    return s;
}

static void TestMenuOpens() {
    CHECK(ChooseInputMode(S(false, true, false, false, false)) == InputMode::GameAndUI);   // rising edge: cursor once
    CHECK(ChooseInputMode(S(false, true, true, false, false)) == InputMode::Leave);        // already open: not again
}

static void TestMenuBlocksRestores() {
    CHECK(ChooseInputMode(S(false, true, true, true, false)) == InputMode::Leave);         // respawn under the menu
    CHECK(ChooseInputMode(S(false, true, true, false, true)) == InputMode::Leave);         // the delayed fix under the menu
    CHECK(ChooseInputMode(S(false, true, false, true, true)) == InputMode::GameAndUI);     // opens on the same tick
}

static void TestMenuCloses() {
    CHECK(ChooseInputMode(S(false, false, true, false, false)) == InputMode::GameOnly);    // restored once on close
    CHECK(ChooseInputMode(S(false, false, false, false, false)) == InputMode::Leave);
}

static void TestNormalRestores() {
    CHECK(ChooseInputMode(S(false, false, false, true, false)) == InputMode::GameOnly);
    CHECK(ChooseInputMode(S(false, false, false, false, true)) == InputMode::GameOnly);
}

static InputModeState Off(InputModeState s) { s.fightLevel = false; return s; }

static void TestOffFightLevelLeaves() {
    CHECK(ChooseInputMode(Off(S(false, false, true, false, false))) == InputMode::Leave);  // pause menu open, then the stock quit loaded the menu level
    CHECK(ChooseInputMode(Off(S(false, false, false, true, true))) == InputMode::Leave);   // respawn / delayed fix flags do not apply there
    CHECK(ChooseInputMode(Off(S(false, true, false, false, false))) == InputMode::Leave);
    CHECK(ChooseInputMode(S(false, false, true, false, false)) == InputMode::GameOnly);    // on a fight level it still restores once
}

static void TestTypingWins() {
    CHECK(ChooseInputMode(S(true, false, false, true, true)) == InputMode::Leave);
    CHECK(ChooseInputMode(S(true, true, false, false, false)) == InputMode::Leave);
    CHECK(ChooseInputMode(S(true, false, true, false, false)) == InputMode::Leave);        // closes after the typing ends
}

static void TestPauseScreens() {
    CHECK(PauseScreenOf("UI_Pause_Eng_C") == PauseScreen::Menu && PauseScreenOf("UI_Pause_C") == PauseScreen::Menu);
    for (const char* c : {"UI_DisplaySettings2_C", "UI_GameSettings_C", "UI_AudioSettings_C", "UI_CameraSettings_C",
                          "UI_GoreSettings_C", "UI_LanguageSettings_C", "UI_Controls_C", "UI_KeyBinds_C"})
        CHECK(PauseScreenOf(c) == PauseScreen::Settings);
    CHECK(PauseScreenOf("UI_HUD_C") == PauseScreen::None && PauseScreenOf("WBP_Settings_C") == PauseScreen::None);
    CHECK(PauseScreenOf("") == PauseScreen::None);
}

static EscContext Ctx() { EscContext c; c.sessionMatch = true; return c; }

static void TestEscNotOurs() {
    CHECK(EscOwnerOf(Ctx()) == EscOwner::OpenPanel);
    EscContext off = Ctx(); off.sessionMatch = false;            // the menus, a fight alone: the game's own Esc
    CHECK(EscOwnerOf(off) == EscOwner::Game);
    EscContext ini = Ctx(); ini.modPause = false;                // mod_pause=0
    CHECK(EscOwnerOf(ini) == EscOwner::Game);
    EscContext busy = off; busy.chatOpen = true; busy.panelOpen = true;   // outside a match no other flag matters
    CHECK(EscOwnerOf(busy) == EscOwner::Game);
    EscContext iniBusy = ini; iniBusy.settingsScreen = true;
    CHECK(EscOwnerOf(iniBusy) == EscOwner::Game);
}

static void TestEscBusyScreens() {
    EscContext c = Ctx(); c.chatOpen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Chat);
    c = Ctx(); c.textFocused = true;
    CHECK(EscOwnerOf(c) == EscOwner::TextBox);
    c = Ctx(); c.fetchMenu = true;
    CHECK(EscOwnerOf(c) == EscOwner::Fetch);
    c = Ctx(); c.lobbyPage = true;
    CHECK(EscOwnerOf(c) == EscOwner::Lobby);
    c = Ctx(); c.settingsScreen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Settings);
    c = Ctx(); c.panelOpen = true;
    CHECK(EscOwnerOf(c) == EscOwner::ClosePanel);
}

static void TestEscPrecedence() {
    EscContext c = Ctx(); c.panelOpen = true; c.settingsScreen = true;   // the panel is up under the settings screen: the screen goes back
    CHECK(EscOwnerOf(c) == EscOwner::Settings);
    c = Ctx(); c.panelOpen = true; c.lobbyPage = true;
    CHECK(EscOwnerOf(c) == EscOwner::Lobby);
    c = Ctx(); c.panelOpen = true; c.fetchMenu = true;
    CHECK(EscOwnerOf(c) == EscOwner::Fetch);
    c = Ctx(); c.panelOpen = true; c.textFocused = true;
    CHECK(EscOwnerOf(c) == EscOwner::TextBox);
    c = Ctx(); c.panelOpen = true; c.chatOpen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Chat);
    c = Ctx(); c.chatOpen = true; c.textFocused = true; c.fetchMenu = true; c.lobbyPage = true; c.settingsScreen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Chat);
    c = Ctx(); c.textFocused = true; c.fetchMenu = true; c.lobbyPage = true; c.settingsScreen = true;
    CHECK(EscOwnerOf(c) == EscOwner::TextBox);
    c = Ctx(); c.fetchMenu = true; c.lobbyPage = true; c.settingsScreen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Fetch);
    c = Ctx(); c.lobbyPage = true; c.settingsScreen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Lobby);
}

static void TestEscText() {
    const EscOwner all[] = {EscOwner::Chat, EscOwner::TextBox, EscOwner::Fetch, EscOwner::Lobby, EscOwner::Settings,
                            EscOwner::ClosePanel, EscOwner::OpenPanel};
    for (size_t i = 0; i < 7; ++i) {
        CHECK(EscOwnerText(all[i])[0] != 0);
        for (size_t j = i + 1; j < 7; ++j) CHECK(std::string_view(EscOwnerText(all[i])) != EscOwnerText(all[j]));
    }
    CHECK(std::string_view(EscOwnerText(EscOwner::OpenPanel)) == "mod panel");   // the log line "pause: Esc -> mod panel"
    CHECK(std::string_view(EscOwnerText(EscOwner::Game)).empty());
}

static void TestEscPress() {
    EscPress p;
    CHECK(!p.Step(false, false, 1000));
    CHECK(p.Step(true, false, 1000));            // the key goes down
    CHECK(!p.Step(true, false, 1016));           // held
    CHECK(!p.Step(true, false, 5000));           // held for seconds: still one press
    CHECK(!p.Step(false, false, 5016));
    CHECK(p.Step(true, false, 5032));            // pressed again, long after
}

static void TestEscPressEvent() {
    EscPress p;
    CHECK(p.Step(false, true, 1000));            // the game's event alone: P, or the gamepad's Start
    CHECK(!p.Step(false, false, 1016));
    EscPress q;
    CHECK(q.Step(true, false, 2000));            // Esc: the key first, the blocked event a frame later
    CHECK(!q.Step(true, true, 2020));            // the same press
    CHECK(!q.Step(true, false, 2040));
}

static void TestEscPressGap() {
    EscPress p;
    CHECK(p.Step(false, true, 1000));
    CHECK(!p.Step(false, true, 1249));           // a second event inside the gap is the same press
    CHECK(p.Step(false, true, 1249 + 1));        // 250 ms after the first press: a new one
    EscPress k;
    CHECK(k.Step(true, false, 100));
    CHECK(!k.Step(false, false, 120));
    CHECK(!k.Step(true, false, 150));            // key released and down again within the gap: not a new press
    CHECK(!k.Step(false, false, 160));
    CHECK(k.Step(true, false, 350));             // exactly kEscGapMs after the first
    CHECK(kEscGapMs == 250);
}

static void TestEscPressWraps() {
    EscPress p;
    CHECK(p.Step(false, true, 0xFFFFFFF0u));
    CHECK(!p.Step(false, true, 0xFFFFFFF8u));    // 8 ms later
    CHECK(!p.Step(false, true, 0x00000009u));    // 25 ms later, across the wrap
    CHECK(!p.Step(false, true, 0x000000E9u));    // 249 ms later
    CHECK(p.Step(false, true, 0x000000EAu));     // 250 ms later
}

static void TestEscPressStartsFromZero() {
    EscPress p;
    CHECK(p.Step(true, false, 0));               // the very first press, at tick 0, is not taken for a repeat
    EscPress q;
    CHECK(q.Step(false, true, 100));             // nor 100 ms into the run
}

int main() {
    TestEscNotOurs(); TestEscBusyScreens(); TestEscPrecedence(); TestEscText();
    TestEscPress(); TestEscPressEvent(); TestEscPressGap(); TestEscPressWraps(); TestEscPressStartsFromZero();
    TestPauseScreens();
    TestMenuOpens(); TestMenuBlocksRestores(); TestMenuCloses(); TestNormalRestores(); TestOffFightLevelLeaves(); TestTypingWins();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_input_mode ok");
    return 0;
}

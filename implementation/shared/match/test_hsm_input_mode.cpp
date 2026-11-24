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
    EscContext busy = off; busy.chatOpen = true; busy.panelOpen = true;   // outside a match no other flag matters
    CHECK(EscOwnerOf(busy) == EscOwner::Game);
    EscContext offSettings = off; offSettings.settingsScreen = true;   // nor does a settings screen outside a match
    CHECK(EscOwnerOf(offSettings) == EscOwner::Game);
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
    // An open dropdown list is above all of them: Esc closes the list, never the page, the panel or the chat under it.
    c = Ctx(); c.dropdownOpen = true; c.panelOpen = true; c.lobbyPage = true; c.settingsScreen = true; c.fetchMenu = true; c.textFocused = true; c.chatOpen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Dropdown);
    c = Ctx(); c.dropdownOpen = true;
    CHECK(EscOwnerOf(c) == EscOwner::Dropdown);                  // not OpenPanel: the press that closes a list must not open the panel
    c = Ctx(); c.dropdownOpen = true; c.sessionMatch = false;
    CHECK(EscOwnerOf(c) == EscOwner::Game);                      // outside a match the game's own Esc applies; the list closes itself
}

static void TestEscText() {
    const EscOwner all[] = {EscOwner::Dropdown, EscOwner::Chat, EscOwner::TextBox, EscOwner::Fetch, EscOwner::Lobby, EscOwner::Settings,
                            EscOwner::ClosePanel, EscOwner::OpenPanel};
    for (size_t i = 0; i < 8; ++i) {
        CHECK(EscOwnerText(all[i])[0] != 0);
        for (size_t j = i + 1; j < 8; ++j) CHECK(std::string_view(EscOwnerText(all[i])) != EscOwnerText(all[j]));
    }
    CHECK(std::string_view(EscOwnerText(EscOwner::Dropdown)) == "dropdown (it closes)");   // the log line "pause: Esc -> dropdown (it closes)"
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

static void TestRespawnShots() {
    RespawnInputFix f;
    CHECK(!f.Due(0) && f.Take(0) == -1 && !f.Watching(0));   // never armed
    f.Arm(1000);
    CHECK(!f.Due(1000) && !f.Due(1299));
    CHECK(f.Due(1300) && f.Take(1300) == 0 && !f.Due(1300));
    CHECK(!f.Due(1999));
    CHECK(f.Due(2000) && f.Take(2000) == 1);
    CHECK(!f.Due(3999));
    CHECK(f.Due(4000) && f.Take(4000) == 2);
    CHECK(!f.Due(4001) && !f.Due(100000));                   // three shots, then nothing
    CHECK(kRespawnInputShotsMs[0] == 300 && kRespawnInputShotsMs[1] == 1000 && kRespawnInputShotsMs[2] == 3000);
}

static void TestRespawnShotsMerged() {
    RespawnInputFix f;
    f.Arm(0);
    CHECK(f.Take(1500) == 1);                                // a long frame: the 0.3 s and 1 s shots are one restore
    CHECK(!f.Due(1500) && f.Take(2999) == -1);
    CHECK(f.Take(3000) == 2);
    RespawnInputFix g;
    g.Arm(0);
    CHECK(g.Take(5000) == 2 && !g.Due(5000));                // all three at once
}

static void TestRespawnRearm() {
    RespawnInputFix f;
    f.Arm(0);
    CHECK(f.Take(300) == 0);
    f.Arm(500);                                              // another respawn: the schedule starts over
    CHECK(!f.Due(799) && f.Take(800) == 0 && f.Take(1500) == 1);
    f.Clear();
    CHECK(!f.Due(100000) && !f.Watching(600));
}

static void TestRespawnExtra() {
    RespawnInputFix f;
    f.Extra(0);
    CHECK(!f.Due(0));                                        // no respawn: the Player DED clear restores nothing
    f.Arm(1000);
    f.Extra(1010);
    CHECK(f.Due(1010) && f.Take(1010) == -1 && !f.Due(1010));   // the extra restore, once
    CHECK(f.Take(1300) == 0);                                // the schedule is untouched
    f.Extra(1400);
    CHECK(f.Due(1400) && f.Take(1400) == -1 && f.Due(2000) && f.Take(2000) == 1);
    CHECK(f.Take(4000) == 2);
    f.Extra(1000 + kRespawnWatchMs - 1);                     // the last moment of the watch: still one more
    CHECK(f.Due(1000 + kRespawnWatchMs - 1) && f.Take(1000 + kRespawnWatchMs - 1) == -1 && !f.Due(1000 + kRespawnWatchMs - 1));
    f.Extra(1000 + kRespawnWatchMs);                         // after the watch: nothing
    CHECK(!f.Due(1000 + kRespawnWatchMs));
}

static void TestRespawnWatch() {
    RespawnInputFix f;
    f.Arm(0xFFFFF000u);                                      // across the GetTickCount wrap
    CHECK(f.Watching(0xFFFFF000u) && f.Watching(0x00001000u));
    CHECK(f.Watching(0xFFFFF000u + kRespawnWatchMs - 1) && !f.Watching(0xFFFFF000u + kRespawnWatchMs));
    CHECK(f.Take(0xFFFFF000u + 300) == 0);
    CHECK(kRespawnWatchMs == 10000);
}

int main() {
    TestRespawnShots(); TestRespawnShotsMerged(); TestRespawnRearm(); TestRespawnExtra(); TestRespawnWatch();
    TestEscNotOurs(); TestEscBusyScreens(); TestEscPrecedence(); TestEscText();
    TestEscPress(); TestEscPressEvent(); TestEscPressGap(); TestEscPressWraps(); TestEscPressStartsFromZero();
    TestPauseScreens();
    TestMenuOpens(); TestMenuBlocksRestores(); TestMenuCloses(); TestNormalRestores(); TestOffFightLevelLeaves(); TestTypingWins();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_input_mode ok");
    return 0;
}

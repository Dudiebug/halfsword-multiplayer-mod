// Input-mode policy around the stock pause menu.
#include "match/hsm_input_mode.h"
#include <cstdio>

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

int main() {
    TestMenuOpens(); TestMenuBlocksRestores(); TestMenuCloses(); TestNormalRestores(); TestOffFightLevelLeaves(); TestTypingWins();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_input_mode ok");
    return 0;
}

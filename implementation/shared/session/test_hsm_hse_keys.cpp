// Who gets the keys when HSMMod's chat and HS Enhancer's menu meet (#1290).
#include "session/hsm_hse_keys.h"
#include <cstdio>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static HseHolds Holds(bool lobby, bool typing) { HseHolds h; h.lobby = lobby; h.typing = typing; return h; }

static void TestNothingHeld() {
    for (HseMsg m : {HseMsg::Other, HseMsg::Input, HseMsg::Release}) CHECK(HseGetsMessage(Holds(false, false), m, false));
}

// The report: keys typed in the chat fired HS Enhancer's hotkeys.
static void TestChatHoldsItsInput() {
    CHECK(!HseGetsMessage(Holds(false, true), HseMsg::Input, false));
    CHECK(!HseGetsMessage(Holds(false, true), HseMsg::Input, true));      // not even its menu key: typing Insert does not open it
    CHECK(HseGetsMessage(Holds(false, true), HseMsg::Other, false));      // everything else reaches it
    CHECK(HseGetsMessage(Holds(false, true), HseMsg::Release, false));    // a key it saw go down comes up for it too
    CHECK(HseGetsMessage(Holds(false, true), HseMsg::Release, true));
}

static void TestLobbyHoldIsAsBefore() {
    CHECK(!HseGetsMessage(Holds(true, false), HseMsg::Input, false));
    CHECK(HseGetsMessage(Holds(true, false), HseMsg::Input, true));       // its menu key goes through
    CHECK(!HseGetsMessage(Holds(true, false), HseMsg::Release, false));   // a lobby hold holds the ups too, as it always did
    CHECK(HseGetsMessage(Holds(true, false), HseMsg::Other, false));
}

static void TestBothHolds() {
    CHECK(!HseGetsMessage(Holds(true, true), HseMsg::Input, true));       // typing wins: the menu key is held too
    CHECK(HseGetsMessage(Holds(true, true), HseMsg::Release, false));
    CHECK(HseGetsMessage(Holds(true, true), HseMsg::Other, false));
}

static HseMenuFacts Open() {
    HseMenuFacts f; f.share = f.ready = f.reports = f.keptInput = true; f.held = false;
    return f;
}

static void TestMenuOpenTakesTheKeys() {
    CHECK(HseMenuHasKeys(Open()));
    HseMenuFacts f;
    f = Open(); f.share = false;     CHECK(!HseMenuHasKeys(f));     // hse_share_keys=0
    f = Open(); f.ready = false;     CHECK(!HseMenuHasKeys(f));     // not in HS Enhancer's window hook: nothing to read
    f = Open(); f.reports = false;   CHECK(!HseMenuHasKeys(f));     // 0.5.3 passes raw input on either way: it cannot tell
    f = Open(); f.keptInput = false; CHECK(!HseMenuHasKeys(f));     // its menu is closed, or no input has told yet
    f = Open(); f.held = true;       CHECK(!HseMenuHasKeys(f));     // its input is held: the last state is old
    CHECK(!HseMenuHasKeys(HseMenuFacts{}));
}

int main() {
    TestNothingHeld(); TestChatHoldsItsInput(); TestLobbyHoldIsAsBefore(); TestBothHolds(); TestMenuOpenTakesTheKeys();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("ok");
    return 0;
}

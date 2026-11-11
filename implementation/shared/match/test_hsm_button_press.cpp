// Unit tests for hsm_button_press.h. Run via implementation/shared/run_tests.sh test_hsm_button_press.
#include "match/hsm_button_press.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

int main() {
    {   // a mouse click: on release, over the button; the focus it leaves is not the keys'
        ButtonPress b;
        CHECK(!b.Step(false, true, false));
        CHECK(!b.Step(true, true, true));
        CHECK(b.Step(false, true, true));
        CHECK(!b.Step(false, true, true));          // once
        CHECK(!b.keyFocus);
        CHECK(!b.Step(false, false, true) && !b.keyFocus);   // the mouse left: no highlight stays behind
    }
    {   // dragging off before the release cancels
        ButtonPress b;
        b.Step(true, true, true);
        CHECK(!b.Step(false, false, true));
    }
    {   // Enter after a mouse click, the mouse elsewhere (the chat key): not a second click
        ButtonPress b;
        b.Step(true, true, true); CHECK(b.Step(false, true, true));
        b.Step(false, false, true);
        CHECK(!b.Step(true, false, true));
        CHECK(!b.Step(false, false, true));
    }
    {   // the keys or a gamepad move the focus onto it: highlighted, and a press counts with the mouse elsewhere
        ButtonPress b;
        CHECK(!b.Step(false, false, true) && b.keyFocus);
        CHECK(!b.Step(true, false, true));
        CHECK(b.Step(false, false, true));
        CHECK(b.keyFocus);
        CHECK(!b.Step(false, false, false) && !b.keyFocus);   // the focus moved on
    }
    {   // a key press that loses the focus before its release does not count
        ButtonPress b;
        b.Step(false, false, true);
        b.Step(true, false, true);
        CHECK(!b.Step(false, false, false));
    }
    {   // a mouse press on a button the keys were on is a mouse click again
        ButtonPress b;
        b.Step(false, false, true);
        CHECK(!b.Step(true, true, true) && !b.keyFocus);
        CHECK(!b.Step(false, false, true));         // dragged off: cancelled, not a key click
    }
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hsm_button_press: all tests passed\n");
    return 0;
}

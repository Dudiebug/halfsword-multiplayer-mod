// The dropdown list (hsm_dropdown.h): open, pick, close on an outside click, Esc and a page change, one list at a time, a
// disabled option, opening upward, and scrolling. The rows that are dropdowns are walked in TestEveryRow (below).
#include "match/hsm_dropdown.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static std::vector<DropOption> Opts(size_t n) {
    std::vector<DropOption> v;
    for (size_t i = 0; i < n; ++i) v.push_back({"option " + std::to_string(i), true, ""});
    return v;
}
static DropPlace Below(size_t rows = 3) { DropPlace p; p.side = DropSide::Below; p.top = 100; p.height = 30.f * float(rows); p.rows = rows; return p; }

static void TestOpenAndPick() {
    Dropdown d;
    CHECK(!d.IsOpen() && !d.IsOpen(1));
    CHECK(d.Open(1, Opts(3), 1, 7, Below()));
    CHECK(d.IsOpen() && d.IsOpen(1) && !d.IsOpen(2) && d.Owner() == 1);
    CHECK(d.Picked() == 1 && d.Options().size() == 3);
    const DropEvent e = d.ClickOption(2);                          // a usable option: picked, and the list closes
    CHECK(e.kind == DropEvent::Kind::Picked && e.index == 2 && e.owner == 1);
    CHECK(!d.IsOpen());
    size_t got = 99;
    CHECK(!d.TakePick(2, got));                                    // not that control's pick
    CHECK(d.TakePick(1, got) && got == 2);
    CHECK(!d.TakePick(1, got));                                    // handed over once
    CHECK(!d.Open(1, {}, 0, 7, Below()) && !d.IsOpen());           // nothing to choose from: nothing opens
    CHECK(d.ClickOption(0).kind == DropEvent::Kind::None);         // a click on a list that is not open does nothing
}

static void TestPickedIsClamped() {
    Dropdown d;
    CHECK(d.Open(1, Opts(3), 99, 0, Below()));                     // a value the list does not have: the last option shows picked
    CHECK(d.Picked() == 2);
}

static void TestOutsideEscAndPage() {
    Dropdown d;
    d.Open(1, Opts(4), 0, 7, Below());
    DropEvent e = d.ClickOutside();                                // the backdrop: closes, picks nothing
    CHECK(e.kind == DropEvent::Kind::Closed && std::string(e.reason) == "outside" && !d.IsOpen());
    size_t got = 0;
    CHECK(!d.TakePick(1, got));
    CHECK(d.ClickOutside().kind == DropEvent::Kind::None);         // already closed: nothing to say twice

    d.Open(1, Opts(4), 0, 7, Below());
    e = d.Esc();                                                   // Esc closes the list
    CHECK(e.kind == DropEvent::Kind::Closed && std::string(e.reason) == "esc" && !d.IsOpen());
    CHECK(d.Esc().kind == DropEvent::Kind::None);                  // no list: the key is someone else's

    d.Open(1, Opts(4), 0, 7, Below());
    CHECK(d.Context(7).kind == DropEvent::Kind::None && d.IsOpen());   // the same page: stays open
    e = d.Context(8);                                              // another tab: closes
    CHECK(e.kind == DropEvent::Kind::Closed && std::string(e.reason) == "page" && !d.IsOpen());
    CHECK(d.Context(9).kind == DropEvent::Kind::None);             // closed: nothing to close
}

static void TestOneAtATime() {
    Dropdown d;
    d.Open(1, Opts(3), 0, 7, Below());
    d.Open(2, Opts(5), 3, 7, Below());                             // opening another list closes the first
    CHECK(d.IsOpen(2) && !d.IsOpen(1) && d.Options().size() == 5 && d.Picked() == 3);
    const DropEvent e = d.ClickOption(4);
    CHECK(e.kind == DropEvent::Kind::Picked && e.owner == 2);      // the pick belongs to the list that is open
    size_t got = 0;
    CHECK(!d.TakePick(1, got) && d.TakePick(2, got) && got == 4);
    d.Open(1, Opts(3), 0, 7, Below());
    d.ClickOption(1);
    d.Open(2, Opts(3), 0, 7, Below());                             // a pick nobody claimed is gone once another list opens
    CHECK(!d.TakePick(1, got));
}

static void TestDisabledOption() {
    Dropdown d;
    std::vector<DropOption> o = Opts(3);
    o[1].enabled = false; o[1].why = "Duel is for 2 players";
    d.Open(1, o, 0, 7, Below());
    DropEvent e = d.ClickOption(1);                                // explains and stays open
    CHECK(e.kind == DropEvent::Kind::Explain && e.why == "Duel is for 2 players" && e.index == 1);
    CHECK(d.IsOpen(1));
    size_t got = 0;
    CHECK(!d.TakePick(1, got));                                    // nothing was picked
    e = d.ClickOption(2);                                          // then a usable one still works
    CHECK(e.kind == DropEvent::Kind::Picked && !d.IsOpen());
    d.Open(1, o, 0, 7, Below());
    CHECK(d.ClickOption(3).kind == DropEvent::Kind::None && d.IsOpen());   // past the last option: ignored
}

static void TestPlaceBelowAndAbove() {
    // 3 options of 30 high: 90 needed. The control spans 100..130 in a 600-high space: room below.
    DropPlace p = PlaceDropdown(100, 130, 600, 30, 3);
    CHECK(p.side == DropSide::Below && p.top == 130 && p.rows == 3 && p.height == 90);
    // The control sits near the bottom (540..570): 30 left below, 540 above: the list opens upward, its bottom on the control's top.
    p = PlaceDropdown(540, 570, 600, 30, 3);
    CHECK(p.side == DropSide::Above && p.top == 450 && p.rows == 3 && p.height == 90);
    CHECK(p.top + p.height == 540);
    // Exactly enough room below still opens down; one pixel short opens up.
    p = PlaceDropdown(100, 130, 220, 30, 3);
    CHECK(p.side == DropSide::Below);
    p = PlaceDropdown(100, 130, 219, 30, 3);
    CHECK(p.side == DropSide::Above && p.top == 10);
    // Eight rows at most however many options: 21 options show 8 rows.
    p = PlaceDropdown(100, 130, 600, 30, 21);
    CHECK(p.rows == kDropdownRows && p.height == 240);
    // No room on either side for them all: the roomier side, as many whole rows as fit.
    p = PlaceDropdown(100, 130, 300, 30, 21);                      // 170 below (5 rows), 100 above (3 rows)
    CHECK(p.side == DropSide::Below && p.rows == 5 && p.height == 150);
    p = PlaceDropdown(200, 230, 300, 30, 21);                      // 70 below (2 rows), 200 above (6 rows)
    CHECK(p.side == DropSide::Above && p.rows == 6 && p.top == 20);
    p = PlaceDropdown(5, 20, 30, 30, 4);                           // no whole row fits anywhere: one row, on the roomier side (above 5, below 10)
    CHECK(p.side == DropSide::Below && p.rows == 1);
    p = PlaceDropdown(100, 130, 230, 30, 21);                      // 100 below and 100 above: equally roomy, so under the control
    CHECK(p.side == DropSide::Below && p.rows == 3 && p.top == 130);
    p = PlaceDropdown(-10, 1000, 600, 30, 5);                      // a control taller than the screen has no room below, not a negative room
    CHECK(p.side == DropSide::Below && p.rows == 1 && p.top == 1000);
    // Nothing to place.
    CHECK(PlaceDropdown(0, 10, 100, 30, 0).rows == 0);
    CHECK(PlaceDropdown(0, 10, 100, 0, 3).rows == 0);
}

static void TestScrolling() {
    // A list that fits shows from the top. A longer one scrolls to the picked option (about the middle), never past the end.
    CHECK(RevealFirst(0, 5, 8) == 0 && RevealFirst(4, 5, 8) == 0 && RevealFirst(8, 8, 8) == 0);
    CHECK(RevealFirst(6, 7, 8) == 0);                              // fewer options than rows: nothing to scroll, whatever is picked
    CHECK(RevealFirst(0, 21, 8) == 0);
    CHECK(RevealFirst(3, 21, 8) == 0);
    CHECK(RevealFirst(10, 21, 8) == 6);
    CHECK(RevealFirst(20, 21, 8) == 13);                           // the last option: the last window
    CHECK(RevealFirst(50, 21, 8) == 13);                           // a pick past the end clamps
    CHECK(RevealFirst(5, 21, 0) == 0);
    Dropdown d;
    d.Open(1, Opts(21), 15, 0, PlaceDropdown(100, 130, 600, 30, 21));
    CHECK(d.First() == 11 && d.Place().rows == 8);                 // the picked option is inside rows First()..First()+7
    CHECK(d.Picked() >= d.First() && d.Picked() < d.First() + d.Place().rows);
    d.Open(1, Opts(21), 20, 0, PlaceDropdown(100, 130, 600, 30, 21));
    CHECK(d.First() == 13 && d.Picked() < d.First() + d.Place().rows);
    d.Open(1, Opts(21), 0, 0, PlaceDropdown(100, 130, 600, 30, 21));
    CHECK(d.First() == 0);
    CHECK(kDropdownPool >= 21);                                    // the browser's mode filter fits the widget pool
    // Measured after it opened: the rows on screen change and the picked option is still inside them.
    d.Open(1, Opts(21), 15, 0, Below(8));
    d.Place(PlaceDropdown(100, 130, 300, 30, 21));                 // 5 rows fit below
    CHECK(d.Place().rows == 5 && d.First() == 13 && d.Picked() >= d.First() && d.Picked() < d.First() + d.Place().rows);
    d.Close();
    d.Place(Below(2));                                             // a list that is not open ignores it
    CHECK(!d.IsOpen() && d.Place().rows == 5 && d.First() == 13);
}

static void TestWidth() {
    std::vector<DropOption> v = {{"Own kits", true, ""}, {"Host's kit", true, ""}};            // short labels: the control or the minimum
    CHECK(DropWidth(300, v, 12, 40, 200, 1000) == 300);                                       // as wide as the control
    CHECK(DropWidth(80, v, 12, 40, 200, 1000) == 200);                                        // a narrow chip: the minimum
    v.insert(v.begin() + 1, {"Auto (South America)", true, ""});                              // 20 characters: 20 * 12 + 40; a shorter label after it
    CHECK(DropWidth(80, v, 12, 40, 200, 1000) == 280);
    CHECK(DropWidth(300, v, 12, 40, 200, 1000) == 300);                                       // the control is wider than the label needs
    CHECK(DropWidth(80, v, 12, 40, 200, 250) == 250);                                         // never wider than the screen allows
    CHECK(DropWidth(900, v, 12, 40, 200, 250) == 250);
    std::string accents;                                                                      // ten e-acutes: 10 characters in 20 bytes
    for (int i = 0; i < 10; ++i) accents += "\xC3\xA9";
    const std::vector<DropOption> utf = {{accents, true, ""}};
    CHECK(DropWidth(0, utf, 12, 40, 0, 1000) == 160);
    CHECK(DropWidth(0, {}, 12, 40, 0, 1000) == 40);                                           // nothing listed: only the padding
}

int main() {
    TestWidth();
    TestOpenAndPick();
    TestPickedIsClamped();
    TestOutsideEscAndPage();
    TestOneAtATime();
    TestDisabledOption();
    TestPlaceBelowAndAbove();
    TestScrolling();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_dropdown: ok\n");
    return 0;
}

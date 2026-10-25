// Chat look (MATCH_ROUNDS_UI_CHAT_PLAN 3.3): which lines the overlay shows and how faded, the sender's colour, the
// input row with its blinking caret and hint, "slow down" after a refused line, and the idle "Enter to chat" hint.
#include "match/hsm_chat_scene.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::style;

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
static bool Same(Rgba a, Rgba b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }

constexpr uint32_t kNow = 100000;

static ChatEntry Line(const char* name, const char* text, uint32_t ageMs, Rgba ink = kIron) {
    ChatEntry e; e.name = name; e.text = text; e.ink = ink; e.atMs = kNow - ageMs;
    return e;
}
static ChatLocal Shown() { ChatLocal c; c.hud = true; c.hudAtMs = kNow - 60000; return c; }   // HUD up for a minute

static void TestFadeOut() {
    CHECK(Near(FadeOut(0, 10000, 1000), 1));
    CHECK(Near(FadeOut(9000, 10000, 1000), 1));          // full until the last second
    CHECK(Near(FadeOut(9250, 10000, 1000), 0.75));
    CHECK(Near(FadeOut(9500, 10000, 1000), 0.5));
    CHECK(Near(FadeOut(9999, 10000, 1000), 0.001));
    CHECK(Near(FadeOut(10000, 10000, 1000), 0));
    CHECK(Near(FadeOut(10001, 10000, 1000), 0) && Near(FadeOut(60000, 10000, 1000), 0));
    CHECK(Near(FadeOut(19500, 20000, 1000), 0.5));
    CHECK(Near(FadeOut(500, 1000, 2000), 0.25));         // a fade longer than the life: never full, no underflow
    CHECK(Near(FadeOut(999, 1000, 0), 1) && Near(FadeOut(1000, 1000, 0), 0));   // no fade: a cut
    CHECK(kChatFadeMs == 1000 && kCaretBlinkMs == 530 && kChatHintMs == 20000 && kChatSlowMs == 1000);
}

static void TestCaret() {
    CHECK(CaretOn(5000, 5000));
    CHECK(CaretOn(5000, 5529));
    CHECK(!CaretOn(5000, 5530));                         // 530 ms on, 530 ms off
    CHECK(!CaretOn(5000, 6059));
    CHECK(CaretOn(5000, 6060));
    CHECK(!CaretOn(5000, 6590));
    CHECK(CaretOn(0xFFFFFF00u, 0x100u));                 // 512 ms across the counter wrap
    CHECK(!CaretOn(0xFFFFFF00u, 0x112u));                // 530 ms across it
    CHECK(CaretOn(6000, 5000));                          // opened "after" now (clock skew): on
}

static void TestKeyLabel() {
    CHECK(std::string(ChatKeyLabel(ChatKey::Enter)) == "Enter");
    CHECK(std::string(ChatKeyLabel(ChatKey::T)) == "T");
    CHECK(std::string(ChatKeyLabel(ChatKey::Y)) == "Y");
    CHECK(std::string(ChatKeyLabel(ChatKey::U)) == "U");
}

static void TestNameInk() {
    MatchInfo teams; teams.format = uint8_t(Format::TwoTeams);
    teams.fighters = {{1, 0, 0, 0, "Host", 0, true}, {2, 1, 0, 0, "Ann", 0, true}, {3, 7, 0, 0, "Odd", 0, true},
                      {4, -1, 0, 0, "Nobody", 0, true}, {5, 2, 0, 0, "Third", 0, true}};
    CHECK(Same(ChatNameInk(teams, 1), TeamInk(0)));
    CHECK(Same(ChatNameInk(teams, 2), TeamInk(1)));
    CHECK(Same(ChatNameInk(teams, 3), kIron));           // a team the format does not have
    CHECK(Same(ChatNameInk(teams, 4), kIron));           // no team
    CHECK(Same(ChatNameInk(teams, 5), kIron));           // the third team, in a two-team match
    CHECK(Same(ChatNameInk(teams, 9), kIron));           // not a fighter (a left player, a spectator)
    MatchInfo three = teams; three.format = uint8_t(Format::ThreeTeams); three.fighters[2].team = 2;
    CHECK(Same(ChatNameInk(three, 3), TeamInk(2)));
    MatchInfo ffa = teams; ffa.format = uint8_t(Format::FreeForAll);
    CHECK(Same(ChatNameInk(ffa, 1), kIron) && Same(ChatNameInk(ffa, 2), kIron));   // stale teams mean nothing here
    MatchInfo duel = teams; duel.format = uint8_t(Format::Duel);
    CHECK(Same(ChatNameInk(duel, 2), kIron));
    MatchInfo pvb = teams; pvb.format = uint8_t(Format::PlayersVsBots);
    CHECK(Same(ChatNameInk(pvb, 1), kIron) && Same(ChatNameInk(pvb, 2), kIron));   // everyone talking is a player
    MatchInfo zombies = pvb; zombies.format = uint8_t(Format::Zombies);
    CHECK(Same(ChatNameInk(zombies, 1), kIron));
}

static void TestHidden() {
    ChatLocal c = Shown();
    c.hud = false;
    c.lines = {Line("Ann", "hi", 0)};
    c.typing = true; c.typingAtMs = kNow; c.draft = "x";
    c.hudAtMs = kNow;
    ChatScene s = BuildChatScene(c, kNow);
    CHECK(!s.visible && s.lines.empty() && !s.input && !s.idle.visible && Near(s.panel, 0));
}

static void TestLog() {
    ChatLocal c = Shown();
    c.lines = {Line("Old", "gone", 15000), Line("Edge", "just gone", 10000), Line("Ann", "fading", 9500, TeamInk(1)),
               Line("Bo", "hello there", 0)};
    ChatScene s = BuildChatScene(c, kNow);
    CHECK(s.visible && !s.input);
    CHECK(s.lines.size() == 2);
    if (s.lines.size() == 2) {
        CHECK(s.lines[0].name.text == "Ann:" && Same(s.lines[0].name.color, TeamInk(1)));
        CHECK(s.lines[0].text.text == "fading" && Same(s.lines[0].text.color, kBone));
        CHECK(Near(s.lines[0].opacity, 0.5));
        CHECK(s.lines[1].name.text == "Bo:" && Same(s.lines[1].name.color, kIron) && s.lines[1].text.text == "hello there");
        CHECK(Near(s.lines[1].opacity, 1));
    }
    CHECK(Near(s.panel, 1));                             // the panel is as strong as its strongest line

    ChatLocal fading = Shown();
    fading.lines = {Line("Ann", "a", 9750), Line("Bo", "b", 9500)};
    ChatScene f = BuildChatScene(fading, kNow);
    CHECK(f.lines.size() == 2 && Near(f.panel, 0.5));
    if (f.lines.size() == 2) CHECK(Near(f.lines[0].opacity, 0.25) && Near(f.lines[1].opacity, 0.5));

    ChatLocal skewed = Shown();                          // stamps out of order: the panel follows the strongest line
    skewed.lines = {Line("Ann", "a", 0), Line("Bo", "b", 9500)};
    CHECK(Near(BuildChatScene(skewed, kNow).panel, 1));

    ChatLocal stale = Shown();
    stale.lines = {Line("Ann", "a", 10000), Line("Bo", "b", 20000)};
    ChatScene g = BuildChatScene(stale, kNow);
    CHECK(g.visible && g.lines.empty() && Near(g.panel, 0));

    ChatLocal empty = Shown();
    ChatScene e = BuildChatScene(empty, kNow);
    CHECK(e.visible && e.lines.empty() && Near(e.panel, 0) && !e.input);

    ChatLocal future = Shown();
    future.lines = {Line("Ann", "early", 0)};
    future.lines[0].atMs = kNow + 50;                    // stamped a moment ahead: shown, not expired
    ChatScene fu = BuildChatScene(future, kNow);
    CHECK(fu.lines.size() == 1 && (fu.lines.empty() || Near(fu.lines[0].opacity, 1)));

    ChatLocal noName = Shown();
    noName.lines = {Line("", "who", 0)};
    ChatScene nn = BuildChatScene(noName, kNow);
    CHECK(nn.lines.size() == 1 && (nn.lines.empty() || nn.lines[0].name.text == "?:"));
}

static void TestNewestEight() {
    ChatLocal c = Shown();
    for (int i = 0; i < 11; ++i) c.lines.push_back(Line("P", std::to_string(i).c_str(), uint32_t(11 - i) * 10));
    ChatScene s = BuildChatScene(c, kNow);
    CHECK(s.lines.size() == kChatLogLines);
    if (s.lines.size() == kChatLogLines) CHECK(s.lines.front().text.text == "3" && s.lines.back().text.text == "10");
    c.typing = true; c.typingAtMs = kNow;
    ChatScene t = BuildChatScene(c, kNow);
    CHECK(t.lines.size() == kChatLogLines);
    if (t.lines.size() == kChatLogLines) CHECK(t.lines.front().text.text == "3" && t.lines.back().text.text == "10");
}

static void TestTyping() {
    ChatLocal c = Shown();
    c.lines = {Line("Old", "older than ten seconds", 45000), Line("Ann", "fading", 9500), Line("Bo", "new", 0)};
    c.typing = true; c.typingAtMs = kNow - 100; c.draft = "on my w";
    ChatScene s = BuildChatScene(c, kNow);
    CHECK(s.visible && s.input && s.draft == "on my w" && s.caret);
    CHECK(s.lines.size() == 3);                          // typing shows the kept history, unfaded
    for (const ChatLineView& l : s.lines) CHECK(Near(l.opacity, 1));
    if (s.lines.size() == 3) CHECK(s.lines[0].text.text == "older than ten seconds" && s.lines[2].text.text == "new");
    CHECK(Near(s.panel, 1));
    CHECK(s.hint.text == "Enter send \xC2\xB7 Esc cancel" && Same(s.hint.color, kIron));
    CHECK(!BuildChatScene(c, kNow + 430).caret);         // 530 ms after it opened
    CHECK(BuildChatScene(c, kNow + 960).caret);

    ChatLocal blank = Shown();
    blank.typing = true; blank.typingAtMs = kNow;
    ChatScene b = BuildChatScene(blank, kNow);
    CHECK(b.input && b.draft.empty() && b.caret && b.lines.empty() && Near(b.panel, 0));

    ChatLocal idle = Shown();
    idle.draft = "left over";                            // a draft field the overlay did not clear: not typed
    ChatScene i = BuildChatScene(idle, kNow);
    CHECK(!i.input && i.draft.empty() && !i.caret);
}

static void TestSlowDown() {
    ChatLocal c = Shown();
    c.typing = true; c.typingAtMs = kNow - 5000; c.draft = "spam";
    c.refused = true; c.refusedAtMs = kNow - 999;
    ChatScene s = BuildChatScene(c, kNow);
    CHECK(s.hint.text == "slow down" && Same(s.hint.color, kWarn));
    c.refusedAtMs = kNow;
    CHECK(BuildChatScene(c, kNow).hint.text == "slow down");
    c.refusedAtMs = kNow - 1000;                         // shown for one second
    ChatScene after = BuildChatScene(c, kNow);
    CHECK(after.hint.text == "Enter send \xC2\xB7 Esc cancel" && Same(after.hint.color, kIron));
    c.refused = false; c.refusedAtMs = kNow;             // the stamp alone is not a refusal
    CHECK(BuildChatScene(c, kNow).hint.text == "Enter send \xC2\xB7 Esc cancel");
    c.refused = true; c.refusedAtMs = kNow + 20;         // stamped a moment ahead
    CHECK(BuildChatScene(c, kNow).hint.text == "slow down");
}

static void TestIdleHint() {
    ChatLocal c = Shown();
    c.hudAtMs = kNow;
    ChatScene s = BuildChatScene(c, kNow);
    CHECK(s.idle.visible && s.idle.text == "Enter to chat" && Same(s.idle.color, kIron) && Near(s.idle.opacity, 1));
    CHECK(Near(BuildChatScene(c, kNow + 19000).idle.opacity, 1));
    ChatScene late = BuildChatScene(c, kNow + 19500);
    CHECK(late.idle.visible && Near(late.idle.opacity, 0.5));   // fades over its last second
    CHECK(!BuildChatScene(c, kNow + 20000).idle.visible);       // the first 20 s only
    CHECK(!BuildChatScene(c, kNow + 90000).idle.visible);
    c.key = ChatKey::T;
    CHECK(BuildChatScene(c, kNow).idle.text == "T to chat");
    c.typing = true; c.typingAtMs = kNow;
    CHECK(!BuildChatScene(c, kNow + 1000).idle.visible);       // not while typing
    ChatLocal skew = Shown();
    skew.hudAtMs = kNow + 300;                                  // came up "after" now: still the start
    CHECK(BuildChatScene(skew, kNow).idle.visible);
}

int main() {
    TestFadeOut();
    TestCaret();
    TestKeyLabel();
    TestNameInk();
    TestHidden();
    TestLog();
    TestNewestEight();
    TestTyping();
    TestSlowDown();
    TestIdleHint();
    if (g_fail) { std::printf("%d chat scene checks failed\n", g_fail); return 1; }
    std::printf("all chat scene tests passed\n");
    return 0;
}

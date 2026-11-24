// Notices: the stack's order, keyed replace, Clear, the limit and its eviction rule, life per kind (which starts at the
// first View that shows a notice), the 60 s wait for a first show, the fades, the clock, and the sanitizer's cap on
// multibyte text. A test that wants a notice's life to start at its push calls View right after the push.
#include "match/hsm_notice.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::style;

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
static bool Same(Rgba a, Rgba b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }

static Notice N(NoticeKind k, const char* text, const char* key = "", const char* title = "T") {
    Notice n; n.kind = k; n.title = title; n.text = text; n.key = key;
    return n;
}
static bool Valid8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(s, i, len, cp)) return false;
        i += len;
    }
    return true;
}

static void TestLifeAndInk() {
    CHECK(NoticeLifeMs(NoticeKind::Info) == 6000 && NoticeLifeMs(NoticeKind::Done) == 6000);
    CHECK(NoticeLifeMs(NoticeKind::Warn) == 15000 && NoticeLifeMs(NoticeKind::Error) == 20000);
    CHECK(Same(NoticeInk(NoticeKind::Info), kIron) && Same(NoticeInk(NoticeKind::Done), kOk));
    CHECK(Same(NoticeInk(NoticeKind::Warn), kWarn) && Same(NoticeInk(NoticeKind::Error), kBloodInk));
    CHECK(kNoticeShown == 4 && kNoticeMaxBytes == 160 && kNoticeFixedBytes == 96);
}

static void TestExpiry() {
    NoticeStack s;
    const uint32_t t0 = 1000;
    s.Push(N(NoticeKind::Info, "i"), t0);
    s.Push(N(NoticeKind::Done, "d"), t0);
    s.Push(N(NoticeKind::Warn, "w"), t0);
    s.Push(N(NoticeKind::Error, "e"), t0);
    CHECK(s.View(t0).rows.size() == 4);                  // on screen at once: the life starts here
    CHECK(s.View(t0 + 5999).rows.size() == 4);
    NoticeScene v = s.View(t0 + 6000);                   // Info and Done end at exactly their life
    CHECK(v.rows.size() == 2 && v.rows[0].text == "e" && v.rows[1].text == "w");
    CHECK(s.View(t0 + 14999).rows.size() == 2);
    v = s.View(t0 + 15000);
    CHECK(v.rows.size() == 1 && v.rows[0].text == "e");
    CHECK(s.View(t0 + 19999).rows.size() == 1);
    CHECK(s.View(t0 + 20000).rows.empty() && s.View(t0 + 20000).more == 0);
    // A life of its own.
    Notice n = N(NoticeKind::Info, "long"); n.lifeMs = 30000;
    NoticeStack c; c.Push(n, 0); c.View(0);
    CHECK(c.View(29999).rows.size() == 1 && c.View(30000).rows.empty());
}

static void TestOrder() {
    NoticeStack s;
    s.Push(N(NoticeKind::Info, "one"), 100);
    s.Push(N(NoticeKind::Warn, "two"), 200);
    s.Push(N(NoticeKind::Done, "three"), 300);
    const NoticeScene v = s.View(400);
    CHECK(v.rows.size() == 3 && v.more == 0);
    CHECK(v.rows[0].text == "three" && v.rows[1].text == "two" && v.rows[2].text == "one");     // newest on top
    CHECK(v.rows[0].kind == NoticeKind::Done && Same(v.rows[0].ink, kOk));
    CHECK(Same(v.rows[1].ink, kWarn) && Same(v.rows[2].ink, kIron));
}

static void TestTitleAndText() {
    NoticeStack s;
    CHECK(s.Push(N(NoticeKind::Info, "text", "", "crypts"), 0));
    CHECK(s.View(10).rows[0].title == "CRYPTS");                                                 // upper case
    CHECK(!s.Push(N(NoticeKind::Info, ""), 10));                                                 // no text: nothing to show
    CHECK(!s.Push(N(NoticeKind::Info, " \t\n "), 10));
    CHECK(s.View(10).rows.size() == 1);
    NoticeStack t;
    t.Push(N(NoticeKind::Info, "x", "", "a title that is much too long to show"), 0);
    CHECK(t.View(1).rows[0].title == "A TITLE THAT IS MUCH");                                    // 20 bytes at most
    Notice k = N(NoticeKind::Info, "x", std::string(200, 'k').c_str());
    NoticeStack u; u.Push(k, 0); u.Push(k, 5);                                                   // a long key still matches itself
    CHECK(u.View(10).rows.size() == 1);
    NoticeStack w;                                                                               // keys count for their first 48 bytes
    w.Push(N(NoticeKind::Info, "x", (std::string(48, 'k') + "a").c_str()), 0);
    w.Push(N(NoticeKind::Info, "y", (std::string(48, 'k') + "b").c_str()), 1);
    CHECK(w.View(2).rows.size() == 1 && w.View(2).rows[0].text == "y");
}

static void TestKeyedReplace() {
    NoticeStack s;
    s.Push(N(NoticeKind::Warn, "1 minute left", "crypts-minute"), 0);
    s.View(0);
    s.Push(N(NoticeKind::Info, "other"), 1000);
    s.View(1000);
    s.Push(N(NoticeKind::Warn, "1 minute left", "crypts-minute"), 2000);                         // pushed twice: still one
    NoticeScene v = s.View(2500);
    CHECK(v.rows.size() == 2);
    CHECK(v.rows[0].text == "1 minute left" && v.rows[1].text == "other");                       // and it is back on top
    // On screen already, so the life starts again from the second push.
    CHECK(s.View(2000 + 14999).rows.size() >= 1 && s.View(2000 + 14999).rows[0].text == "1 minute left");
    CHECK(s.View(2000 + 15000).rows.empty());
    // A different key does not replace.
    NoticeStack t;
    t.Push(N(NoticeKind::Warn, "a", "k1"), 0);
    t.Push(N(NoticeKind::Warn, "a", "k2"), 0);
    CHECK(t.View(1).rows.size() == 2);
    // No key never replaces.
    NoticeStack u;
    u.Push(N(NoticeKind::Warn, "same"), 0);
    u.Push(N(NoticeKind::Warn, "same"), 0);
    CHECK(u.View(1).rows.size() == 2);
    // The same key with new content: the new kind, text and ink win.
    NoticeStack w;
    w.Push(N(NoticeKind::Warn, "Put it back within a minute.", "class-self"), 0);
    w.View(0);
    w.Push(N(NoticeKind::Done, "Your class fits the rules again.", "class-self"), 500);
    v = w.View(600);
    CHECK(v.rows.size() == 1 && v.rows[0].kind == NoticeKind::Done && v.rows[0].text == "Your class fits the rules again.");
    CHECK(w.View(600 + 5999).rows.size() == 1);                                                  // and it lives as a Done,
    CHECK(w.View(600 + 6000).rows.empty());                                                      // from when the new one is first shown
}

static void TestRefreshDoesNotFlash() {
    NoticeStack s;
    s.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 0);
    s.View(0);
    const uint32_t first = s.View(5000).rows[0].serial;
    CHECK(Near(s.View(5000).rows[0].opacity, 1));
    s.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 5000);                                      // the same again, mid-life
    NoticeScene v = s.View(5001);
    CHECK(v.rows.size() == 1 && v.rows[0].serial == first && Near(v.rows[0].opacity, 1));        // no new fade in
    s.Push(N(NoticeKind::Warn, "fps 20", "frame-rate"), 6000);                                   // new text
    s.View(6000);
    v = s.View(6050);
    CHECK(v.rows[0].serial != first && Near(v.rows[0].opacity, 0.25));                           // fades in again
    // The same words in another kind are new content too (a warning settled as done fades in as done).
    NoticeStack k;
    k.Push(N(NoticeKind::Warn, "same words", "class-self"), 0);
    const uint32_t warnSerial = k.View(1000).rows[0].serial;
    k.Push(N(NoticeKind::Done, "same words", "class-self"), 1000);
    CHECK(k.View(1000).rows[0].serial != warnSerial && k.View(1000).rows[0].kind == NoticeKind::Done);
    k.Push(N(NoticeKind::Done, "same words", "class-self", "other title"), 2000);                 // and so is another title
    CHECK(k.View(2000).rows[0].title == "OTHER TITLE" && Near(k.View(2000).rows[0].opacity, 0));
    // Another notice has its own serial.
    s.Push(N(NoticeKind::Info, "x"), 6100);
    v = s.View(6200);
    CHECK(v.rows[0].serial != v.rows[1].serial);
}

static void TestClear() {
    NoticeStack s;
    s.Push(N(NoticeKind::Warn, "a", "class-self"), 0);
    s.Push(N(NoticeKind::Warn, "b", "class-host"), 0);
    s.Push(N(NoticeKind::Info, "c"), 0);
    s.Clear("class-self");
    NoticeScene v = s.View(1);
    CHECK(v.rows.size() == 2 && v.rows[0].text == "c" && v.rows[1].text == "b");
    s.Clear("nothing");                                                                          // an unknown key: nothing happens
    CHECK(s.View(1).rows.size() == 2);
    s.Clear("");                                                                                 // the empty key is no key: "c" stays
    CHECK(s.View(1).rows.size() == 2);
    s.Clear("class-host");
    CHECK(s.View(1).rows.size() == 1);
    s.Push(N(NoticeKind::Warn, "again", "class-self"), 2);                                       // a cleared key can be used again
    CHECK(s.View(3).rows.size() == 2);
    s.ClearAll();
    CHECK(s.View(3).rows.empty() && s.Size() == 0);
}

static void TestFades() {
    NoticeStack s;
    s.Push(N(NoticeKind::Warn, "w"), 10000);
    CHECK(Near(s.View(10000).rows[0].opacity, 0));                                               // in: 200 ms
    CHECK(Near(s.View(10100).rows[0].opacity, 0.5));
    CHECK(Near(s.View(10200).rows[0].opacity, 1));
    CHECK(Near(s.View(10000 + 14000).rows[0].opacity, 1));                                       // out: the last second
    CHECK(Near(s.View(10000 + 14250).rows[0].opacity, 0.75));
    CHECK(Near(s.View(10000 + 14500).rows[0].opacity, 0.5));
    CHECK(Near(s.View(10000 + 14999).rows[0].opacity, 0.001));
    // A life shorter than the two fades: the lower of the two.
    Notice n = N(NoticeKind::Info, "blink"); n.lifeMs = 300;
    NoticeStack t; t.Push(n, 0); t.View(0);
    CHECK(Near(t.View(100).rows[0].opacity, 0.2));           // fading in (0.5) and out (0.2) at once
}

static void TestClock() {
    NoticeStack s;
    const uint32_t near = 0xFFFFFF00u;                       // the tick counter wraps while a notice is up
    s.Push(N(NoticeKind::Info, "w"), near);
    s.View(near);
    CHECK(s.View(near + 5000).rows.size() == 1);             // across the wrap: 5 s old
    CHECK(s.View(near + 6000).rows.empty());
    // A clock that stepped back counts as no time: the notice stays, fully faded out of nothing, then lives its life.
    NoticeStack t;
    t.Push(N(NoticeKind::Info, "back"), 50000);
    t.View(50000);
    CHECK(t.View(40000).rows.size() == 1);
    CHECK(Near(t.View(40000).rows[0].opacity, 0));
}

static void TestLifeStartsAtFirstShow() {
    // Pushed with no HUD up: nothing has used up its life, whatever the wait.
    NoticeStack s;
    s.Push(N(NoticeKind::Info, "early"), 1000);
    NoticeScene v = s.View(1000 + 59000);                    // first on screen 59 s later
    CHECK(v.rows.size() == 1 && Near(v.rows[0].opacity, 0));                                     // and it fades in from here
    CHECK(Near(s.View(1000 + 59100).rows[0].opacity, 0.5));
    CHECK(s.View(1000 + 59000 + 5999).rows.size() == 1);     // its whole 6 s
    CHECK(s.View(1000 + 59000 + 6000).rows.empty());
    // Looking again does not start the clock again.
    NoticeStack t;
    t.Push(N(NoticeKind::Info, "once"), 0);
    t.View(5000);
    t.View(8000);
    t.View(10999);
    CHECK(t.View(10999).rows.size() == 1);
    CHECK(t.View(11000).rows.empty());                       // 6 s after the first show at 5000
    // A kind with a longer life keeps it from the first show too.
    NoticeStack w;
    w.Push(N(NoticeKind::Warn, "warn"), 0);
    w.View(30000);
    CHECK(w.View(30000 + 14999).rows.size() == 1 && w.View(30000 + 15000).rows.empty());
    NoticeStack own; Notice n = N(NoticeKind::Info, "own"); n.lifeMs = 2000;
    own.Push(n, 0);
    own.View(9000);
    CHECK(own.View(10999).rows.size() == 1 && own.View(11000).rows.empty());
}

static void TestWaitForFirstShow() {
    // Never on screen for more than 60 s: dropped. Exactly 60 s is still fine.
    CHECK(kNoticeWaitMs == 60000);
    NoticeStack s;
    s.Push(N(NoticeKind::Error, "late"), 1000);
    CHECK(s.View(1000 + 60000).rows.size() == 1);
    NoticeStack t;
    t.Push(N(NoticeKind::Error, "too late"), 1000);
    NoticeScene v = t.View(1000 + 60001);
    CHECK(v.rows.empty() && v.more == 0);
    // Push clears what waited too long, so a stack nobody looks at does not fill with old news.
    NoticeStack u;
    u.Push(N(NoticeKind::Warn, "old"), 0);
    u.Push(N(NoticeKind::Warn, "at the line"), 60000);
    CHECK(u.Size() == 2);
    u.Push(N(NoticeKind::Warn, "past it"), 60001);
    CHECK(u.Size() == 2);                                    // "old" went: its wait was 60001 ms
    v = u.View(60002);
    CHECK(v.rows.size() == 2 && v.rows[0].text == "past it" && v.rows[1].text == "at the line");
    // A notice that has been shown is not touched by the wait rule: it lives its own life.
    NoticeStack w;
    Notice seen = N(NoticeKind::Error, "seen"); seen.lifeMs = 100000;
    w.Push(seen, 0);
    w.View(10);
    CHECK(w.View(70000).rows.size() == 1);                   // 70 s after its push, 60 s after it came up
    // A clock that stepped back is no wait at all.
    NoticeStack b;
    b.Push(N(NoticeKind::Warn, "back"), 90000);
    CHECK(b.View(20000).rows.size() == 1);
}

static void TestBehindTheFold() {
    // Six errors: the newest four show, two wait behind "+2 more" and have not started their life.
    NoticeStack s;
    for (uint32_t i = 1; i <= 6; ++i) s.Push(N(NoticeKind::Error, std::to_string(i).c_str()), i);
    NoticeScene v = s.View(10);
    CHECK(v.rows.size() == 4 && v.more == 2);
    v = s.View(10 + 20000);                                  // the four shown at 10 end
    CHECK(v.rows.size() == 2 && v.more == 0 && v.rows[0].text == "2" && v.rows[1].text == "1");
    CHECK(Near(v.rows[0].opacity, 0) && Near(v.rows[1].opacity, 0));                             // they come up fading in
    CHECK(s.View(10 + 20000 + 19999).rows.size() == 2);      // a full 20 s from coming up, not from the push
    CHECK(s.View(10 + 20000 + 20000).rows.empty());
    // One that waits behind the fold for more than 60 s is dropped without ever showing.
    NoticeStack w;
    w.Push(N(NoticeKind::Warn, "waits"), 0);
    for (uint32_t i = 1; i <= 4; ++i) {
        Notice e = N(NoticeKind::Error, std::to_string(i).c_str()); e.lifeMs = 100000;
        w.Push(e, i);
    }
    v = w.View(10);
    CHECK(v.rows.size() == 4 && v.more == 1);
    v = w.View(60000);                                       // waited 60000 ms: not more than 60 s yet
    CHECK(v.rows.size() == 4 && v.more == 1);
    v = w.View(60001);
    CHECK(v.rows.size() == 4 && v.more == 0);                // dropped, never shown
    CHECK(w.View(70000).more == 0);
}

static void TestKeyedAndTheClock() {
    // Not shown yet and pushed again with the same words: it waits again from the new push.
    NoticeStack s;
    s.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 0);
    s.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 50000);
    CHECK(s.Size() == 1);
    NoticeScene v = s.View(100000);                          // 100 s after the first push, 50 s after the second
    CHECK(v.rows.size() == 1 && Near(v.rows[0].opacity, 0));
    // The same with new words.
    NoticeStack t;
    t.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 0);
    t.Push(N(NoticeKind::Warn, "fps 20", "frame-rate"), 50000);
    CHECK(t.View(100000).rows.size() == 1 && t.View(100000).rows[0].text == "fps 20");
    NoticeStack t2;                                          // and it is dropped 60 s after the second push, if unseen
    t2.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 0);
    t2.Push(N(NoticeKind::Warn, "fps 20", "frame-rate"), 50000);
    CHECK(t2.View(110000).rows.size() == 1);
    NoticeStack t3;
    t3.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 0);
    t3.Push(N(NoticeKind::Warn, "fps 20", "frame-rate"), 50000);
    CHECK(t3.View(110001).rows.empty());
    // Seen, then pushed again with the same words: a new life from the push, and no new fade-in.
    NoticeStack u;
    u.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 0);
    const uint32_t serial = u.View(0).rows[0].serial;
    u.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 10000);
    v = u.View(10000);
    CHECK(v.rows.size() == 1 && v.rows[0].serial == serial && Near(v.rows[0].opacity, 1));
    CHECK(u.View(10000 + 14999).rows.size() == 1 && u.View(10000 + 15000).rows.empty());
    // Seen, then replaced with other words: the new words fade in from their own first show.
    NoticeStack w;
    w.Push(N(NoticeKind::Warn, "fps", "frame-rate"), 0);
    const uint32_t first = w.View(0).rows[0].serial;
    w.Push(N(NoticeKind::Warn, "fps 20", "frame-rate"), 10000);
    v = w.View(10500);                                       // first shown 500 ms after its push
    CHECK(v.rows[0].serial != first && Near(v.rows[0].opacity, 0));
    CHECK(Near(w.View(10600).rows[0].opacity, 0.5));
    CHECK(w.View(10500 + 14999).rows.size() == 1 && w.View(10500 + 15000).rows.empty());
    // Clear takes an unseen notice down too.
    NoticeStack c;
    c.Push(N(NoticeKind::Warn, "gone", "k"), 0);
    c.Clear("k");
    CHECK(c.View(1).rows.empty());
}

static void TestLimitInfo() {
    NoticeStack s;
    for (int i = 1; i <= 6; ++i) s.Push(N(i % 2 ? NoticeKind::Info : NoticeKind::Done, std::to_string(i).c_str()), 100 * i);
    NoticeScene v = s.View(700);
    CHECK(v.rows.size() == 4 && v.more == 0);                // the oldest Info or Done went first
    CHECK(v.rows[0].text == "6" && v.rows[1].text == "5" && v.rows[2].text == "4" && v.rows[3].text == "3");
    CHECK(s.Size() == 4);
}

static void TestLimitWarn() {
    NoticeStack s;
    for (int i = 1; i <= 6; ++i) s.Push(N(i % 2 ? NoticeKind::Warn : NoticeKind::Error, std::to_string(i).c_str()), 100 * i);
    NoticeScene v = s.View(700);
    CHECK(v.rows.size() == 4 && v.more == 2);                // warnings and errors are never dropped: "+2 more"
    CHECK(v.rows[0].text == "6" && v.rows[3].text == "3");
    // As the ones above end, the waiting ones come up (Warn 15 s, Error 20 s: the warnings end first). Their life
    // starts when they come up, not when they were pushed.
    v = s.View(700 + 14999);
    CHECK(v.rows.size() == 4 && v.more == 2 && v.rows[0].text == "6" && v.rows[3].text == "3");
    v = s.View(700 + 15000);                                  // Warn 5 and 3 end: Error 2 and Warn 1 come up
    CHECK(v.rows.size() == 4 && v.more == 0);
    CHECK(v.rows[0].text == "6" && v.rows[1].text == "4" && v.rows[2].text == "2" && v.rows[3].text == "1");
    CHECK(Near(v.rows[3].opacity, 0));                        // coming up fades in
    v = s.View(700 + 15000 + 14999);                          // Error 6 and 4 ended at 20700; Warn 1 lives its own 15 s from 15700
    CHECK(v.rows.size() == 2 && v.rows[0].text == "2" && v.rows[1].text == "1");
    v = s.View(700 + 15000 + 15000);
    CHECK(v.rows.size() == 1 && v.rows[0].text == "2");
}

static void TestLimitMixed() {
    // Three Warn and two Info: the oldest Info goes, the warnings stay.
    NoticeStack s;
    s.Push(N(NoticeKind::Info, "old info"), 1);
    s.Push(N(NoticeKind::Warn, "w1"), 2);
    s.Push(N(NoticeKind::Warn, "w2"), 3);
    s.Push(N(NoticeKind::Info, "new info"), 4);
    CHECK(s.Push(N(NoticeKind::Warn, "w3"), 5));
    NoticeScene v = s.View(6);
    CHECK(v.rows.size() == 4 && v.more == 0);
    CHECK(v.rows[0].text == "w3" && v.rows[1].text == "new info" && v.rows[2].text == "w2" && v.rows[3].text == "w1");
    // A Done is as quiet as an Info.
    NoticeStack d;
    d.Push(N(NoticeKind::Done, "done"), 1);
    for (int i = 0; i < 4; ++i) d.Push(N(NoticeKind::Warn, std::to_string(i).c_str()), 2 + uint32_t(i));
    v = d.View(10);
    CHECK(v.rows.size() == 4 && v.more == 0 && v.rows[3].text == "0");
    // Four warnings stand: a new Info is dropped (Push says so), a new Warn is kept behind them.
    NoticeStack f;
    for (int i = 0; i < 4; ++i) f.Push(N(NoticeKind::Warn, std::to_string(i).c_str()), uint32_t(i));
    CHECK(!f.Push(N(NoticeKind::Info, "news"), 10));
    CHECK(!f.Push(N(NoticeKind::Done, "done"), 11));
    v = f.View(12);
    CHECK(v.rows.size() == 4 && v.more == 0 && v.rows[0].text == "3");
    CHECK(f.Push(N(NoticeKind::Warn, "fifth"), 13));
    v = f.View(14);
    CHECK(v.rows.size() == 4 && v.more == 1 && v.rows[0].text == "fifth");
    // Ended notices do not count against the limit.
    NoticeStack e;
    for (int i = 0; i < 4; ++i) e.Push(N(NoticeKind::Info, std::to_string(i).c_str()), uint32_t(i));
    e.View(4);
    e.Push(N(NoticeKind::Info, "later"), 7000);
    v = e.View(7001);
    CHECK(v.rows.size() == 1 && v.rows[0].text == "later" && e.Size() == 1);
}

static void TestKept() {
    NoticeStack s;
    for (int i = 0; i < 40; ++i) s.Push(N(NoticeKind::Error, std::to_string(i).c_str()), uint32_t(i));
    CHECK(s.Size() == 16);
    const NoticeScene v = s.View(100);
    CHECK(v.rows.size() == 4 && v.more == 12 && v.rows[0].text == "39");
}

static void TestSanitize() {
    CHECK(SanitizeNotice("  hello   world \t\n ") == "hello world");
    CHECK(SanitizeNotice("one\ntwo\r\nthree") == "one two three");
    CHECK(SanitizeNotice(std::string("a\x01" "b\x7f" "c")) == "abc");                           // control characters go
    CHECK(SanitizeNotice("fire \xF0\x9F\x94\xA5 now") == "fire now");                           // an emoji the font cannot draw
    CHECK(SanitizeNotice("bad \xC3 byte\xFF") == "bad byte");                                    // invalid UTF-8
    CHECK(SanitizeNotice("") == "" && SanitizeNotice("   ") == "");
    CHECK(SanitizeNotice("caf\xC3\xA9") == "caf\xC3\xA9");                                       // a letter stays
    // The cap: 160 bytes for a notice, on a code point.
    const std::string longAscii(300, 'x');
    CHECK(SanitizeNotice(longAscii).size() == kNoticeMaxBytes);
    std::string two, three, four;
    for (int i = 0; i < 120; ++i) two += "\xC3\xA9";                                             // 240 bytes
    for (int i = 0; i < 80; ++i) three += "\xE2\x82\xAC";                                        // 240 bytes
    for (int i = 0; i < 60; ++i) four += "\xF0\x9D\x90\x80";                                     // 240 bytes (math letters, not emoji)
    const std::string a = SanitizeNotice(two), b = SanitizeNotice(three), c = SanitizeNotice(four);
    CHECK(a.size() == 160 && Valid8(a));                                                         // 80 letters, whole
    CHECK(b.size() == 159 && Valid8(b));                                                         // 53 letters: 160 would split one
    CHECK(c.size() == 160 && Valid8(c));
    CHECK(SanitizeNotice("x" + two).size() == 159 && Valid8(SanitizeNotice("x" + two)));         // 1 + 79 two-byte letters
    CHECK(SanitizeNotice(std::string(159, 'x') + "\xC3\xA9").size() == 159);                     // the letter would not fit
    CHECK(SanitizeNotice(std::string(158, 'x') + "\xC3\xA9").size() == 160);
    CHECK(SanitizeNotice(longAscii, 20).size() == 20);
    // A cut that lands after a space does not end in one.
    CHECK(SanitizeNotice(std::string(159, 'x') + " yyyy").back() == 'x');
    // Through the stack.
    NoticeStack s;
    s.Push(N(NoticeKind::Info, three.c_str()), 0);
    const std::string shown = s.View(1).rows[0].text;
    CHECK(shown.size() == 159 && Valid8(shown));
}

static void TestCutFitJoin() {
    CHECK(NoticeCut("short", 20) == "short");
    CHECK(NoticeCut("this is a rather long sentence", 20) == "this is a rather...");           // at a word, 3 bytes for the dots
    CHECK(NoticeCut("this is a rather long sentence", 22) == "this is a rather...");           // a word is not cut in two
    CHECK(NoticeCut("abcdefghijklmnopqrstuvwxyz", 10) == "abcdefg...");
    CHECK(NoticeCut("abcdefghij", 2) == "..");
    CHECK(NoticeCut("abcdef", 6) == "abcdef");
    std::string e;
    for (int i = 0; i < 40; ++i) e += "\xC3\xA9";
    const std::string cut = NoticeCut(e, 21);
    CHECK(cut.size() <= 21 && Valid8(cut) && cut.substr(cut.size() - 3) == "...");
    CHECK(NoticeFit(std::string(400, 'x')).size() == kNoticeMaxBytes);
    CHECK(NoticeFit("  fine \n text ") == "fine text");
    // The tail stays whole, the head gives way.
    const std::string tail = "Fix it within a minute or you leave the lobby.";
    const std::string reason(200, 'r');
    const std::string joined = NoticeJoin(reason, tail);
    CHECK(joined.size() <= kNoticeMaxBytes && joined.size() >= kNoticeMaxBytes - 8);
    CHECK(joined.substr(joined.size() - tail.size()) == tail);
    CHECK(NoticeJoin("Short.", tail) == "Short. " + tail);
    CHECK(NoticeJoin("", tail) == tail);
    CHECK(NoticeJoin("Only a head.", "") == "Only a head.");
    CHECK(NoticeJoin(std::string(300, 'h'), "").size() == kNoticeMaxBytes);
    CHECK(NoticeJoin("head", std::string(300, 't')).size() <= kNoticeMaxBytes);
    // Exactly at the cap: no dots.
    const std::string fits = std::string(kNoticeMaxBytes - 1 - tail.size(), 'r');
    CHECK(NoticeJoin(fits, tail) == fits + " " + tail);
    CHECK(NoticeJoin(fits + "r", tail).size() == kNoticeMaxBytes);
    CHECK(NoticeJoin(fits + "r", tail) != fits + "r " + tail);
}

int main() {
    TestLifeAndInk();
    TestExpiry();
    TestOrder();
    TestTitleAndText();
    TestKeyedReplace();
    TestRefreshDoesNotFlash();
    TestClear();
    TestFades();
    TestClock();
    TestLifeStartsAtFirstShow();
    TestWaitForFirstShow();
    TestBehindTheFold();
    TestKeyedAndTheClock();
    TestLimitInfo();
    TestLimitWarn();
    TestLimitMixed();
    TestKept();
    TestSanitize();
    TestCutFitJoin();
    if (g_fail) { std::printf("%d notice checks failed\n", g_fail); return 1; }
    std::printf("all notice tests passed\n");
    return 0;
}

// The log guard (M3): the session size cap with its one final line, the lines-per-second throttle, and the collapse of
// identical consecutive lines (the first two in full, a tally after), with normal logging untouched.
#include "util/hsm_logguard.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::util;

// Feeds a line the way log.cpp does: writes it if told to, counting its bytes. Returns what was written, notes included.
static std::string Feed(LogGuard& g, const std::string& msg, double nowMs, bool* wrote = nullptr) {
    const LogGuard::Verdict v = g.Filter(msg, nowMs);
    std::string out;
    if (!v.note.empty()) { out += v.note + "\n"; g.Wrote(v.note.size()); }
    if (v.write) { out += msg + "\n"; g.Wrote(msg.size()); }
    if (wrote) *wrote = v.write;
    return out;
}

static void TestNormalLogUntouched() {
    LogGuard g;
    for (int i = 0; i < 20000; ++i) {
        bool w = false;
        const std::string out = Feed(g, "host: line " + std::to_string(i), i * 10.0, &w);   // 100 a second
        CHECK(w && out == "host: line " + std::to_string(i) + "\n");
    }
    CHECK(!g.capped() && g.droppedByThrottle() == 0);
    // The same line twice in a row is still written in full; only a third time collapses.
    LogGuard h;
    bool w1, w2, w3;
    Feed(h, "frames 16.6 ms", 0, &w1); Feed(h, "frames 16.6 ms", 10, &w2); Feed(h, "frames 16.6 ms", 20, &w3);
    CHECK(w1 && w2 && !w3);
}

static void TestSizeCap() {
    LogGuard g(1000);
    std::string all;
    for (int i = 0; i < 200; ++i) {
        const std::string out = Feed(g, "line number " + std::to_string(i) + " padding padding", i * 5.0);
        all += out;
    }
    CHECK(g.capped());
    CHECK(all.find("log size limit of 0 MB reached") != std::string::npos);   // 1000 bytes: reports 0 MB
    // The notice is written exactly once, and nothing after it.
    size_t first = all.find("log size limit"), second = all.find("log size limit", first + 1);
    CHECK(first != std::string::npos && second == std::string::npos);
    CHECK(all.size() < 1000 + 200);
    CHECK(Feed(g, "after the cap", 99999.0).empty());
    LogGuard big;   // the real cap
    CHECK(kLogMaxBytes == 32u * 1024 * 1024);
    big.Wrote(kLogMaxBytes);
    CHECK(Feed(big, "x", 0).find("log size limit of 32 MB reached") != std::string::npos && Feed(big, "y", 1).empty());
}

static void TestThrottle() {
    LogGuard g;
    int written = 0;
    for (int i = 0; i < 5000; ++i) {
        bool w = false;
        Feed(g, "burst " + std::to_string(i), 100.0 + i * 0.01, &w);   // 5000 distinct lines inside 50 ms
        written += w;
    }
    CHECK(written == kLogMaxLinesPerSec && g.droppedByThrottle() == 5000 - kLogMaxLinesPerSec);
    // The next second reports the loss once and logs normally again.
    bool w = false;
    const std::string out = Feed(g, "calm again", 1500.0, &w);
    CHECK(w && out.find("4000 log lines dropped by the throttle") != std::string::npos && out.find("calm again") != std::string::npos);
    CHECK(Feed(g, "calm 2", 1600.0) == "calm 2\n");
}

static void TestRepeatCollapse() {
    LogGuard g;
    std::string all;
    for (int i = 0; i < 1000; ++i) all += Feed(g, "net: bad packet", 100.0 + i);   // the same line every ms for a second
    // Two in full, then a tally every 5 s at most: within this second none yet.
    CHECK(all == "net: bad packet\nnet: bad packet\n");
    // A different line ends the run with the tally first.
    all = Feed(g, "something else", 1200.0);
    CHECK(all == "(the line above repeated 998 more times)\nsomething else\n");
    // A run that never ends is tallied every 5 s.
    LogGuard h;
    int tallies = 0;
    for (int i = 0; i < 20000; ++i) {
        const std::string out = Feed(h, "spam", i * 1.0);   // 20 s of one line per ms... throttled? identical lines are not counted
        if (out.find("repeated") != std::string::npos) ++tallies;
    }
    CHECK(tallies >= 3 && tallies <= 5);
    CHECK(h.droppedByThrottle() == 0);   // collapsed lines never reach the throttle
    // One repeat is told in the singular.
    LogGuard s;
    Feed(s, "a", 0); Feed(s, "a", 1); Feed(s, "a", 2);
    CHECK(Feed(s, "b", 3) == "(the line above repeated 1 more time)\nb\n");
    // Non-consecutive repeats are not collapsed.
    LogGuard n;
    for (int i = 0; i < 100; ++i) { CHECK(Feed(n, "a", i * 2.0) == "a\n"); CHECK(Feed(n, "b", i * 2.0 + 1) == "b\n"); }
}

static void TestClockBackwards() {
    LogGuard g;
    for (int i = 0; i < 1500; ++i) Feed(g, "l" + std::to_string(i), 9000.0);   // fills the window at t=9000
    CHECK(Feed(g, "later", 100.0).find("later") != std::string::npos);        // the clock went back: a fresh window, not a stall
}

int main() {
    TestNormalLogUntouched();
    TestSizeCap();
    TestThrottle();
    TestRepeatCollapse();
    TestClockBackwards();
    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::printf("hsm_logguard tests passed\n");
    return 0;
}

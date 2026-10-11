// Unit tests for hsm_chat.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_chat.cpp
#include "hsm_chat.h"
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(s, i, len, cp)) return false;
        i += len;
    }
    return true;
}

static void TestRoundTrip() {
    ChatLine l; l.fromSlot = 3; l.text = "gg wp";
    auto b = Encode(l); ChatLine o;
    CHECK(Decode(b.data(), b.size(), o));
    CHECK(o.fromSlot == 3 && o.text == "gg wp");
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));          // truncation
    auto extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), o));      // trailing bytes
    auto bad = b; bad[2] = uint8_t(kProtocolRevision + 1); CHECK(!Decode(bad.data(), bad.size(), o));
    auto wrongType = b; wrongType[3] = uint8_t(MsgType::Kill); CHECK(!Decode(wrongType.data(), wrongType.size(), o));
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::Chat);
}

static void TestTruncationLoop() {
    // Every prefix of a long multi-byte line encodes to a valid line of at most kMaxChatBytes.
    std::string src;
    for (int i = 0; i < 80; ++i) src += (i % 3 == 0) ? "a" : (i % 3 == 1) ? "\xC3\xA9" : "\xE2\x82\xAC";   // a, e-acute, euro
    for (size_t n = 0; n <= src.size(); ++n) {
        std::string s = SanitizeChat(src.substr(0, n));
        CHECK(s.size() <= kMaxChatBytes);
        CHECK(ValidUtf8(s));
        CHECK(SanitizeChat(s) == s);                       // idempotent
        ChatLine l; l.fromSlot = 1; l.text = src.substr(0, n);
        auto b = Encode(l); ChatLine o;
        if (s.empty()) CHECK(!Decode(b.data(), b.size(), o));     // empty after sanitize is rejected
        else CHECK(Decode(b.data(), b.size(), o) && o.text == s);
    }
}

static void TestUtf8CutEdges() {
    // 'a' + 60 x e-acute (2 bytes each) = 121 bytes; a naive cut at 120 would split the last code point.
    std::string s = "a";
    for (int i = 0; i < 60; ++i) s += "\xC3\xA9";
    std::string r = SanitizeChat(s);
    CHECK(r.size() == 119 && ValidUtf8(r));
    // 30 x 4-byte emoji = exactly 120 bytes: kept whole.
    std::string e; for (int i = 0; i < 30; ++i) e += "\xF0\x9F\x98\x80";
    CHECK(SanitizeChat(e).size() == 120);
    // 31 emoji: cut to 30 whole code points.
    std::string e2 = e + "\xF0\x9F\x98\x80";
    CHECK(SanitizeChat(e2) == e);
    // 3-byte euro sign: 41 x 3 = 123 bytes cut to 40 code points (120 bytes).
    std::string euro; for (int i = 0; i < 41; ++i) euro += "\xE2\x82\xAC";
    CHECK(SanitizeChat(euro).size() == 120 && ValidUtf8(SanitizeChat(euro)));
    // Truncation that leaves trailing space is trimmed.
    std::string sp = std::string(119, 'x') + " yz";
    CHECK(SanitizeChat(sp) == std::string(119, 'x'));
}

static void TestInvalidUtf8() {
    CHECK(SanitizeChat("a\xFFz") == "az");                    // invalid lead byte
    CHECK(SanitizeChat("a\xC3") == "a");                      // truncated sequence at end
    CHECK(SanitizeChat("a\xE2\x82z") == "az");                // truncated 3-byte sequence mid-string
    CHECK(SanitizeChat("a\x80z") == "az");                    // stray continuation byte
    CHECK(SanitizeChat("a\xC0\xAFz") == "az");                // overlong '/'
    CHECK(SanitizeChat("a\xE0\x80\x80z") == "az");            // overlong 3-byte
    CHECK(SanitizeChat("a\xED\xA0\x80z") == "az");            // UTF-16 surrogate U+D800
    CHECK(SanitizeChat("a\xF4\x90\x80\x80z") == "az");        // above U+10FFFF
    CHECK(SanitizeChat("a\xF5\x80\x80\x80z") == "az");        // lead byte F5 never valid
    CHECK(SanitizeChat("\xC3\xA9") == "\xC3\xA9");            // valid e-acute kept
}

static void TestControlChars() {
    CHECK(SanitizeChat("a\tb\nc\rd") == "abcd");              // C0 controls
    CHECK(SanitizeChat(std::string("a\0b", 3)) == "ab");      // embedded NUL
    CHECK(SanitizeChat("a\x7Fz") == "az");                    // DEL
    CHECK(SanitizeChat("a\xC2\x85z") == "az");                // C1 NEL U+0085
    CHECK(SanitizeChat("a\xC2\x9Fz") == "az");               // C1 U+009F
    CHECK(SanitizeChat("\x1b[31mred") == "[31mred");        // ESC stripped, rest kept
    CHECK(SanitizeChat("  \t hi \n ") == "hi");               // trim
    CHECK(SanitizeChat("\x01\x02   ").empty());               // empty after sanitize
}

static void TestDecodeRejects() {
    // Too long: 121 bytes of text.
    Writer w; w.header(MsgType::Chat); w.u8(1); w.u8(121); for (int i = 0; i < 121; ++i) w.u8('a');
    ChatLine o; CHECK(!Decode(w.buf.data(), w.buf.size(), o));
    // Exactly 120 is accepted.
    Writer ok; ok.header(MsgType::Chat); ok.u8(1); ok.u8(120); for (int i = 0; i < 120; ++i) ok.u8('a');
    CHECK(Decode(ok.buf.data(), ok.buf.size(), o) && o.text.size() == 120);
    // Empty text after sanitize (control bytes only) is rejected.
    Writer c; c.header(MsgType::Chat); c.u8(1); c.u8(2); c.u8(1); c.u8(2);
    CHECK(!Decode(c.buf.data(), c.buf.size(), o));
    // Zero-length text is rejected.
    Writer z; z.header(MsgType::Chat); z.u8(1); z.u8(0);
    CHECK(!Decode(z.buf.data(), z.buf.size(), o));
    // Invalid bytes inside an otherwise valid payload are dropped, not trusted.
    Writer v; v.header(MsgType::Chat); v.u8(2); v.u8(3); v.u8('a'); v.u8(0xFF); v.u8('b');
    CHECK(Decode(v.buf.data(), v.buf.size(), o) && o.text == "ab" && o.fromSlot == 2);
}

static void TestFromSlotPreserved() {
    ChatLine l; l.fromSlot = 255; l.text = "x";
    auto b = Encode(l); ChatLine o;
    CHECK(Decode(b.data(), b.size(), o) && o.fromSlot == 255);
}

static void TestRateLimiter() {
    ChatRateLimiter rl;
    CHECK(rl.Allow(0)); CHECK(rl.Allow(0)); CHECK(rl.Allow(0));   // burst of 3
    CHECK(!rl.Allow(0));                                          // 4th denied
    CHECK(!rl.Allow(999));                                        // not yet a full second
    CHECK(rl.Allow(1000));                                        // one token refilled
    CHECK(!rl.Allow(1000));
    CHECK(rl.Allow(3000)); CHECK(rl.Allow(3000));                 // 2s since last check refills two tokens
    CHECK(!rl.Allow(3000));
    // Long idle never banks more than the burst.
    ChatRateLimiter idle;
    CHECK(idle.Allow(0));
    CHECK(idle.Allow(3600000)); CHECK(idle.Allow(3600000)); CHECK(idle.Allow(3600000));
    CHECK(!idle.Allow(3600000));
    // Wrap-safe: the uint32 millisecond counter wraps mid-window.
    ChatRateLimiter wrap;
    uint32_t t0 = std::numeric_limits<uint32_t>::max() - 500;
    CHECK(wrap.Allow(t0)); CHECK(wrap.Allow(t0)); CHECK(wrap.Allow(t0));
    CHECK(!wrap.Allow(t0));
    CHECK(!wrap.Allow(t0 + 400));                                 // 400ms elapsed, across the wrap
    CHECK(!wrap.Allow(t0 + 600));                                 // 600ms elapsed in total
    CHECK(wrap.Allow(t0 + 1000));                                 // 1000ms elapsed, wrapped to 499
    // Long-running sender: many lines spaced at exactly one refill interval are all allowed.
    ChatRateLimiter steady; uint32_t now = 0;
    CHECK(steady.Allow(now));
    for (int i = 0; i < 100; ++i) { now += kChatRefillMs; CHECK(steady.Allow(now)); }
}

static void TestChatLog() {
    ChatLog log;
    for (uint32_t i = 0; i < 10; ++i) log.Add(ChatLine{uint8_t(i), std::string("m") + char('0' + char(i))}, 0);
    CHECK(log.size() == kChatLogLines);                           // bounded to last 8
    auto live = log.Live(0);
    CHECK(live.size() == 8 && live.front().fromSlot == 2 && live.back().fromSlot == 9);
    // Expiry: lines are shown for less than 10 s.
    ChatLog e;
    e.Add(ChatLine{1, "old"}, 1000);
    e.Add(ChatLine{2, "new"}, 5000);
    CHECK(e.Live(10999).size() == 2);                             // 9999 ms old: still shown
    auto after = e.Live(11000);                                   // 10000 ms old: expired
    CHECK(after.size() == 1 && after[0].text == "new");
    CHECK(e.Live(14999).size() == 1);
    CHECK(e.Live(15000).empty());
    CHECK(e.size() == 0);
    // Wrap-safe expiry across the uint32 counter wrap.
    ChatLog w;
    uint32_t t0 = std::numeric_limits<uint32_t>::max() - 100;
    w.Add(ChatLine{4, "wrap"}, t0);
    CHECK(w.Live(t0 + 9999).size() == 1);
    CHECK(w.Live(t0 + 10000).empty());
    // Adding past capacity keeps the newest lines.
    ChatLog c;
    for (uint32_t i = 0; i < 20; ++i) c.Add(ChatLine{uint8_t(i), "x"}, 0);
    auto cl = c.Live(0);
    CHECK(cl.size() == 8 && cl.front().fromSlot == 12 && cl.back().fromSlot == 19);
    c.Clear(); CHECK(c.size() == 0);
}

int main() {
    TestRoundTrip();
    TestTruncationLoop();
    TestUtf8CutEdges();
    TestInvalidUtf8();
    TestControlChars();
    TestDecodeRejects();
    TestFromSlotPreserved();
    TestRateLimiter();
    TestChatLog();
    if (g_fail) { std::printf("test_hsm_chat: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_chat: all passed\n");
    return 0;
}

// Unit tests for hsm_chat.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_chat.cpp
#include "hsm_chat.h"
#include <cstdio>
#include <limits>
#include <random>

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

// ---------------------------------------------------------------- ChatSession: the keyboard is the chat's while typing
// docs/MATCH_ROUNDS_UI_CHAT_PLAN.md section 2: G and R opened the inventory and slow motion while the player typed.

static void TestChatKey() {
    ChatKey k = ChatKey::U;
    CHECK(ParseChatKey("enter", k) && k == ChatKey::Enter);
    CHECK(ParseChatKey("T", k) && k == ChatKey::T);
    CHECK(ParseChatKey("y", k) && k == ChatKey::Y);
    CHECK(ParseChatKey("u", k) && k == ChatKey::U);
    CHECK(ParseChatKey("ENTER", k) && k == ChatKey::Enter);
    CHECK(ParseChatKey("Enter", k) && k == ChatKey::Enter);
    for (const char* bad : {"", "x", "g", "r", "tt", "enter ", " t", "return", "esc"}) {
        k = ChatKey::Y;
        CHECK(!ParseChatKey(bad, k) && k == ChatKey::Y);   // refused, left as it was
    }
    for (ChatKey key : {ChatKey::Enter, ChatKey::T, ChatKey::Y, ChatKey::U}) {
        ChatKey back = key == ChatKey::Enter ? ChatKey::T : ChatKey::Enter;
        CHECK(ParseChatKey(ChatKeyName(key), back) && back == key);
    }
    CHECK(ChatSession().key() == ChatKey::Enter);   // the default: Enter is no fight action in the game's list
}

static void TestSessionEnter() {
    ChatSession s;
    CHECK(!s.typing() && s.draft().empty());
    // Not typing: letters, Backspace and the exits are the game's.
    for (ChatEvent e : {ChatEvent::Char, ChatEvent::Backspace, ChatEvent::Escape, ChatEvent::FocusLost, ChatEvent::HudHidden,
                        ChatEvent::Paused}) {
        ChatStep st = s.On(e, 0, "g");
        CHECK(!st.captureChanged && !st.capture && !st.send && !st.refused && !s.typing());
    }
    // Enter opens: the keyboard is taken, and the Enter is not typed.
    ChatStep st = s.On(ChatEvent::Enter, 0);
    CHECK(st.captureChanged && st.capture && !st.send && s.typing() && s.draft().empty());
    // G and R are letters of the line now.
    st = s.On(ChatEvent::Char, 10, "g");
    CHECK(!st.captureChanged && st.capture && !st.send && s.draft() == "g");
    s.On(ChatEvent::Char, 20, "r");
    s.On(ChatEvent::Char, 30, "!");
    CHECK(s.draft() == "gr!");
    // Enter sends and gives the keyboard back.
    st = s.On(ChatEvent::Enter, 40);
    CHECK(st.captureChanged && !st.capture && st.send && *st.send == "gr!" && !s.typing() && s.draft().empty());
    // With Enter as the key, a letter key is never the open key: OpenKey still opens (the caller sends it only for the
    // configured key), and while typing it is a letter.
    ChatSession o;
    st = o.On(ChatEvent::OpenKey, 0, "t");
    CHECK(st.captureChanged && st.capture && o.draft().empty());
}

static void TestSessionLetterKey() {
    ChatSession s(ChatKey::T);
    // Enter is no open key now.
    ChatStep st = s.On(ChatEvent::Enter, 0);
    CHECK(!st.captureChanged && !st.capture && !s.typing());
    // T opens and is not typed; the T pressed while typing is an ordinary letter.
    st = s.On(ChatEvent::OpenKey, 0, "t");
    CHECK(st.captureChanged && st.capture && s.typing() && s.draft().empty());
    st = s.On(ChatEvent::OpenKey, 10, "t");
    CHECK(!st.captureChanged && st.capture && s.draft() == "t");
    s.On(ChatEvent::Char, 20, "o");
    s.On(ChatEvent::OpenKey, 30, "T");
    CHECK(s.draft() == "toT");
    st = s.On(ChatEvent::Enter, 40);
    CHECK(st.captureChanged && !st.capture && st.send && *st.send == "toT");
}

static void TestSessionEmptyAndRate() {
    // Enter on an empty or blank draft only closes.
    ChatSession s;
    s.On(ChatEvent::Enter, 0);
    ChatStep st = s.On(ChatEvent::Enter, 10);
    CHECK(st.captureChanged && !st.capture && !st.send && !st.refused && !s.typing());
    s.On(ChatEvent::Enter, 20);
    s.On(ChatEvent::Char, 30, " ");
    s.On(ChatEvent::Char, 40, " ");
    st = s.On(ChatEvent::Enter, 50);
    CHECK(st.captureChanged && !st.capture && !st.send && !s.typing());
    // The line sent is sanitized (trimmed).
    s.On(ChatEvent::Enter, 60);
    for (const char* c : {" ", "h", "i", " "}) s.On(ChatEvent::Char, 70, c);
    st = s.On(ChatEvent::Enter, 80);
    CHECK(st.send && *st.send == "hi");
    // Three lines back to back, the fourth refused: the draft and the keyboard stay the chat's.
    ChatSession r;
    for (int i = 0; i < 3; ++i) {
        r.On(ChatEvent::Enter, 1000);
        r.On(ChatEvent::Char, 1000, "x");
        st = r.On(ChatEvent::Enter, 1000);
        CHECK(st.send && *st.send == "x" && !st.refused);
    }
    r.On(ChatEvent::Enter, 1000);
    r.On(ChatEvent::Char, 1000, "y");
    st = r.On(ChatEvent::Enter, 1500);
    CHECK(!st.send && st.refused && !st.captureChanged && st.capture && r.typing() && r.draft() == "y");
    // A second after the burst the kept draft goes.
    st = r.On(ChatEvent::Enter, 2000);
    CHECK(st.send && *st.send == "y" && !st.refused && st.captureChanged && !st.capture);
    // A blank Enter is no send and costs no token.
    ChatSession b;
    for (int i = 0; i < 5; ++i) { b.On(ChatEvent::Enter, 0); b.On(ChatEvent::Char, 0, " "); b.On(ChatEvent::Enter, 0); }
    for (int i = 0; i < 3; ++i) {
        b.On(ChatEvent::Enter, 0);
        b.On(ChatEvent::Char, 0, "z");
        CHECK(b.On(ChatEvent::Enter, 0).send.has_value());
    }
}

// Every way out of typing gives the keyboard back exactly once, and all but Enter drop the draft.
static void TestSessionExits() {
    const ChatEvent exits[] = {ChatEvent::Escape, ChatEvent::FocusLost, ChatEvent::HudHidden, ChatEvent::Paused,
                               ChatEvent::Enter};
    for (ChatEvent e : exits) {
        ChatSession s;
        CHECK(s.On(ChatEvent::Enter, 0).captureChanged);
        s.On(ChatEvent::Char, 10, "a");
        s.On(ChatEvent::Char, 20, "b");
        ChatStep st = s.On(e, 30);
        CHECK(st.captureChanged && !st.capture && !s.typing() && s.draft().empty() && !st.refused);
        CHECK(e == ChatEvent::Enter ? (st.send && *st.send == "ab") : !st.send);
        // Released once: the same exit again, or any other, changes nothing (an Enter would open again).
        for (ChatEvent again : exits) {
            if (again == ChatEvent::Enter) continue;
            st = s.On(again, 40);
            CHECK(!st.captureChanged && !st.capture && !s.typing());
        }
        // Opened again: the dropped draft is gone.
        s.On(ChatEvent::Enter, 50);
        CHECK(s.typing() && s.draft().empty());
    }
    for (ChatEvent e : exits) CHECK(std::string(ChatEventName(e)).size() > 0);
    CHECK(std::string(ChatEventName(ChatEvent::FocusLost)) == "focus lost");
}

static void TestSessionDraft() {
    ChatSession s;
    s.On(ChatEvent::Enter, 0);
    // Backspace takes one code point, never half of one; on an empty draft it does nothing (and closes nothing).
    s.On(ChatEvent::Char, 0, "a");
    s.On(ChatEvent::Char, 0, "\xC3\xA9");          // é
    s.On(ChatEvent::Char, 0, "\xE2\x82\xAC");      // €
    CHECK(s.draft() == "a\xC3\xA9\xE2\x82\xAC");
    s.On(ChatEvent::Backspace, 0);
    CHECK(s.draft() == "a\xC3\xA9");
    s.On(ChatEvent::Backspace, 0);
    CHECK(s.draft() == "a");
    s.On(ChatEvent::Backspace, 0);
    ChatStep st = s.On(ChatEvent::Backspace, 0);
    CHECK(s.draft().empty() && s.typing() && st.capture && !st.captureChanged);
    // Control characters, invalid UTF-8 and empty text are not typed; several code points at once are.
    for (const char* bad : {"", "\x01", "\x7F", "\n", "\xFF", "\xC3", "a\x01", "\xC2\x85"}) s.On(ChatEvent::Char, 0, bad);
    CHECK(s.draft().empty());
    s.On(ChatEvent::Char, 0, "ab");
    CHECK(s.draft() == "ab");
    // Nothing past the wire limit: a character that would not fit is dropped whole.
    ChatSession f;
    f.On(ChatEvent::Enter, 0);
    for (size_t i = 0; i + 1 < kMaxChatBytes; ++i) f.On(ChatEvent::Char, 0, "x");
    CHECK(f.draft().size() == kMaxChatBytes - 1);
    f.On(ChatEvent::Char, 0, "\xC3\xA9");
    CHECK(f.draft().size() == kMaxChatBytes - 1);
    f.On(ChatEvent::Char, 0, "y");
    CHECK(f.draft().size() == kMaxChatBytes && f.draft().back() == 'y');
    f.On(ChatEvent::Char, 0, "z");
    CHECK(f.draft().size() == kMaxChatBytes);
    st = f.On(ChatEvent::Enter, 0);
    CHECK(st.send && st.send->size() == kMaxChatBytes);
}

// Random event streams: the capture is on exactly while typing, every change is reported once, and what goes out is a
// sanitized, non-empty line within the wire limit.
static void TestSessionRandom() {
    std::mt19937 rng(1234);
    const char* texts[] = {"a", "g", "r", " ", "\xC3\xA9", "\x01", "\xFF", "t", "xyz"};
    for (int run = 0; run < 200; ++run) {
        ChatSession s(run % 2 ? ChatKey::T : ChatKey::Enter);
        bool capture = false;
        int ons = 0, offs = 0;
        uint32_t now = 0;
        for (int i = 0; i < 400; ++i) {
            now += rng() % 700;
            const ChatEvent e = ChatEvent(rng() % 8);
            const bool wasTyping = s.typing();
            const ChatStep st = s.On(e, now, texts[rng() % 9]);
            CHECK(st.capture == s.typing());
            CHECK(st.captureChanged == (wasTyping != s.typing()));
            if (st.captureChanged) { capture = st.capture; (capture ? ons : offs)++; }
            CHECK(capture == s.typing());
            if (st.send) CHECK(!st.send->empty() && *st.send == SanitizeChat(*st.send) && st.send->size() <= kMaxChatBytes);
            CHECK(!(st.send && st.refused));
            CHECK(s.draft().size() <= kMaxChatBytes && ValidUtf8(s.draft()));
            if (!s.typing()) CHECK(s.draft().empty());
        }
        CHECK(ons == offs + (s.typing() ? 1 : 0));
    }
}

// Text as the engine takes it (an FString is UTF-16): BMP code points as one unit, the rest as a surrogate pair,
// every byte that does not start a valid sequence as one U+FFFD.
static void TestUtf16() {
    CHECK(Utf16FromUtf8("").empty());
    CHECK(Utf16FromUtf8("Ann 3") == u"Ann 3");
    CHECK(Utf16FromUtf8("WAVE 4 \xC2\xB7 2") == u"WAVE 4 \u00B7 2");
    CHECK(Utf16FromUtf8("Gr\xC3\xBC\xC3\x9F" "e") == u"Gr\u00FC\u00DF" "e");
    CHECK(Utf16FromUtf8("\xE2\x82\xAC") == u"\u20AC");
    CHECK(Utf16FromUtf8("\xEF\xBF\xBF") == std::u16string(1, char16_t(0xFFFF)));
    CHECK(Utf16FromUtf8("\xF0\x9F\x98\x80!") == (std::u16string{char16_t(0xD83D), char16_t(0xDE00), u'!'}));
    CHECK(Utf16FromUtf8("\xF0\x90\x80\x80") == (std::u16string{char16_t(0xD800), char16_t(0xDC00)}));   // U+10000
    CHECK(Utf16FromUtf8("\xF4\x8F\xBF\xBF") == (std::u16string{char16_t(0xDBFF), char16_t(0xDFFF)}));   // U+10FFFF
    CHECK(Utf16FromUtf8("a\xFF" "b") == u"a\uFFFD" "b");
    CHECK(Utf16FromUtf8("\xE2\x82") == u"\uFFFD\uFFFD");             // truncated: each stray byte
    CHECK(Utf16FromUtf8("\xC0\xAF") == u"\uFFFD\uFFFD");             // overlong
    CHECK(Utf16FromUtf8("\xED\xA0\x80") == u"\uFFFD\uFFFD\uFFFD");   // an encoded surrogate
    CHECK(Utf16FromUtf8(std::string("a\0b", 3)) == std::u16string(u"a\0b", 3));   // NUL is kept; callers terminate
}

int main() {
    TestUtf16();
    TestRoundTrip();
    TestTruncationLoop();
    TestUtf8CutEdges();
    TestInvalidUtf8();
    TestControlChars();
    TestDecodeRejects();
    TestFromSlotPreserved();
    TestRateLimiter();
    TestChatLog();
    TestChatKey();
    TestSessionEnter();
    TestSessionLetterKey();
    TestSessionEmptyAndRate();
    TestSessionExits();
    TestSessionDraft();
    TestSessionRandom();
    if (g_fail) { std::printf("test_hsm_chat: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_chat: all passed\n");
    return 0;
}

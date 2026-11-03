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
    // 30 x 4-byte code points (U+10000: kept, it is no emoji) = exactly 120 bytes: kept whole.
    std::string e; for (int i = 0; i < 30; ++i) e += "\xF0\x90\x80\x80";
    CHECK(SanitizeChat(e).size() == 120);
    // 31 of them: cut to 30 whole code points. (Emoji are stripped whole, never cut: TestEmojiStripped.)
    std::string e2 = e + "\xF0\x90\x80\x80";
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

// Emoji the chat font cannot draw are stripped with the code points that dress them (#611); every other text stays.
static void TestEmojiStripped() {
    CHECK(SanitizeChat("\xF0\x9F\x98\x80\xF0\x9F\x94\xA5\xE2\x9C\x8A").empty());   // pure emoji: astral and BMP
    CHECK(SanitizeChat("gg \xF0\x9F\x98\x80 gg") == "gg  gg");                     // mixed: both spaces stay
    CHECK(SanitizeChat("\xE4\xBD\xA0\xE5\xA5\xBD caf\xC3\xA9 \xE2\x82\xAC!") ==    // CJK, accents, Latin-1: untouched
          "\xE4\xBD\xA0\xE5\xA5\xBD caf\xC3\xA9 \xE2\x82\xAC!");
    const std::string mid = SanitizeChat("a\xF0\x9F\x94\xA5z");                    // a 4-byte emoji mid-string
    CHECK(mid == "az" && ValidUtf8(mid));                                          // the byte after it is not broken
    CHECK(SanitizeChat("a \xF0\x9F\x94\xA5") == "a");                              // the trim still runs after the strip
    // The ranges: a code point on each edge, and the neighbour outside it kept.
    CHECK(SanitizeChat("\xE2\x98\x80").empty());                                   // U+2600, the symbols' bottom
    CHECK(SanitizeChat("\xE2\x97\xBF") == "\xE2\x97\xBF");                         // U+25FF below them: kept
    CHECK(SanitizeChat("\xE2\x9E\xBE").empty());                                   // U+27BE, the dingbats' top
    CHECK(SanitizeChat("\xE2\x9F\x80") == "\xE2\x9F\x80");                         // U+27C0 above them: kept
    CHECK(SanitizeChat("\xF0\x9F\x80\x80").empty());                               // U+1F000, the emoji blocks' bottom
    CHECK(SanitizeChat("\xF0\x9F\xAB\xBF").empty());                               // U+1FAFF, their top
    CHECK(SanitizeChat("\xF0\x9F\xAC\x80") == "\xF0\x9F\xAC\x80");                 // U+1FB00 above them: kept
    CHECK(SanitizeChat("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8").empty());               // a flag's regional indicators
    CHECK(SanitizeChat("\xF0\x9F\x8F\xBB").empty());                               // a skin-tone modifier
    CHECK(SanitizeChat("\xF0\x9F\x91\x8D\xEF\xB8\x8F").empty());                   // an emoji and its variation selector
    CHECK(SanitizeChat("\xEF\xB8\x8Fx") == "x");                                   // the selector alone
    CHECK(SanitizeChat("\xEF\xB8\x90") == "\xEF\xB8\x90");                         // U+FE10 is no selector: kept
    CHECK(SanitizeChat("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9").empty());   // a ZWJ pair
    CHECK(SanitizeChat("\xE2\x80\x8D").empty());                                   // the joiner alone
    CHECK(SanitizeChat("\xE2\x80\x8B") == "\xE2\x80\x8B");                         // U+200B is no joiner: kept
    // The wire: a line that is only emoji never reaches it.
    ChatLine l; l.fromSlot = 2; l.text = "\xF0\x9F\x94\xA5";
    auto b = Encode(l); ChatLine o;
    CHECK(!Decode(b.data(), b.size(), o));
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

static void TestUtf8FromUtf16() {
    const std::string cases[] = {"", "gg wp", "caf\xC3\xA9", "\xE2\x82\xAC 5", "\xE2\x99\xA5", "\xF0\x9F\x94\xA5 fire",
                                 "a\xF0\x9F\x98\x84\xF0\x9F\x98\x82z", "\xEF\xBF\xBD", "\xF4\x8F\xBF\xBF"};
    for (const std::string& c : cases) CHECK(Utf8FromUtf16(Utf16FromUtf8(c)) == c);          // round trip
    const std::u16string u = Utf16FromUtf8("x\xF0\x9F\x94\xA5y");
    CHECK(Utf16FromUtf8(Utf8FromUtf16(u)) == u);
    const char16_t pair[] = {0xD83D, 0xDD25};                                                // U+1F525
    CHECK(Utf8FromUtf16(pair, 2) == "\xF0\x9F\x94\xA5");
    const char16_t loneHi[] = {u'a', 0xD83D, u'b'}, loneLo[] = {u'a', 0xDD25, u'b'}, endHi[] = {u'a', 0xD83D};
    const char16_t swapped[] = {0xDD25, 0xD83D};
    CHECK(Utf8FromUtf16(loneHi, 3) == "a\xEF\xBF\xBD" "b");
    CHECK(Utf8FromUtf16(loneLo, 3) == "a\xEF\xBF\xBD" "b");
    CHECK(Utf8FromUtf16(endHi, 2) == "a\xEF\xBF\xBD");
    CHECK(Utf8FromUtf16(swapped, 2) == "\xEF\xBF\xBD\xEF\xBF\xBD");
    CHECK(Utf8FromUtf16(nullptr, 0).empty());
    std::mt19937 rng(7);                                                                     // any units give valid UTF-8
    for (int i = 0; i < 2000; ++i) {
        std::u16string v;
        for (int k = 0, n = int(rng() % 12); k < n; ++k) v.push_back(char16_t((rng() & 1) ? 0xD7F0 + rng() % 0x900 : rng() % 0x10000));
        CHECK(ValidUtf8(Utf8FromUtf16(v)));
    }
}

static void TestPasteText() {
    CHECK(PasteText("hello", 120) == "hello");
    CHECK(PasteText("a\r\nb", 120) == "a b");                         // CRLF is one space
    CHECK(PasteText("a\nb\rc\td", 120) == "a b c d");
    CHECK(PasteText("a\r\n\r\n\t b", 120) == "a  b");                 // a run is one space, then the typed one
    CHECK(PasteText("a\x01" "b\x7F" "c\x1B" "d", 120) == "abcd");     // controls dropped
    CHECK(PasteText("a\xC2\x85" "b\xC2\x9F" "c", 120) == "abc");      // C1 dropped
    CHECK(PasteText("a\xFF" "b\xC3" "c\xE2\x82", 120) == "abc");      // invalid and truncated sequences dropped
    CHECK(PasteText("\xF0\x9F\x94\xA5!", 120) == "\xF0\x9F\x94\xA5!");   // emoji kept
    CHECK(PasteText("", 120).empty());
    CHECK(PasteText("abc", 0).empty());
    CHECK(PasteText("abc", 2) == "ab");
    const std::string e = "\xF0\x9F\x94\xA5\xF0\x9F\x98\x82";         // two 4-byte emoji: truncation never splits one
    CHECK(PasteText(e, 8) == e);
    CHECK(PasteText(e, 7) == "\xF0\x9F\x94\xA5");
    CHECK(PasteText(e, 4) == "\xF0\x9F\x94\xA5");
    CHECK(PasteText(e, 3).empty());
    CHECK(PasteText("a\xE2\x99\xA5", 3) == "a");
    CHECK(PasteText("ab\r\ncd", 3) == "ab ");
    CHECK(PasteText(std::string(500, 'x'), kMaxChatBytes).size() == kMaxChatBytes);
    std::string many; for (int i = 0; i < 100; ++i) many += "\xE2\x99\xA5\n";
    for (size_t room = 0; room <= 130; ++room) {
        std::string p = PasteText(many, room);
        CHECK(p.size() <= room && ValidUtf8(p));
    }
    // Into a session: freeBytes, then the paste fits whole.
    ChatSession s;
    s.On(ChatEvent::Enter, 0);
    CHECK(s.freeBytes() == kMaxChatBytes);
    s.On(ChatEvent::Char, 0, "gg ");
    CHECK(s.freeBytes() == kMaxChatBytes - 3);
    s.On(ChatEvent::Char, 0, PasteText(std::string(300, 'y') + "\xF0\x9F\x94\xA5", s.freeBytes()));
    CHECK(s.draft().size() == kMaxChatBytes && s.freeBytes() == 0);
    s.On(ChatEvent::Char, 0, PasteText("more", s.freeBytes()));       // nothing left: nothing typed
    CHECK(s.draft().size() == kMaxChatBytes);
    ChatSession t;
    t.On(ChatEvent::Enter, 0);
    t.On(ChatEvent::Char, 0, std::string(118, 'a'));
    t.On(ChatEvent::Char, 0, PasteText("\xF0\x9F\x94\xA5 x\r\ny", t.freeBytes()));   // 2 bytes left: the emoji does not fit
    CHECK(t.draft().size() == 118);
    CHECK(PasteText("\xF0\x9F\x94\xA5 x\r\ny", 2).empty());
}

int main() {
    TestUtf8FromUtf16();
    TestPasteText();
    TestUtf16();
    TestRoundTrip();
    TestTruncationLoop();
    TestUtf8CutEdges();
    TestInvalidUtf8();
    TestControlChars();
    TestEmojiStripped();
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

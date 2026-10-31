// Emoji shortcodes (hsm_emoji.h): expansion, the byte limit, the /emoji list.
#include "match/hsm_emoji.h"
#include <cstdio>
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

static const char* kFire = "\xF0\x9F\x94\xA5";
static const char* kHeart = "\xE2\x99\xA5";

static void TestTable() {
    size_t n = 0;
    const EmojiEntry* t = EmojiTable(n);
    CHECK(n == 20);
    bool bmp = false, astral = false;
    for (size_t i = 0; i < n; ++i) {
        const std::string code = t[i].code, e = EmojiUtf8(t[i].cp);
        CHECK(!code.empty() && ValidUtf8(e) && !e.empty());
        CHECK(e.size() <= code.size() + 2);                       // the emoji is no longer than ":code:"
        CHECK(t[i].cp >= 0x80 && !(t[i].cp >= 0xD800 && t[i].cp <= 0xDFFF));
        (t[i].cp < 0x10000 ? bmp : astral) = true;
        for (char c : code) CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
        for (size_t j = 0; j < i; ++j) CHECK(code != t[j].code && t[i].cp != t[j].cp);   // no duplicates
        CHECK(EmojiFor(code) == e);
    }
    CHECK(bmp && astral);
    CHECK(EmojiUtf8(0x1F525) == kFire && EmojiUtf8(0x2665) == kHeart);
    CHECK(EmojiFor("nope").empty() && EmojiFor("").empty());
}

static void TestExpand() {
    CHECK(ExpandShortcodes("") == "");
    CHECK(ExpandShortcodes("gg wp") == "gg wp");
    CHECK(ExpandShortcodes(":fire:") == kFire);                                    // whole line
    CHECK(ExpandShortcodes(":fire: go") == std::string(kFire) + " go");            // at start
    CHECK(ExpandShortcodes("go :fire:") == "go " + std::string(kFire));            // at end
    CHECK(ExpandShortcodes("a:heart:b") == "a" + std::string(kHeart) + "b");       // inside a word
    CHECK(ExpandShortcodes(":fire::heart:") == std::string(kFire) + kHeart);       // adjacent
    CHECK(ExpandShortcodes(":fire::fire::fire:") == std::string(kFire) + kFire + kFire);
    CHECK(ExpandShortcodes(":fire:fire:") == std::string(kFire) + "fire:");        // the colon after is not reused
    CHECK(ExpandShortcodes(":FIRE: :Heart:") == std::string(kFire) + " " + kHeart);   // case-insensitive
    CHECK(ExpandShortcodes(":100:") == EmojiUtf8(0x1F4AF));
    // Unknown codes and lone colons stay as typed.
    CHECK(ExpandShortcodes(":nope:") == ":nope:");
    CHECK(ExpandShortcodes("::") == "::");
    CHECK(ExpandShortcodes(":") == ":");
    CHECK(ExpandShortcodes("a : b :") == "a : b :");
    CHECK(ExpandShortcodes(":fire") == ":fire");
    CHECK(ExpandShortcodes("fire:") == "fire:");
    CHECK(ExpandShortcodes(": fire :") == ": fire :");
    CHECK(ExpandShortcodes(":fi re:") == ":fi re:");
    CHECK(ExpandShortcodes("12:30:45") == "12:30:45");
    CHECK(ExpandShortcodes(":nope:fire:") == ":nope" + std::string(kFire));           // a failed code does not eat the next
    CHECK(ExpandShortcodes(":a:fire:") == ":a" + std::string(kFire));
    CHECK(ExpandShortcodes(":thumbsup:x:thumbsdown:") == EmojiUtf8(0x1F44D) + "x" + EmojiUtf8(0x1F44E));
    const std::string longWord = ":" + std::string(60, 'a') + ":";
    CHECK(ExpandShortcodes(longWord) == longWord);
    // Other text, including emoji and non-ASCII, is untouched.
    CHECK(ExpandShortcodes("caf\xC3\xA9 :fire: \xE2\x82\xAC") == "caf\xC3\xA9 " + std::string(kFire) + " \xE2\x82\xAC");
    CHECK(ExpandShortcodes(std::string(kFire) + ":fire:") == std::string(kFire) + kFire);
    CHECK(ExpandShortcodes(":\xC3\xA9:") == ":\xC3\xA9:");
}

static void TestLimit() {
    // Expansion never grows a line, so a line within the limit stays within it.
    size_t n = 0;
    const EmojiEntry* t = EmojiTable(n);
    for (size_t i = 0; i < n; ++i) {
        const std::string code = ":" + std::string(t[i].code) + ":";
        CHECK(ExpandShortcodes(code).size() <= code.size());
    }
    std::string line;
    while (line.size() + 5 <= kMaxChatBytes) line += ":joy:";                 // 120 bytes of codes
    CHECK(line.size() == kMaxChatBytes);
    const std::string out = SanitizeChat(ExpandChatLine(line));
    CHECK(out.size() == 24 * 4 && ValidUtf8(out));
    // A caller that passes more than the limit gets a cut on a code-point boundary, never a split emoji.
    std::string big;
    for (int i = 0; i < 40; ++i) big += ":fire:";                              // 240 bytes of codes, 160 expanded
    const std::string cut = SanitizeChat(ExpandShortcodes(big));
    CHECK(cut.size() == kMaxChatBytes && ValidUtf8(cut));
    std::string odd = "x";                                                     // shifts the emoji off the 4-byte grid
    for (int i = 0; i < 40; ++i) odd += ":fire:";
    const std::string cut2 = SanitizeChat(ExpandShortcodes(odd));
    CHECK(cut2.size() == 117 && ValidUtf8(cut2));
    // Commands are left as typed.
    CHECK(ExpandChatLine("/kick :fire:") == "/kick :fire:");
    CHECK(ExpandChatLine("x :fire:") == "x " + std::string(kFire));
}

static void TestCommand() {
    CHECK(IsEmojiCommand("/emoji") && IsEmojiCommand("/EMOJI") && IsEmojiCommand("  /emojis ") && IsEmojiCommand("/Emoji"));
    CHECK(!IsEmojiCommand("emoji") && !IsEmojiCommand("/emoji x") && !IsEmojiCommand("/emojix") && !IsEmojiCommand("") && !IsEmojiCommand("/"));
    for (size_t w : {size_t(0), size_t(20), size_t(64), size_t(120), size_t(500)}) {
        const std::vector<std::string> lines = EmojiListLines(w);
        CHECK(!lines.empty());
        std::string all;
        for (const std::string& l : lines) {
            CHECK(!l.empty() && l.size() <= kMaxChatBytes && ValidUtf8(l));
            CHECK(SanitizeChat(l) == l);                                       // survives ShowLine's sanitize
            all += l + " ";
        }
        size_t n = 0;
        const EmojiEntry* t = EmojiTable(n);
        for (size_t i = 0; i < n; ++i)
            CHECK(all.find(":" + std::string(t[i].code) + ": " + EmojiUtf8(t[i].cp)) != std::string::npos);   // every code listed with its emoji
    }
    for (const std::string& l : EmojiListLines(64)) CHECK(l.size() <= 64);
    CHECK(EmojiListLines().size() <= 5);                                       // fits the chat log with its header
}

static void TestRandom() {
    std::mt19937 rng(11);
    const char* bits[] = {":", ":", "fire", "heart", "100", "x", " ", "_", "\xC3\xA9", "\xF0\x9F\x94\xA5", "gg", "Rip", "::"};
    for (int i = 0; i < 5000; ++i) {
        std::string s;
        for (int k = 0, n = int(rng() % 12); k < n; ++k) s += bits[rng() % (sizeof(bits) / sizeof(bits[0]))];
        const std::string e = ExpandShortcodes(s);
        CHECK(ValidUtf8(e));
        CHECK(e.size() <= s.size());                                           // never grows
    }
}

int main() {
    TestTable();
    TestExpand();
    TestLimit();
    TestCommand();
    TestRandom();
    if (g_fail) { std::printf("test_hsm_emoji: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_emoji: all passed\n");
    return 0;
}

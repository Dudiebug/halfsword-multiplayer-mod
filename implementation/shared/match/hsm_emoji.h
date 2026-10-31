// Emoji in chat: ":shortcode:" typed in a line becomes the emoji when the line is sent (never on receive), so the
// 120-byte limit (hsm_chat.h) applies to the bytes that go out. Game-independent: no Unreal types. Whether the chat
// font can draw a given glyph is the game's; the text always reaches the widget as valid UTF-8.
//
// Shortcodes are lowercase [a-z0-9_]; matching ignores case (":Fire:" works). An unknown ":word:" and a lone colon
// stay as typed. Every emoji is no longer than its code (the test checks it), so expanding never grows a line past
// the limit; SanitizeChat still cuts on a code-point boundary if a caller passes more.
#pragma once
#include "hsm_chat.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

struct EmojiEntry { const char* code; uint32_t cp; };

// Mixes BMP symbols (heart, sword, rip: one UTF-16 unit) and astral emoji (a surrogate pair) on purpose.
inline const EmojiEntry* EmojiTable(size_t& n) {
    static const EmojiEntry t[] = {
        {"smile", 0x1F604}, {"joy", 0x1F602}, {"wink", 0x1F609}, {"cool", 0x1F60E}, {"cry", 0x1F622},
        {"angry", 0x1F620}, {"skull", 0x1F480}, {"thumbsup", 0x1F44D}, {"thumbsdown", 0x1F44E}, {"clap", 0x1F44F},
        {"pray", 0x1F64F}, {"fire", 0x1F525}, {"heart", 0x2665}, {"100", 0x1F4AF}, {"eyes", 0x1F440},
        {"crown", 0x1F451}, {"sword", 0x2694}, {"shield", 0x1F6E1}, {"gg", 0x1F91D}, {"rip", 0x2620},
    };
    n = sizeof(t) / sizeof(t[0]);
    return t;
}

inline std::string EmojiUtf8(uint32_t cp) {
    const char16_t u[2] = { char16_t(cp < 0x10000 ? cp : 0xD800 + ((cp - 0x10000) >> 10)), char16_t(0xDC00 + ((cp - 0x10000) & 0x3FF)) };
    return Utf8FromUtf16(u, cp < 0x10000 ? 1 : 2);
}

// The emoji for a code without its colons (any case), or "" when unknown.
inline std::string EmojiFor(const std::string& code) {
    std::string low;
    for (char c : code) low += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    size_t n = 0;
    const EmojiEntry* t = EmojiTable(n);
    for (size_t i = 0; i < n; ++i)
        if (low == t[i].code) return EmojiUtf8(t[i].cp);
    return std::string();
}

// Replaces every known ":code:" in text. Other text, bytes included, is copied untouched.
inline std::string ExpandShortcodes(const std::string& text) {
    constexpr size_t kLongestCode = 16;
    auto word = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ':') {
            size_t j = i + 1;
            while (j < text.size() && j - i - 1 <= kLongestCode && word(text[j])) ++j;
            if (j < text.size() && text[j] == ':' && j > i + 1) {
                const std::string e = EmojiFor(text.substr(i + 1, j - i - 1));
                if (!e.empty()) { out += e; i = j + 1; continue; }
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

// What the chat does to a typed line before it goes out. Command lines ("/kick :fire:") are left as typed.
inline std::string ExpandChatLine(const std::string& text) {
    return !text.empty() && text[0] == '/' ? text : ExpandShortcodes(text);
}

// "/emoji" (or "/emojis"), any case, with nothing after it: lists the codes locally; never sent, never a moderation command.
inline bool IsEmojiCommand(const std::string& line) {
    size_t b = 0, e = line.size();
    while (b < e && line[b] == ' ') ++b;
    while (e > b && line[e - 1] == ' ') --e;
    std::string low;
    for (size_t i = b; i < e; ++i) low += (line[i] >= 'A' && line[i] <= 'Z') ? char(line[i] - 'A' + 'a') : line[i];
    return low == "/emoji" || low == "/emojis";
}

// The list for /emoji: ":smile: 😄  :joy: 😂  ..." in lines of at most maxBytes (each fits the 120-byte chat limit).
inline std::vector<std::string> EmojiListLines(size_t maxBytes = 64) {
    if (maxBytes > kMaxChatBytes) maxBytes = kMaxChatBytes;
    std::vector<std::string> lines;
    std::string cur;
    size_t n = 0;
    const EmojiEntry* t = EmojiTable(n);
    for (size_t i = 0; i < n; ++i) {
        const std::string item = ":" + std::string(t[i].code) + ": " + EmojiUtf8(t[i].cp);
        if (!cur.empty() && cur.size() + 2 + item.size() > maxBytes) { lines.push_back(cur); cur.clear(); }
        if (!cur.empty()) cur += "  ";
        cur += item;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

}  // namespace hsm

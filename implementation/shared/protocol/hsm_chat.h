// In-game text chat (MsgType::Chat, reliable, both ways; the host relays). Game-independent: no Unreal types.
// Every line is sanitized before it is sent or shown: control characters and invalid UTF-8 are dropped, the text is
// trimmed, and it is truncated to kMaxChatBytes on a code-point boundary. Decode validates the same way and returns
// false on malformed input instead of trusting the sender. The host overwrites fromSlot with the sender's real slot.
#pragma once
#include "hsm_protocol.h"
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kMaxChatBytes = 120;
constexpr uint32_t kChatBurst = 3;          // token bucket capacity (lines sendable back to back)
constexpr uint32_t kChatRefillMs = 1000;    // one token every second
constexpr uint32_t kChatLogLines = 8;       // lines the log keeps
constexpr uint32_t kChatExpireMs = 10000;   // overlay shows a line this long

// fromSlot of a line the host itself writes as the server (kick/ban/vote notices, a role=server console "say"), shown
// under the name SERVER. The host overwrites a client's fromSlot, so no client can send one.
constexpr uint8_t kChatSystemSlot = 0xFF;

struct ChatLine {
    uint8_t fromSlot = 0;
    std::string text;
};

// Decodes one UTF-8 sequence at s[i]. Returns false for an invalid or truncated sequence (overlongs, surrogates
// and code points above U+10FFFF are invalid). On success sets len (bytes) and cp.
inline bool ChatUtf8Next(const std::string& s, size_t i, size_t& len, uint32_t& cp) {
    const size_t n = s.size();
    auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
    auto cont = [&](size_t k) { return k < n && (b(k) & 0xC0) == 0x80; };
    uint8_t c = b(i);
    if (c < 0x80) { len = 1; cp = c; return true; }
    if (c >= 0xC2 && c <= 0xDF) {
        if (!cont(i + 1)) return false;
        len = 2; cp = (uint32_t(c & 0x1F) << 6) | (b(i + 1) & 0x3F); return true;
    }
    if (c >= 0xE0 && c <= 0xEF) {
        if (!cont(i + 1) || !cont(i + 2)) return false;
        uint8_t lo = (c == 0xE0) ? 0xA0 : 0x80, hi = (c == 0xED) ? 0x9F : 0xBF;   // no overlong, no surrogates
        if (b(i + 1) < lo || b(i + 1) > hi) return false;
        len = 3; cp = (uint32_t(c & 0x0F) << 12) | (uint32_t(b(i + 1) & 0x3F) << 6) | (b(i + 2) & 0x3F); return true;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return false;
        uint8_t lo = (c == 0xF0) ? 0x90 : 0x80, hi = (c == 0xF4) ? 0x8F : 0xBF;   // no overlong, <= U+10FFFF
        if (b(i + 1) < lo || b(i + 1) > hi) return false;
        len = 4;
        cp = (uint32_t(c & 0x07) << 18) | (uint32_t(b(i + 1) & 0x3F) << 12) | (uint32_t(b(i + 2) & 0x3F) << 6) | (b(i + 3) & 0x3F);
        return true;
    }
    return false;
}

// UTF-8 as the engine takes text (an FString is UTF-16): code points above U+FFFF become a surrogate pair, and
// every byte that does not start a valid sequence becomes one U+FFFD.
inline std::u16string Utf16FromUtf8(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(s, i, len, cp)) { out.push_back(char16_t(0xFFFD)); ++i; continue; }
        i += len;
        if (cp < 0x10000) { out.push_back(char16_t(cp)); continue; }
        cp -= 0x10000;
        out.push_back(char16_t(0xD800 + (cp >> 10)));
        out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    }
    return out;
}

// Strips control characters (C0, DEL, C1) and invalid UTF-8, trims ASCII spaces, truncates to kMaxChatBytes on a
// code-point boundary. Output is always valid UTF-8 and at most kMaxChatBytes long; it may be empty.
inline std::string SanitizeChat(const std::string& in) {
    std::string out;
    out.reserve(in.size() < kMaxChatBytes ? in.size() : kMaxChatBytes + 4);
    for (size_t i = 0; i < in.size();) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(in, i, len, cp)) { ++i; continue; }          // drop the bad byte, resync on the next
        bool control = cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
        if (!control) out.append(in, i, len);
        i += len;
    }
    size_t b = 0, e = out.size();
    while (b < e && out[b] == ' ') ++b;
    while (e > b && out[e - 1] == ' ') --e;
    out = out.substr(b, e - b);
    if (out.size() > kMaxChatBytes) {
        size_t cut = kMaxChatBytes;
        while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0) == 0x80) --cut;   // back up to a lead byte
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out;
}

inline std::vector<uint8_t> Encode(const ChatLine& m) {
    Writer w; w.header(MsgType::Chat); w.u8(m.fromSlot);
    std::string text = SanitizeChat(m.text);
    w.str(text, kMaxChatBytes); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ChatLine& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Chat) return false;
    m.fromSlot = r.u8();
    std::string text = r.str(kMaxChatBytes);   // rejects lengths over kMaxChatBytes
    if (!r.done()) return false;               // rejects trailing bytes
    std::string clean = SanitizeChat(text);
    if (clean.empty()) return false;           // empty after sanitize
    m.text = clean;
    return true;
}

// Per-sender token bucket: kChatBurst lines at once, then one line per kChatRefillMs. Times are wrap-safe uint32 ms
// (elapsed time is computed with unsigned subtraction, so a 49-day counter wrap does not stall or flood).
class ChatRateLimiter {
public:
    bool Allow(uint32_t nowMs) {
        if (!started_) { started_ = true; last_ = nowMs; tokens_ = kChatBurst * kChatRefillMs; }
        uint32_t elapsed = nowMs - last_;          // wrap-safe
        last_ = nowMs;
        uint64_t cap = uint64_t(kChatBurst) * kChatRefillMs;
        uint64_t t = uint64_t(tokens_) + elapsed;
        tokens_ = uint32_t(t > cap ? cap : t);
        if (tokens_ < kChatRefillMs) return false;
        tokens_ -= kChatRefillMs;
        return true;
    }
    uint32_t tokensMilli() const { return tokens_; }   // for tests; 1000 = one line
private:
    bool started_ = false;
    uint32_t last_ = 0;
    uint32_t tokens_ = 0;
};

// Bounded log of the last kChatLogLines lines. Lines are shown for kChatExpireMs after they were added.
class ChatLog {
public:
    struct Entry { ChatLine line; uint32_t atMs = 0; };
    void Add(const ChatLine& line, uint32_t nowMs) {
        entries_.push_back(Entry{line, nowMs});
        while (entries_.size() > kChatLogLines) entries_.erase(entries_.begin());
    }
    // Lines still inside the display window, oldest first. Expired entries are dropped.
    std::vector<ChatLine> Live(uint32_t nowMs) {
        std::vector<ChatLine> out;
        std::vector<Entry> kept;
        for (const Entry& e : entries_) {
            if (uint32_t(nowMs - e.atMs) < kChatExpireMs) { kept.push_back(e); out.push_back(e.line); }
        }
        entries_.swap(kept);
        return out;
    }
    size_t size() const { return entries_.size(); }
    void Clear() { entries_.clear(); }
private:
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------- typing: the keyboard is the chat's
// docs/MATCH_ROUNDS_UI_CHAT_PLAN.md section 2. The overlay polls the OS key state, but the game's own input kept firing
// while the player typed: G opened the inventory, R slowed time, Esc paused. While a line is being typed the client
// takes the keyboard from the game (UI-only input mode, as the game's own menus do) and gives it back on every way out.
// ChatSession decides when; the client only polls keys into events and applies the capture it reports.

// The key that opens the chat (HSMMod.ini [net] chat_key, -hsmchatkey=): Enter by default, as no fight action is bound to
// it; T is very likely the game's Talk.
enum class ChatKey : uint8_t { Enter, T, Y, U };

inline const char* ChatKeyName(ChatKey k) {
    switch (k) {
        case ChatKey::Enter: return "enter";
        case ChatKey::T: return "t";
        case ChatKey::Y: return "y";
        case ChatKey::U: return "u";
    }
    return "enter";
}

// "enter", "t", "y" or "u", any case. Anything else is refused and out is left as it was.
inline bool ParseChatKey(const std::string& s, ChatKey& out) {
    std::string low;
    for (char c : s) low += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    for (ChatKey k : {ChatKey::Enter, ChatKey::T, ChatKey::Y, ChatKey::U})
        if (low == ChatKeyName(k)) { out = k; return true; }
    return false;
}

// OpenKey: the configured letter key (its text along); Char: any other key's text. FocusLost, HudHidden and Paused may
// be sent every frame they hold.
enum class ChatEvent : uint8_t { OpenKey, Enter, Escape, FocusLost, HudHidden, Paused, Char, Backspace };

inline const char* ChatEventName(ChatEvent e) {
    switch (e) {
        case ChatEvent::OpenKey: return "open key";
        case ChatEvent::Enter: return "enter";
        case ChatEvent::Escape: return "escape";
        case ChatEvent::FocusLost: return "focus lost";
        case ChatEvent::HudHidden: return "hud hidden";
        case ChatEvent::Paused: return "paused";
        case ChatEvent::Char: return "char";
        case ChatEvent::Backspace: return "backspace";
    }
    return "?";
}

struct ChatStep {
    bool captureChanged = false;            // this event took the keyboard or gave it back
    bool capture = false;                   // the keyboard is the chat's after it (exactly while typing)
    bool refused = false;                   // the rate limiter refused the line: the draft and the keyboard stay
    std::optional<std::string> send;        // a line to send: sanitized, non-empty
};

// Not typing: Enter (when it is the key) or OpenKey starts a line and takes the keyboard; the key that opened it is not
// typed; everything else is the game's. Typing: OpenKey and Char type their text (whole, or not at all when it holds a
// control character, invalid UTF-8 or would pass kMaxChatBytes), Backspace takes the last code point, Enter sends the
// sanitized draft (an empty or blank one only closes and costs no token), and Escape, FocusLost, HudHidden and Paused
// drop the draft. Every close gives the keyboard back, once.
class ChatSession {
public:
    explicit ChatSession(ChatKey key = ChatKey::Enter) : key_(key) {}

    ChatStep On(ChatEvent e, uint32_t nowMs, const std::string& text = std::string()) {
        ChatStep st;
        if (!typing_) {
            if (e == ChatEvent::OpenKey || (e == ChatEvent::Enter && key_ == ChatKey::Enter)) {
                typing_ = true;
                draft_.clear();
                st.captureChanged = true;
            }
            st.capture = typing_;
            return st;
        }
        switch (e) {
            case ChatEvent::OpenKey:
            case ChatEvent::Char:
                if (Typable(text) && draft_.size() + text.size() <= kMaxChatBytes) draft_ += text;
                break;
            case ChatEvent::Backspace:
                if (!draft_.empty()) {
                    size_t i = draft_.size() - 1;
                    while (i > 0 && (static_cast<uint8_t>(draft_[i]) & 0xC0) == 0x80) --i;   // back to the lead byte
                    draft_.resize(i);
                }
                break;
            case ChatEvent::Enter: {
                std::string line = SanitizeChat(draft_);
                if (!line.empty()) {
                    if (!limiter_.Allow(nowMs)) { st.refused = true; break; }
                    st.send = std::move(line);
                }
                Close(st);
                break;
            }
            case ChatEvent::Escape:
            case ChatEvent::FocusLost:
            case ChatEvent::HudHidden:
            case ChatEvent::Paused:
                Close(st);
                break;
        }
        st.capture = typing_;
        return st;
    }

    bool typing() const { return typing_; }
    const std::string& draft() const { return draft_; }
    ChatKey key() const { return key_; }

private:
    // Non-empty valid UTF-8 without a control character (C0, DEL, C1).
    static bool Typable(const std::string& s) {
        if (s.empty()) return false;
        for (size_t i = 0; i < s.size();) {
            size_t len = 0;
            uint32_t cp = 0;
            if (!ChatUtf8Next(s, i, len, cp) || cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) return false;
            i += len;
        }
        return true;
    }
    void Close(ChatStep& st) {
        typing_ = false;
        draft_.clear();
        st.captureChanged = true;
    }

    ChatKey key_;
    bool typing_ = false;
    std::string draft_;
    ChatRateLimiter limiter_;
};

}  // namespace hsm

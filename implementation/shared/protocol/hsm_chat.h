// In-game text chat (MsgType::Chat, reliable, both ways; the host relays). Game-independent: no Unreal types.
// Every line is sanitized before it is sent or shown: control characters and invalid UTF-8 are dropped, the text is
// trimmed, and it is truncated to kMaxChatBytes on a code-point boundary. Decode validates the same way and returns
// false on malformed input instead of trusting the sender. The host overwrites fromSlot with the sender's real slot.
#pragma once
#include "hsm_protocol.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hsm {

constexpr size_t kMaxChatBytes = 120;
constexpr uint32_t kChatBurst = 3;          // token bucket capacity (lines sendable back to back)
constexpr uint32_t kChatRefillMs = 1000;    // one token every second
constexpr uint32_t kChatLogLines = 8;       // lines the log keeps
constexpr uint32_t kChatExpireMs = 10000;   // overlay shows a line this long

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

}  // namespace hsm

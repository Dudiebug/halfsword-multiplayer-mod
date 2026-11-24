// Notices (UI overhaul PR2): the mod's own messages as a short stack of notifications in the top right, newest on top,
// instead of lines in the chat. A notice is a kind (its edge colour and how long it stays), a short title and a text.
// A keyed notice replaces the one with the same key, so a warning that repeats stays one; Clear(key) takes one down
// early once it is settled. The stack is plain data: ui/notices.cpp keeps one, asks View for the scene every tick and
// maps it onto widgets. Game-independent: no Unreal types, and the clock is the caller's (wrap-safe).
//
// Life: Info and Done 6 s, Warn 15 s, Error 20 s. A notice fades in over 200 ms and out over its last second. The
// life starts when the notice is first on screen, not when it is pushed: View is the call that draws, so the first View
// that puts a notice among the rows is its start (a notice pushed before the HUD is up, or one waiting behind "+N more",
// has not used up any of its life). A notice that waits unshown for more than kNoticeWaitMs is dropped: the player is
// past it by then (a warning that never found a screen, or one behind the fold the whole time).
// Limit: four are shown. Past four, the oldest Info or Done goes first; if the stack holds only Warn and Error, the
// extra ones wait behind a "+N more" row and come up as the ones above them end. When four Warn or Error already stand,
// a new Info or Done is the oldest Info or Done there is, so it is dropped (Push says so): quiet news never pushes out
// a warning.
// Text: notices are local, so the 120-byte chat cap does not apply. A fixed line stays within kNoticeFixedBytes (the
// catalogue, hsm_notice_text.h); a line with a name or a value in it within kNoticeMaxBytes, cut on a code point.
#pragma once
#include "match/hsm_chat_scene.h"
#include "match/hsm_hud_style.h"
#include "hsm_chat.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

enum class NoticeKind : uint8_t { Info, Done, Warn, Error };

inline constexpr uint32_t kNoticeFadeInMs = 200;
inline constexpr uint32_t kNoticeFadeOutMs = 1000;
inline constexpr uint32_t kNoticeWaitMs = 60000;   // a notice never shown is dropped once it has waited longer than this
inline constexpr size_t kNoticeShown = 4;          // rows on screen; the rest are "+N more"
inline constexpr size_t kNoticeKept = 16;          // rows the stack keeps at most (a flood of warnings stays bounded)
inline constexpr size_t kNoticeMaxBytes = 160;     // any notice text: two lines of the 440 px notice, with room
inline constexpr size_t kNoticeFixedBytes = 96;    // a fixed (catalogue) line: about two lines of 45 characters
inline constexpr size_t kNoticeTitleBytes = 20;
inline constexpr size_t kNoticeKeyBytes = 48;

inline constexpr uint32_t NoticeLifeMs(NoticeKind k) {
    switch (k) {
        case NoticeKind::Info: return 6000;
        case NoticeKind::Done: return 6000;
        case NoticeKind::Warn: return 15000;
        case NoticeKind::Error: return 20000;
    }
    return 6000;
}

// The edge and the title: iron for news, green for done, amber for a warning, red for an error.
inline Rgba NoticeInk(NoticeKind k) {
    switch (k) {
        case NoticeKind::Info: return style::kIron;
        case NoticeKind::Done: return style::kOk;
        case NoticeKind::Warn: return style::kWarn;
        case NoticeKind::Error: return style::kBloodInk;
    }
    return style::kIron;
}

// Info and Done are the news a full stack may drop; Warn and Error stay until their time is up.
inline constexpr bool NoticeQuiet(NoticeKind k) { return k == NoticeKind::Info || k == NoticeKind::Done; }

// Text made fit to show: control characters (a tab, CR or LF becomes one space), invalid UTF-8 and the emoji the font
// cannot draw are dropped, runs of spaces collapse, the ends are trimmed, and it is cut to at most `cap` bytes on a code
// point. Always valid UTF-8; may be empty.
inline std::string SanitizeNotice(const std::string& in, size_t cap = kNoticeMaxBytes) {
    std::string out;
    out.reserve(std::min(in.size(), cap + 4));
    for (size_t i = 0; i < in.size() && out.size() <= cap + 4;) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(in, i, len, cp)) { ++i; continue; }
        const size_t at = i;
        i += len;
        if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            continue;
        }
        const bool control = cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
        if (!control && !ChatEmojiCp(cp)) out.append(in, at, len);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > cap) {
        size_t cut = cap;
        while (cut > 0 && (static_cast<uint8_t>(out[cut]) & 0xC0) == 0x80) --cut;   // back up to a lead byte
        out.resize(cut);
        while (!out.empty() && out.back() == ' ') out.pop_back();
    }
    return out;
}

// `s` (already clean) cut to at most `room` bytes, ending in "..." when something was cut: at a word if one is near,
// else at a code point. Unchanged when it fits.
inline std::string NoticeCut(const std::string& s, size_t room) {
    if (s.size() <= room) return s;
    if (room <= 3) return std::string(room, '.');
    size_t cut = room - 3;
    while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) --cut;
    for (size_t k = cut; k > 0 && cut - k < 24; --k)
        if (s[k - 1] == ' ') { cut = k - 1; break; }
    std::string out = s.substr(0, cut);
    while (!out.empty() && (out.back() == ' ' || out.back() == ',' || out.back() == ';' || out.back() == ':')) out.pop_back();
    return out + "...";
}

// A dynamic line (a name, a value) made fit: clean, and cut with "..." when it is over `cap`.
inline std::string NoticeFit(const std::string& text, size_t cap = kNoticeMaxBytes) {
    return NoticeCut(SanitizeNotice(text, 4 * kNoticeMaxBytes), cap);
}

// `head` and then `tail` in one line within `cap` bytes. The tail is what the player has to do, so it stays whole; the
// head (a reason that varies) gives way.
inline std::string NoticeJoin(const std::string& head, const std::string& tail, size_t cap = kNoticeMaxBytes) {
    const std::string t = SanitizeNotice(tail, cap);
    const std::string h = SanitizeNotice(head, 4 * kNoticeMaxBytes);
    if (t.empty()) return NoticeCut(h, cap);
    if (h.empty()) return t;
    if (t.size() + 1 >= cap) return NoticeCut(t, cap);
    return NoticeCut(h, cap - 1 - t.size()) + " " + t;
}

struct Notice {
    NoticeKind kind = NoticeKind::Info;
    std::string title, text, key;     // key: "" for one that never replaces another
    uint32_t atMs = 0;                // set by Push; the start of the wait for a first show
    uint32_t lifeMs = 0;              // 0: by kind
};

struct NoticeView {
    NoticeKind kind = NoticeKind::Info;
    std::string title, text;
    Rgba ink = style::kIron;
    float opacity = 1;                // fade in and fade out together
    uint32_t serial = 0;              // changes when the content does, so a widget sets its text only then
};
struct NoticeScene { std::vector<NoticeView> rows; size_t more = 0; };   // rows newest first; more: the "+N more" row

class NoticeStack {
public:
    // Adds a notice (`nowMs` is when its wait for a first show starts). A key replaces the notice with the same key and
    // brings it to the top with a new life; when the kind, title and text are the same too and it is already on screen it
    // does not fade in again. false: it was not kept (no text, or a full stack of warnings and errors dropped it).
    bool Push(Notice n, uint32_t nowMs) {
        n.title = SanitizeNotice(n.title, kNoticeTitleBytes);
        for (char& c : n.title) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        n.text = SanitizeNotice(n.text);
        n.key = SanitizeNotice(n.key, kNoticeKeyBytes);
        if (n.text.empty()) return false;
        n.atMs = nowMs;
        items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& it) { return Over(it, nowMs); }), items_.end());
        Item fresh{n, ++serial_, nowMs, nowMs, false};
        if (!n.key.empty()) {
            const auto old = std::find_if(items_.begin(), items_.end(), [&](const Item& it) { return it.n.key == n.key; });
            if (old != items_.end()) {
                if (old->n.kind == n.kind && old->n.title == n.title && old->n.text == n.text) {
                    fresh.serial = old->serial;                       // the same words: the same row
                    // On screen: no new fade-in, and a new life from now. Not shown yet: it waits again from now.
                    if (old->shown) { fresh.bornMs = old->bornMs; fresh.shown = true; }
                }
                items_.erase(old);
            }
        }
        items_.insert(items_.begin(), std::move(fresh));
        bool kept = true;
        while (items_.size() > kNoticeShown) {
            size_t i = items_.size();
            while (i > 0 && !NoticeQuiet(items_[i - 1].n.kind)) --i;       // the oldest Info or Done
            if (i == 0) break;
            if (i == 1) kept = false;                                       // ... and it is this one
            items_.erase(items_.begin() + std::ptrdiff_t(i - 1));
        }
        if (items_.size() > kNoticeKept) items_.resize(kNoticeKept);
        return kept;
    }

    // Takes the notice with this key down now (nothing happens for an unknown key, and the empty key is no key).
    void Clear(std::string_view key) {
        if (key.empty()) return;
        items_.erase(std::remove_if(items_.begin(), items_.end(), [&](const Item& it) { return it.n.key == key; }), items_.end());
    }
    void ClearAll() { items_.clear(); }

    // The scene to draw at `nowMs`. Call it only when the notices are being drawn: a notice that gets a row here is
    // marked as shown, and its life (and its fade-in) starts at that first View. A live notice past the fourth row stays
    // unshown (counted in `more`) and keeps waiting.
    NoticeScene View(uint32_t nowMs) {
        NoticeScene s;
        size_t live = 0;
        for (Item& it : items_) {
            if (Over(it, nowMs)) continue;
            ++live;
            if (s.rows.size() >= kNoticeShown) continue;
            if (!it.shown) { it.shown = true; it.bornMs = it.lifeFromMs = nowMs; }
            const uint32_t age = AgeMs(it.lifeFromMs, nowMs);
            const float in = std::min(1.f, float(AgeMs(it.bornMs, nowMs)) / float(kNoticeFadeInMs));
            const float out = FadeOut(age, Life(it.n), kNoticeFadeOutMs);
            s.rows.push_back({it.n.kind, it.n.title, it.n.text, NoticeInk(it.n.kind), std::min(in, out), it.serial});
        }
        s.more = live - s.rows.size();
        return s;
    }

    size_t Size() const { return items_.size(); }

private:
    // shown: has a View put it on screen yet. bornMs is the start of the fade-in (the first show; kept when the same
    // notice is pushed again), lifeFromMs the start of the life (the first show, or the latest push of a notice already
    // seen).
    struct Item { Notice n; uint32_t serial = 0, bornMs = 0, lifeFromMs = 0; bool shown = false; };
    static uint32_t Life(const Notice& n) { return n.lifeMs ? n.lifeMs : NoticeLifeMs(n.kind); }
    static bool Over(const Item& it, uint32_t nowMs) {
        return it.shown ? AgeMs(it.lifeFromMs, nowMs) >= Life(it.n) : AgeMs(it.n.atMs, nowMs) > kNoticeWaitMs;
    }
    std::vector<Item> items_;       // newest first
    uint32_t serial_ = 0;
};

}  // namespace hsm

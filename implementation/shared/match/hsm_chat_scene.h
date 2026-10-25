// Chat look (MATCH_ROUNDS_UI_CHAT_PLAN 3.3): what the chat overlay shows, as data. Bottom left, a smoke panel of the
// last lines, each fading over the last second of its ten; the sender in their team's colour, the text in bone. While
// typing: the kept history unfaded, then the input row with its blinking caret and the hint (or "slow down" for a
// second after the rate limiter refused a line). For the first 20 s of the match HUD, "Enter to chat". The overlay
// (ui/chat.cpp) keeps the state in a ChatLocal and maps the scene onto widgets.
#pragma once
#include "match/hsm_hud_scene.h"
#include "hsm_chat.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

inline constexpr uint32_t kChatFadeMs = 1000;     // a line fades over the last second it shows (kChatExpireMs)
inline constexpr uint32_t kCaretBlinkMs = 530;    // the caret is on this long, then off this long
inline constexpr uint32_t kChatHintMs = 20000;    // "Enter to chat" this long after the match HUD comes up
inline constexpr uint32_t kChatSlowMs = 1000;     // "slow down" this long after a refused line

// 1 until `fadeMs` before `endMs`, then linear to 0 at `endMs`, 0 after. A fade longer than the life never reaches 1.
inline float FadeOut(uint32_t ageMs, uint32_t endMs, uint32_t fadeMs) {
    if (ageMs >= endMs) return 0;
    if (!fadeMs) return 1;
    return std::min(1.f, float(endMs - ageMs) / float(fadeMs));
}

// The caret's blink, phased from when the line was opened so it is on the moment typing starts.
inline bool CaretOn(uint32_t openedAtMs, uint32_t nowMs) { return (AgeMs(openedAtMs, nowMs) / kCaretBlinkMs) % 2 == 0; }

inline const char* ChatKeyLabel(ChatKey k) {
    switch (k) {
        case ChatKey::T: return "T";
        case ChatKey::Y: return "Y";
        case ChatKey::U: return "U";
        case ChatKey::Enter: break;
    }
    return "Enter";
}

// A sender's name colour: their team's in the team formats, else iron (so the name stands apart from the bone text).
inline Rgba ChatNameInk(const MatchInfo& m, uint8_t entity) {
    Format f = Format(m.format);
    const FighterEntry* e = FighterOf(m, entity);
    if (!e || (f != Format::TwoTeams && f != Format::ThreeTeams) || e->team < 0 || e->team >= TeamCount(f)) return style::kIron;
    return style::TeamInk(e->team);
}

struct ChatEntry { std::string name; Rgba ink = style::kIron; std::string text; uint32_t atMs = 0; };

struct ChatLocal {
    bool hud = false;                // the match HUD shows: the chat runs only then
    uint32_t hudAtMs = 0;            // when it came up (the idle hint's start)
    ChatKey key = ChatKey::Enter;
    std::vector<ChatEntry> lines;    // oldest first; the newest kChatLogLines are used
    bool typing = false;
    uint32_t typingAtMs = 0;         // when the line was opened (the caret's phase)
    std::string draft;
    bool refused = false;            // the last Enter was refused by the rate limiter...
    uint32_t refusedAtMs = 0;        // ...at this time
};

struct ChatLineView { Span name, text; float opacity = 1; };

struct ChatScene {
    bool visible = false;
    float panel = 0;                     // the log panel's strength: 0 hides it
    std::vector<ChatLineView> lines;     // oldest first
    bool input = false;                  // the input row
    std::string draft;
    bool caret = false;
    Span hint;                           // beside the draft: the keys, or "slow down"
    SceneText idle;                      // "Enter to chat"
};

inline ChatScene BuildChatScene(const ChatLocal& c, uint32_t nowMs) {
    ChatScene s;
    if (!c.hud) return s;
    s.visible = true;
    size_t first = c.lines.size() > kChatLogLines ? c.lines.size() - kChatLogLines : 0;
    for (size_t i = first; i < c.lines.size(); ++i) {
        const ChatEntry& e = c.lines[i];
        uint32_t age = AgeMs(e.atMs, nowMs);
        if (!c.typing && age >= kChatExpireMs) continue;
        float opacity = c.typing ? 1.f : FadeOut(age, kChatExpireMs, kChatFadeMs);
        s.lines.push_back({{(e.name.empty() ? "?" : e.name) + ":", e.ink}, {e.text, style::kBone}, opacity});
        s.panel = std::max(s.panel, opacity);
    }
    if (c.typing) {
        s.input = true;
        s.draft = c.draft;
        s.caret = CaretOn(c.typingAtMs, nowMs);
        s.hint = c.refused && AgeMs(c.refusedAtMs, nowMs) < kChatSlowMs ? Span{"slow down", style::kWarn}
                                                                         : Span{"Enter send \xC2\xB7 Esc cancel", style::kIron};
    } else if (uint32_t age = AgeMs(c.hudAtMs, nowMs); age < kChatHintMs) {
        s.idle = {true, std::string(ChatKeyLabel(c.key)) + " to chat", style::kIron, FadeOut(age, kChatHintMs, kChatFadeMs), 1};
    }
    return s;
}

}  // namespace hsm

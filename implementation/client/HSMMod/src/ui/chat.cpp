#include "ui/chat.h"
#include "ui/umg.h"
#include "match/hsm_hud_style.h"
#include "hsm_chat.h"
#include "log.h"
#include <windows.h>
#include <array>
#include <deque>
#include <utility>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr ULONGLONG kScanEveryMs = 1000;
constexpr uint32_t kRepeatDelayMs = 400, kRepeatEveryMs = 60;
constexpr uint32_t kPendingMs = 5000;                          // how long ChatOpenWith waits for the chat to be openable
constexpr int kZOrder = 60;
constexpr float kWidth = 620;                                  // the panel's width: longer lines wrap
constexpr Rgba kInputBack{0.02f, 0.017f, 0.014f, 0.92f};

struct LogRow { Box row; Text name, text; };

// Widget state. Rebuilt when the UI_HUD instance goes away; chat data below survives that.
struct Widgets {
    UClass* hudClass = nullptr;
    UObject* hud = nullptr; int32_t hudIndex = -1;
    ULONGLONG nextScan = 0;
    ULONGLONG nextLive = 0;   // when to check that the UI_HUD we sit on is still the one on screen
    bool built = false;
    Kit kit;
    Box column;
    Fixed logBox; Panel logPanel; std::array<LogRow, kChatLogLines> rows;
    Fixed inputBox; Text prompt, draft, hint;
    Text idle;
} w;

struct Key { bool down = false; uint32_t heldSince = 0, lastRepeat = 0; };

struct Data {
    std::deque<ChatEntry> lines;
    std::deque<std::string> outgoing;
    hsm::ChatSession session;
    std::function<void(bool)> capture;
    std::function<bool()> paused;
    int openVk = VK_RETURN;
    int swallowVk = 0;                      // the letter key that opened the chat, not typed until it is let go of
    Key keys[256];
    bool hudShown = false; uint32_t hudAt = 0;   // the match HUD's last state, and when it came up (the idle hint)
    uint32_t typingAt = 0;                  // the caret's phase: when the line opened or was last typed into
    bool refused = false; uint32_t refusedAt = 0;   // the last Enter was refused by the rate limiter ("slow down")
    bool lobbyTyping = false;               // the multiplayer page's chat box has the keyboard
    uint32_t added = 0;                     // lines ever shown
    bool pending = false; std::string pendingText; uint32_t pendingAt = 0;   // ChatOpenWith: open with this typed, when possible
} d;

uint32_t NowMs() { return uint32_t(GetTickCount64()); }   // wrap-safe for elapsed-time math

// Chat text: the game's font, a dark outline, never taking clicks.
Text MakeText(float size, Rgba color) {
    Text t = w.kit.MakeText();
    if (!t) return t;
    t.Font(size, 1, {0, 0, 0, 0.9f});
    t.Color(color);
    t.Align(Justify::Left);
    return t;
}

// A box of the chat's width holding a padded panel.
Panel MakeBand(Fixed& box, Rgba color, Margin padding) {
    box = w.kit.MakeFixed();
    box.Width(kWidth);
    w.column.Add(box).Align(HAlign::Left, VAlign::Bottom).Padding({0, 0, 0, 4});
    Panel p = w.kit.MakePanel();
    p.Color(color);
    p.Padding(padding);
    box.Content(p).Align(HAlign::Fill, VAlign::Fill);
    return p;
}

bool Build() {
    w.built = true;
    w.kit = Kit(w.hud, Vis::HitTestInvisible);
    Canvas root = w.kit.Root();
    if (!root) { HSM_LOG("chat: UI_HUD root is not a CanvasPanel; chat overlay disabled"); return false; }
    if (UObject** src = Field<UObject*>(w.hud, "TextHurt"); src && *src) w.kit.FontFrom(*src, "UI_HUD.TextHurt");
    // Bottom-left, growing upwards: the log panel, the input row under it, the idle hint at the bottom.
    w.column = w.kit.MakeColumn();
    root.Add(w.column).Anchor({{0.012, 0.965}, {0.012, 0.965}}, {0, 1}).AutoSize().Z(kZOrder);
    w.logPanel = MakeBand(w.logBox, style::kSmoke, {12, 6, 12, 6});
    Box lines = w.kit.MakeColumn();
    w.logPanel.Content(lines).Align(HAlign::Fill, VAlign::Bottom);
    for (LogRow& r : w.rows) {
        r.row = w.kit.MakeRow();
        lines.Add(r.row).Align(HAlign::Fill, VAlign::Top).Padding({0, 1, 0, 1});
        r.name = MakeText(20, style::kIron);
        r.row.Add(r.name).Align(HAlign::Left, VAlign::Top).Padding({0, 0, 8, 0});
        r.text = MakeText(20, style::kBone);
        r.text.Wrap(true);
        r.row.Add(r.text).Fill().Align(HAlign::Fill, VAlign::Top);
    }
    Panel input = MakeBand(w.inputBox, kInputBack, {12, 4, 12, 4});
    Box row = w.kit.MakeRow();
    input.Content(row).Align(HAlign::Fill, VAlign::Center);
    w.prompt = MakeText(20, style::kBloodInk);
    w.prompt.Set(">");
    row.Add(w.prompt).Align(HAlign::Left, VAlign::Top).Padding({0, 0, 8, 0});
    w.draft = MakeText(20, style::kBone);
    w.draft.Wrap(true);
    row.Add(w.draft).Fill().Align(HAlign::Fill, VAlign::Top);
    w.hint = MakeText(14, style::kIron);
    row.Add(w.hint).Align(HAlign::Right, VAlign::Center).Padding({12, 0, 0, 0});
    w.idle = MakeText(16, style::kIron);
    w.column.Add(w.idle).Align(HAlign::Left, VAlign::Bottom).Padding({2, 0, 0, 0});
    if (!w.draft || !w.idle) { HSM_LOG("chat: could not make the chat widgets"); return false; }
    HSM_LOG("chat: overlay added to UI_HUD");
    return true;
}

bool Foreground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Fresh press this frame, or (when allowRepeat) an auto-repeat while held. Updates the key's edge state.
bool Fires(int vk, uint32_t now, bool allowRepeat) {
    Key& k = d.keys[vk];
    if (!(GetAsyncKeyState(vk) & 0x8000)) { k.down = false; return false; }
    if (!k.down) { k.down = true; k.heldSince = k.lastRepeat = now; return true; }
    if (!allowRepeat) return false;
    if (uint32_t(now - k.heldSince) < kRepeatDelayMs || uint32_t(now - k.lastRepeat) < kRepeatEveryMs) return false;
    k.lastRepeat = now;
    return true;
}

// One event into the session; the keyboard follows what it reports, and every take and give-back is logged (with why,
// when the event stands for something else: a level change is a hidden HUD).
void Feed(hsm::ChatEvent e, uint32_t now, const std::string& text = std::string(), const char* why = nullptr) {
    const hsm::ChatStep st = d.session.On(e, now, text);
    if (d.session.typing() && (st.captureChanged || e == hsm::ChatEvent::Char || e == hsm::ChatEvent::Backspace)) {
        d.typingAt = now;                   // the caret shows solid while keys go in, and blinks once they stop
    }
    if (st.captureChanged) d.refused = false;
    if (st.refused) { d.refused = true; d.refusedAt = now; }
    if (st.captureChanged) {
        if (d.capture) d.capture(st.capture);
        HSM_LOG("chat: capture %s (%s%s%s)", st.capture ? "on" : "off", hsm::ChatEventName(e), why ? ": " : "", why ? why : "");
    }
    if (st.send) d.outgoing.push_back(*st.send);
}

// Typed text as UTF-8 (the session refuses control characters and anything past the wire limit).
std::string Utf8(const wchar_t* in, int n) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, in, n, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return std::string();
    std::string u(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, in, n, u.data(), bytes, nullptr, nullptr);
    return u;
}

// Keyboard state for ToUnicode, read from the live async state (the thread message queue may not be pumped).
void BuildKeyState(BYTE (&ks)[256]) {
    for (int vk = 0; vk < 256; ++vk) ks[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 0x80 : 0;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] |= 1;
}

void Poll(uint32_t now) {
    if (!d.session.typing()) {
        if (!Fires(d.openVk, now, false)) return;
        if (d.paused && d.paused()) return;   // no chat over the pause menu
        if (d.openVk != VK_RETURN) d.swallowVk = d.openVk;
        Feed(d.openVk == VK_RETURN ? hsm::ChatEvent::Enter : hsm::ChatEvent::OpenKey, now);
        return;
    }
    if (d.paused && d.paused()) { Feed(hsm::ChatEvent::Paused, now); return; }
    if (Fires(VK_ESCAPE, now, false)) { Feed(hsm::ChatEvent::Escape, now); return; }
    if (Fires(VK_RETURN, now, false)) { Feed(hsm::ChatEvent::Enter, now); return; }
    BYTE ks[256] = {};
    bool ksReady = false;
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk == VK_RETURN || vk == VK_ESCAPE) continue;
        if (vk == d.swallowVk) {   // held since it opened the chat: typed only once pressed again
            if (!(GetAsyncKeyState(vk) & 0x8000)) { d.swallowVk = 0; d.keys[vk].down = false; }
            continue;
        }
        if (!Fires(vk, now, true)) continue;
        if (vk == VK_BACK) { Feed(hsm::ChatEvent::Backspace, now); continue; }
        if (!ksReady) { BuildKeyState(ks); ksReady = true; }
        WCHAR buf[8] = {};
        int n = ToUnicode(UINT(vk), MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC), ks, buf, 8, 0x4);   // 0x4: keep dead-key state
        if (n > 0) Feed(hsm::ChatEvent::Char, now, Utf8(buf, n));
    }
}

// ChatOpenWith's request, once the chat can open (the key press would be refused the same way).
void OpenPending(uint32_t now) {
    if (uint32_t(now - d.pendingAt) > kPendingMs) { d.pending = false; HSM_LOG("chat: could not open with text; gave up"); return; }
    if (d.session.typing()) { d.pending = false; return; }
    if (d.paused && d.paused()) return;
    d.pending = false;
    Feed(hsm::ChatEvent::OpenKey, now, std::string(), "text prefilled");
    if (d.session.typing()) Feed(hsm::ChatEvent::Char, now, d.pendingText);
}

void Draw(const ChatScene& s) {
    if (!w.hudClass) {   // not loaded yet (a menu): four full name scans, so at most once per kScanEveryMs
        const ULONGLONG now = GetTickCount64();
        if (now < w.nextScan) return;
        if (!(w.hudClass = FindClass("UI_HUD_C"))) { w.nextScan = now + kScanEveryMs; return; }
    }
    if (!w.hud || ObjectAt(w.hudIndex) != w.hud || ClassOf(w.hud) != w.hudClass) {
        if (w.hud) { UClass* cls = w.hudClass; ULONGLONG next = w.nextScan; w = Widgets{}; w.hudClass = cls; w.nextScan = next; }
        ULONGLONG now = GetTickCount64();
        if (!s.visible || now < w.nextScan) return;
        w.nextScan = now + kScanEveryMs;
        w.hud = OnScreen(FindInstances("UI_HUD_C"));
        if (!w.hud) return;
        w.hudIndex = ObjectIndex(w.hud);
    }
    // The game swaps UI_HUD on a respawn or a new round: the old one leaves the screen but lives until garbage
    // collection (about a minute), and our widgets went with it. Move to the one on screen now.
    if (w.built) {
        const ULONGLONG now = GetTickCount64();
        if (now >= w.nextLive) {
            w.nextLive = now + 1000;
            if (!InViewport(w.hud))
                if (UObject* on = OnScreen(FindInstances("UI_HUD_C")); on && on != w.hud) {
                    UClass* cls = w.hudClass; w = Widgets{}; w.hudClass = cls; w.hud = on; w.hudIndex = ObjectIndex(on);
                    w.nextLive = now + 1000;
                    HSM_LOG("chat: UI_HUD replaced by the game; moving to the new one");
                }
        }
    }
    if (!w.built && !Build()) return;
    w.column.Show(s.visible);
    if (!s.visible) return;
    w.logBox.Show(s.panel > 0);
    w.logPanel.Color({style::kSmoke.r, style::kSmoke.g, style::kSmoke.b, style::kSmoke.a * s.panel});
    for (size_t i = 0; i < w.rows.size(); ++i) {
        LogRow& r = w.rows[i];
        r.row.Show(i < s.lines.size());
        if (i >= s.lines.size()) continue;
        const ChatLineView& l = s.lines[i];
        r.name.Set(l.name.text); r.name.Color(l.name.color);
        r.text.Set(l.text.text); r.text.Color(l.text.color);
        r.row.Opacity(l.opacity);
    }
    w.inputBox.Show(s.input);
    if (s.input) {
        w.draft.Set(s.draft + (s.caret ? "_" : " "));
        w.hint.Set(s.hint.text); w.hint.Color(s.hint.color);
    }
    w.idle.Show(s.idle.visible);
    if (s.idle.visible) { w.idle.Set(s.idle.text); w.idle.Color(s.idle.color); w.idle.Opacity(s.idle.opacity); }
}

int VkOf(ChatKey k) {
    switch (k) {
        case ChatKey::T: return 'T';
        case ChatKey::Y: return 'Y';
        case ChatKey::U: return 'U';
        case ChatKey::Enter: break;
    }
    return VK_RETURN;
}

}  // namespace

void ChatSetup(ChatKey key, std::function<void(bool on)> capture, std::function<bool()> paused) {
    d.session = hsm::ChatSession(key);
    d.openVk = VkOf(key);
    d.capture = std::move(capture);
    d.paused = std::move(paused);
}

void ChatTick(const HudScene& m) {
    uint32_t now = NowMs();
    if (m.visible && !d.hudShown) d.hudAt = now;
    d.hudShown = m.visible;
    if (!Foreground() || !m.visible) {
        // Keys are read only while the game window is foreground and the overlay is active; a line being typed is dropped
        // and the keyboard given back.
        for (Key& k : d.keys) k.down = false;
        d.swallowVk = 0;
        if (d.session.typing()) Feed(!Foreground() ? hsm::ChatEvent::FocusLost : hsm::ChatEvent::HudHidden, now);
    } else {
        if (d.pending) OpenPending(now);
        Poll(now);
    }
    if (d.pending && uint32_t(now - d.pendingAt) > kPendingMs) d.pending = false;   // the HUD never came up
    ChatLocal c;
    c.hud = m.visible; c.hudAtMs = d.hudAt; c.key = d.session.key();
    c.lines.assign(d.lines.begin(), d.lines.end());
    c.typing = d.session.typing(); c.typingAtMs = d.typingAt; c.draft = d.session.draft();
    c.refused = d.refused; c.refusedAtMs = d.refusedAt;
    Draw(BuildChatScene(c, now));
}

std::optional<std::string> TakeOutgoing() {
    if (d.outgoing.empty()) return std::nullopt;
    std::string s = std::move(d.outgoing.front());
    d.outgoing.pop_front();
    return s;
}

void ShowLine(const std::string& name, Rgba ink, const std::string& text) {
    std::string clean = hsm::SanitizeChat(text);
    if (clean.empty()) return;
    d.lines.push_back(ChatEntry{hsm::SanitizeChat(name), ink, clean, NowMs()});
    ++d.added;
    while (d.lines.size() > hsm::kChatLogLines) d.lines.pop_front();
}

void ChatOpenWith(const std::string& text) {
    if (d.session.typing()) return;
    d.pending = true; d.pendingText = text; d.pendingAt = NowMs();
}

bool Typing() { return d.session.typing() || d.lobbyTyping; }

bool ChatHudShown() { return d.hudShown; }
void SetLobbyTyping(bool on) { d.lobbyTyping = on; }
std::vector<ChatEntry> ChatHistory() { return std::vector<ChatEntry>(d.lines.begin(), d.lines.end()); }
uint32_t ChatLinesAdded() { return d.added; }
void ChatClearLines() { d.lines.clear(); }

void ChatDrop(const char* why) {
    if (!d.session.typing()) return;
    d.swallowVk = 0;
    Feed(hsm::ChatEvent::HudHidden, NowMs(), std::string(), why);
}

}  // namespace hsm::ui

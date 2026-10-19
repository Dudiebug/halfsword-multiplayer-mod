#include "ui/chat.h"
#include "util/hsm_names.h"
#include "hsm_chat.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <deque>
#include <utility>
#include <vector>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

struct Vec2 { double x = 0, y = 0; };
struct Anchors { Vec2 min, max; };
enum : uint8_t { kCollapsed = 1, kHitTestInvisible = 3 };

constexpr ULONGLONG kScanEveryMs = 1000;
constexpr uint32_t kRepeatDelayMs = 400, kRepeatEveryMs = 60;
constexpr int kZOrder = 60;

struct Label { UObject* text = nullptr; std::string shown; bool visible = true; };

// Widget state. Rebuilt when the UI_HUD instance goes away; chat data below survives that.
struct Widgets {
    UClass* hudClass = nullptr;
    UObject* hud = nullptr; int32_t hudIndex = -1;
    ULONGLONG nextScan = 0;
    bool built = false;
    Label log, input;
    std::vector<uint8_t> font;
} w;

struct Shown { std::string name, text; uint32_t atMs = 0; };
struct Key { bool down = false; uint32_t heldSince = 0, lastRepeat = 0; };

struct Data {
    std::deque<Shown> lines;
    std::deque<std::string> outgoing;
    bool typing = false;
    std::string draft;
    hsm::ChatRateLimiter limiter;
    Key keys[256];
} d;

uint32_t NowMs() { return uint32_t(GetTickCount64()); }   // wrap-safe for elapsed-time math

UObject* Tree(UObject* o) { UObject** t = Field<UObject*>(o, "WidgetTree"); return t ? *t : nullptr; }

void SetLabel(Label& l, const std::string& s) {
    if (!l.text || s == l.shown) return;
    l.shown = s;
    FText t = MakeText(s);
    Call(l.text, "SetText").set("InText", t).invoke();
}

void ShowLabel(Label& l, bool on) {
    if (!l.text || on == l.visible) return;
    l.visible = on;
    Call(l.text, "SetVisibility").set<uint8_t>("InVisibility", on ? kHitTestInvisible : kCollapsed).invoke();
}

UObject* MakeLabel(UObject* tree, UObject* canvas, Anchors a, Vec2 align, int justify) {
    UObject* t = SpawnObject(FindClass("TextBlock"), tree);
    if (!t) return nullptr;
    if (!w.font.empty()) {
        Call c(t, "SetFont");
        if (FProperty* p = c.function() ? FindProperty(c.function(), "InFontInfo") : nullptr; p && size_t(PropertySize(p)) == w.font.size()) {
            std::memcpy(c.paramPtr("InFontInfo"), w.font.data(), w.font.size());
            c.invoke();
        }
    }
    Call(t, "SetJustification").set<uint8_t>("InJustification", uint8_t(justify)).invoke();
    Call add(canvas, "AddChildToCanvas"); add.set<UObject*>("Content", t).invoke();
    if (UObject* slot = add.get<UObject*>("ReturnValue")) {
        Call(slot, "SetAnchors").set("InAnchors", a).invoke();
        Call(slot, "SetAlignment").set("InAlignment", align).invoke();
        Call(slot, "SetAutoSize").set<bool>("InbAutoSize", true).invoke();
        Call(slot, "SetZOrder").set<int32_t>("InZOrder", kZOrder).invoke();
    }
    Call(t, "SetVisibility").set<uint8_t>("InVisibility", kHitTestInvisible).invoke();
    return t;
}

bool Build() {
    w.built = true;
    UObject* tree = Tree(w.hud);
    UObject** rootPtr = tree ? Field<UObject*>(tree, "RootWidget") : nullptr;
    UObject* root = rootPtr ? *rootPtr : nullptr;
    if (!root || !SameNameAscii(ObjectName(reinterpret_cast<UObject*>(ClassOf(root))), "CanvasPanel")) { HSM_LOG("chat: UI_HUD root is not a CanvasPanel; chat overlay disabled"); return false; }
    if (UObject** src = Field<UObject*>(w.hud, "TextHurt"); src && *src)
        if (void* font = FieldPtr(*src, "Font")) {
            FProperty* p = FindProperty(ClassOf(*src), "Font");
            w.font.assign(static_cast<uint8_t*>(font), static_cast<uint8_t*>(font) + PropertySize(p));
        }
    // Bottom-left: the input line sits at the bottom, recent lines stack above it.
    w.log.text = MakeLabel(tree, root, {{0.0, 0.93}, {0.0, 0.93}}, {0, 1}, 0);
    w.input.text = MakeLabel(tree, root, {{0.0, 0.97}, {0.0, 0.97}}, {0, 1}, 0);
    HSM_LOG("chat: overlay added to UI_HUD");
    return w.input.text != nullptr;
}

// Pruning: drop lines past their display window (wrap-safe age).
void Prune(uint32_t now) {
    while (!d.lines.empty() && uint32_t(now - d.lines.front().atMs) >= hsm::kChatExpireMs) d.lines.pop_front();
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

void StartTyping() { d.typing = true; d.draft.clear(); }
void CancelTyping() { d.typing = false; d.draft.clear(); }

void SendTyping(uint32_t now) {
    std::string text = hsm::SanitizeChat(d.draft);
    if (text.empty()) { CancelTyping(); return; }
    if (!d.limiter.Allow(now)) return;     // too fast: keep the draft so the player can resend
    d.outgoing.push_back(text);
    CancelTyping();
}

void Backspace() {
    if (d.draft.empty()) return;
    size_t i = d.draft.size() - 1;
    while (i > 0 && (uint8_t(d.draft[i]) & 0xC0) == 0x80) --i;   // step back to the lead byte
    d.draft.resize(i);
}

// Appends typed text, dropping control characters and anything past the wire limit.
void AppendDraft(const wchar_t* in, int n) {
    std::wstring keep;
    for (int i = 0; i < n; ++i) if (in[i] >= 0x20 && in[i] != 0x7F) keep.push_back(in[i]);
    if (keep.empty()) return;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, keep.data(), int(keep.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string u(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, keep.data(), int(keep.size()), u.data(), bytes, nullptr, nullptr);
    if (d.draft.size() + u.size() > hsm::kMaxChatBytes) return;
    d.draft += u;
}

// Keyboard state for ToUnicode, read from the live async state (the thread message queue may not be pumped).
void BuildKeyState(BYTE (&ks)[256]) {
    for (int vk = 0; vk < 256; ++vk) ks[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 0x80 : 0;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] |= 1;
}

void Poll(uint32_t now) {
    if (!d.typing) {
        if (Fires('T', now, false)) StartTyping();   // the T that opened typing is not typed
        return;
    }
    if (Fires(VK_ESCAPE, now, false)) { CancelTyping(); return; }
    if (Fires(VK_RETURN, now, false)) { SendTyping(now); return; }
    BYTE ks[256] = {};
    bool ksReady = false;
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (vk == VK_RETURN || vk == VK_ESCAPE) continue;
        if (!Fires(vk, now, true)) continue;
        if (vk == VK_BACK) { Backspace(); continue; }
        if (!ksReady) { BuildKeyState(ks); ksReady = true; }
        WCHAR buf[8] = {};
        int n = ToUnicode(UINT(vk), MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC), ks, buf, 8, 0x4);   // 0x4: keep dead-key state
        if (n > 0) AppendDraft(buf, n);
    }
}

std::string JoinLines() {
    std::string out;
    for (const Shown& s : d.lines) {
        if (!out.empty()) out += '\n';
        out += s.name + ": " + s.text;
    }
    return out;
}

void Draw(const HudModel& m) {
    if (!w.hudClass) w.hudClass = FindClass("UI_HUD_C");
    if (!w.hudClass) return;
    if (!w.hud || ObjectAt(w.hudIndex) != w.hud || ClassOf(w.hud) != w.hudClass) {
        if (w.hud) { UClass* cls = w.hudClass; ULONGLONG next = w.nextScan; w = Widgets{}; w.hudClass = cls; w.nextScan = next; }
        ULONGLONG now = GetTickCount64();
        if (!m.visible || now < w.nextScan) return;
        w.nextScan = now + kScanEveryMs;
        for (UObject* o : FindInstances("UI_HUD_C")) {
            Call v(o, "IsInViewport"); v.invoke();
            if (v.get<bool>("ReturnValue")) { w.hud = o; w.hudIndex = ObjectIndex(o); break; }
        }
        if (!w.hud) return;
    }
    if (!w.built && !Build()) return;
    std::string log = JoinLines();
    SetLabel(w.log, log); ShowLabel(w.log, m.visible && !log.empty());
    SetLabel(w.input, "> " + d.draft + "_"); ShowLabel(w.input, m.visible && d.typing);
}

}  // namespace

void ChatTick(const HudModel& m) {
    uint32_t now = NowMs();
    Prune(now);
    if (!Foreground() || !m.visible) {
        // Keys are read only while the game window is foreground and the overlay is active.
        for (Key& k : d.keys) k.down = false;
        if (d.typing) CancelTyping();
    } else {
        Poll(now);
    }
    Draw(m);
}

std::optional<std::string> TakeOutgoing() {
    if (d.outgoing.empty()) return std::nullopt;
    std::string s = std::move(d.outgoing.front());
    d.outgoing.pop_front();
    return s;
}

void ShowLine(const std::string& name, const std::string& text) {
    std::string clean = hsm::SanitizeChat(text);
    if (clean.empty()) return;
    d.lines.push_back(Shown{hsm::SanitizeChat(name), clean, NowMs()});
    while (d.lines.size() > hsm::kChatLogLines) d.lines.pop_front();
}

bool Typing() { return d.typing; }

}  // namespace hsm::ui

#include "ui/hud.h"
#include "util/hsm_names.h"
#include "log.h"
#include <windows.h>
#include <vector>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

struct Vec2 { double x = 0, y = 0; };
struct Anchors { Vec2 min, max; };
enum : uint8_t { kVisible = 0, kCollapsed = 1, kHitTestInvisible = 3 };

struct Line { UObject* text = nullptr; std::string shown; bool visible = true; };

struct State {
    UClass* hudClass = nullptr;
    UObject* hud = nullptr; int32_t hudIndex = -1;
    ULONGLONG nextScan = 0;
    bool built = false;
    Line status, respawn, feed, board;
    std::vector<uint8_t> font;
} g;

UObject* Tree(UObject* w) { UObject** t = Field<UObject*>(w, "WidgetTree"); return t ? *t : nullptr; }

void SetText(Line& l, const std::string& s) {
    if (!l.text || s == l.shown) return;
    l.shown = s;
    FText t = MakeText(s);
    Call(l.text, "SetText").set("InText", t).invoke();
}

void Show(Line& l, bool on) {
    if (!l.text || on == l.visible) return;
    l.visible = on;
    Call(l.text, "SetVisibility").set<uint8_t>("InVisibility", on ? kHitTestInvisible : kCollapsed).invoke();
}

UObject* MakeText(UObject* tree, UObject* canvas, Anchors a, Vec2 align, int justify) {
    UObject* t = SpawnObject(FindClass("TextBlock"), tree);
    if (!t) return nullptr;
    if (!g.font.empty()) {
        Call c(t, "SetFont");
        if (FProperty* p = c.function() ? FindProperty(c.function(), "InFontInfo") : nullptr; p && size_t(PropertySize(p)) == g.font.size()) {
            std::memcpy(c.paramPtr("InFontInfo"), g.font.data(), g.font.size());
            c.invoke();
        }
    }
    Call(t, "SetJustification").set<uint8_t>("InJustification", uint8_t(justify)).invoke();
    Call add(canvas, "AddChildToCanvas"); add.set<UObject*>("Content", t).invoke();
    if (UObject* slot = add.get<UObject*>("ReturnValue")) {
        Call(slot, "SetAnchors").set("InAnchors", a).invoke();
        Call(slot, "SetAlignment").set("InAlignment", align).invoke();
        Call(slot, "SetAutoSize").set<bool>("InbAutoSize", true).invoke();
        Call(slot, "SetZOrder").set<int32_t>("InZOrder", 50).invoke();
    }
    Call(t, "SetVisibility").set<uint8_t>("InVisibility", kHitTestInvisible).invoke();
    return t;
}

bool Build() {
    g.built = true;
    UObject* tree = Tree(g.hud);
    UObject** rootPtr = tree ? Field<UObject*>(tree, "RootWidget") : nullptr;
    UObject* root = rootPtr ? *rootPtr : nullptr;
    if (!root || !SameNameAscii(ObjectName(reinterpret_cast<UObject*>(ClassOf(root))), "CanvasPanel")) { HSM_LOG("hud: UI_HUD root is not a CanvasPanel; match HUD disabled"); return false; }
    if (UObject** src = Field<UObject*>(g.hud, "TextHurt"); src && *src)
        if (void* font = FieldPtr(*src, "Font")) {
            FProperty* p = FindProperty(ClassOf(*src), "Font");
            g.font.assign(static_cast<uint8_t*>(font), static_cast<uint8_t*>(font) + PropertySize(p));
        }
    g.status.text = MakeText(tree, root, {{0.5, 0.03}, {0.5, 0.03}}, {0.5, 0}, 1);
    g.respawn.text = MakeText(tree, root, {{0.5, 0.40}, {0.5, 0.40}}, {0.5, 0.5}, 1);
    g.feed.text = MakeText(tree, root, {{0.98, 0.08}, {0.98, 0.08}}, {1, 0}, 2);
    g.board.text = MakeText(tree, root, {{0.5, 0.22}, {0.5, 0.22}}, {0.5, 0}, 0);
    HSM_LOG("hud: match HUD added to UI_HUD");
    return g.status.text != nullptr;
}

bool TabHeld() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_TAB) & 0x8000);
}
}  // namespace

void HudTick(const HudModel& m) {
    if (!g.hudClass) g.hudClass = FindClass("UI_HUD_C");
    if (!g.hudClass) return;
    if (!g.hud || ObjectAt(g.hudIndex) != g.hud || ClassOf(g.hud) != g.hudClass) {
        if (g.hud) { UClass* cls = g.hudClass; ULONGLONG next = g.nextScan; g = State{}; g.hudClass = cls; g.nextScan = next; }
        ULONGLONG now = GetTickCount64();
        if (!m.visible || now < g.nextScan) return;
        g.nextScan = now + 1000;
        for (UObject* w : FindInstances("UI_HUD_C")) {
            Call v(w, "IsInViewport"); v.invoke();
            if (v.get<bool>("ReturnValue")) { g.hud = w; g.hudIndex = ObjectIndex(w); break; }
        }
        if (!g.hud) return;
    }
    if (!g.built && !Build()) return;
    SetText(g.status, m.status); Show(g.status, m.visible && !m.status.empty());
    SetText(g.respawn, m.respawn); Show(g.respawn, m.visible && !m.respawn.empty());
    SetText(g.feed, m.feed); Show(g.feed, m.visible && !m.feed.empty());
    bool board = m.visible && TabHeld() && !m.scoreboard.empty();
    if (board) SetText(g.board, m.scoreboard);
    Show(g.board, board);
}

}  // namespace hsm::ui

#include "ui/pause.h"
#include "ui/umg.h"
#include "ui/mp_widgets.h"
#include "match/hsm_hud_style.h"
#include "log.h"
#include <windows.h>
#include <string>
#include <vector>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr int kMaxNodes = 200, kMaxDepth = 12;
constexpr double kGapBelow = 1.15;   // the same step the main menu's MULTIPLAYER entry takes below its anchor

struct Found { UObject* button; UObject* parent; };

// The injected button and which pause menu instance it is in (the slot and the class guard against a reused address).
struct State {
    UObject* menu = nullptr; int32_t menuIndex = -1; UClass* menuClass = nullptr;
    bool tried = false, escTried = false, open = false;
    Kit kit;
    Button button;
    Text label;
    mp::EscPanel esc;
} s;

bool IsClass(UObject* o, const char* cls) {
    UClass* c = FindClass(cls);
    return o && c && IsA(o, c);
}

UObject* Parent(UObject* w) {
    Call p(w, "GetParent");
    return p.invoke() ? p.get<UObject*>("ReturnValue") : nullptr;
}

// Walks the widget tree below `w`: logs each widget (when `log`), collects the buttons. Nested user widgets are not
// entered: their buttons live in their own trees.
void Walk(UObject* w, int depth, int& nodes, bool log, std::vector<Found>& buttons) {
    if (!w || depth > kMaxDepth || ++nodes > kMaxNodes) return;
    const std::string cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(w)));
    if (log) HSM_LOG("ui: pause menu tree: %*s%s '%s'", depth * 2, "", cls.c_str(), ObjectName(w).c_str());
    if (IsClass(w, "Button")) buttons.push_back({w, Parent(w)});
    if (IsClass(w, "PanelWidget")) {
        Call n(w, "GetChildrenCount");
        const int32_t count = n.invoke() ? n.get<int32_t>("ReturnValue") : 0;
        for (int32_t i = 0; i < count; ++i) {
            Call c(w, "GetChildAt"); c.set<int32_t>("Index", i);
            if (c.invoke()) Walk(c.get<UObject*>("ReturnValue"), depth + 1, nodes, log, buttons);
        }
    } else if (IsClass(w, "ContentWidget")) {
        Call c(w, "GetContent");
        if (c.invoke()) Walk(c.get<UObject*>("ReturnValue"), depth + 1, nodes, log, buttons);
    }
}

// The first TextBlock in `w`'s subtree (a stock button's label), or nullptr.
UObject* FindLabel(UObject* w, int depth) {
    if (!w || depth > 6) return nullptr;
    if (IsClass(w, "TextBlock")) return w;
    if (IsClass(w, "PanelWidget")) {
        Call n(w, "GetChildrenCount");
        const int32_t count = n.invoke() ? n.get<int32_t>("ReturnValue") : 0;
        for (int32_t i = 0; i < count; ++i) {
            Call c(w, "GetChildAt"); c.set<int32_t>("Index", i);
            if (c.invoke()) if (UObject* t = FindLabel(c.get<UObject*>("ReturnValue"), depth + 1)) return t;
        }
    } else if (IsClass(w, "ContentWidget")) {
        Call c(w, "GetContent");
        if (c.invoke()) return FindLabel(c.get<UObject*>("ReturnValue"), depth + 1);
    }
    return nullptr;
}

// A canvas child's top edge (position y); false when it has no canvas slot.
bool CanvasTop(UObject* button, double& top) {
    UObject** slot = Field<UObject*>(button, "Slot");
    if (!slot || !*slot || !IsClass(*slot, "CanvasPanelSlot")) return false;
    Call pos(*slot, "GetPosition");
    if (!pos.invoke()) return false;
    top = pos.get<Vec2>("ReturnValue").y;
    return true;
}

// The stock button the new one goes under: the lowest on a canvas, else the last one in the tree.
const Found* LastButton(const std::vector<Found>& buttons) {
    const Found* best = nullptr;
    double bestTop = 0;
    bool bestOnCanvas = false;
    for (const Found& f : buttons) {
        double top = 0;
        if (CanvasTop(f.button, top)) {
            if (!best || !bestOnCanvas || top >= bestTop) { best = &f; bestTop = top; bestOnCanvas = true; }
        } else if (!bestOnCanvas) {
            best = &f;
        }
    }
    return best;
}

// The label's typeface from the stock button's, deep-copied by the engine (never a raw copy of the font struct).
bool CopyFont(UObject* from, UObject* to) {
    void* font = from ? FieldPtr(from, "Font") : nullptr;
    if (!font || !to) return false;
    Call c(to, "SetFont");
    if (!c.function() || !c.paramPtr("InFontInfo")) return false;
    c.setFrom("InFontInfo", font);
    return c.invoke();
}

bool Build(UObject* menu) {
    if (!s.kit) s.kit = Kit(menu, Vis::Visible);
    if (!s.kit) { HSM_LOG("ui: pause menu has no widget tree; no report button"); return false; }
    UObject** tree = Field<UObject*>(menu, "WidgetTree");
    UObject** rootPtr = tree && *tree ? Field<UObject*>(*tree, "RootWidget") : nullptr;
    UObject* root = rootPtr ? *rootPtr : nullptr;
    static bool logged = false;
    std::vector<Found> buttons;
    int nodes = 0;
    Walk(root, 0, nodes, !logged, buttons);
    const Found* last = LastButton(buttons);
    HSM_LOG("ui: pause menu: %d widget(s), %zu button(s)%s", nodes, buttons.size(), logged ? "" : " (tree above)");
    logged = true;
    UObject* stockLabel = nullptr;
    for (const Found& f : buttons) if ((stockLabel = FindLabel(f.button, 0))) break;
    s.button = s.kit.MakeButton();
    s.label = s.kit.MakeText();
    if (!s.button || !s.label) { HSM_LOG("ui: pause menu report button: widget spawn failed"); return false; }
    s.button.Content(s.label);
    s.label.Set(s.esc ? "MULTIPLAYER MENU" : "REPORT A BUG");
    s.label.Color(style::kBone);
    s.label.Align(Justify::Center);
    if (last && stockLabel) {   // a text button: its look and typeface. A picture button keeps the default look.
        s.button.StyleFrom(last->button);
        if (!CopyFont(stockLabel, s.label.get())) HSM_LOG("ui: pause menu: the stock label's font was not copied");
    }
    if (!last) {                // no stock button found: bottom centre of the root canvas, if it is one
        Canvas c = s.kit.Root();
        if (!c) { HSM_LOG("ui: pause menu has no buttons and no canvas root; no report button"); return false; }
        c.Add(s.button).Anchor({{0.5, 0.82}, {0.5, 0.82}}, {0.5, 0}).AutoSize();
        HSM_LOG("ui: pause menu report button added to the root canvas (no stock button found)");
        return true;
    }
    UObject* parent = last->parent;
    if (!parent) { HSM_LOG("ui: pause menu: the last button has no parent; no report button"); return false; }
    const std::string pcls = ObjectName(reinterpret_cast<UObject*>(ClassOf(parent)));
    if (pcls == "CanvasPanel") {
        Call add(parent, "AddChildToCanvas"); add.set<UObject*>("Content", s.button.get()).invoke();
        Slot slot(add.get<UObject*>("ReturnValue"));
        UObject** from = Field<UObject*>(last->button, "Slot");
        if (!slot || !from || !*from) { HSM_LOG("ui: pause menu: no canvas slot for the report button"); return false; }
        Call pos(*from, "GetPosition"); pos.invoke(); const Vec2 p = pos.get<Vec2>("ReturnValue");
        Call size(*from, "GetSize"); size.invoke(); const Vec2 sz = size.get<Vec2>("ReturnValue");
        Call anc(*from, "GetAnchors"); anc.invoke(); const Anchors a = anc.get<Anchors>("ReturnValue");
        Call al(*from, "GetAlignment"); al.invoke(); const Vec2 alignment = al.get<Vec2>("ReturnValue");
        slot.Anchor(a, alignment).Size(sz).Position(Vec2{p.x, p.y + sz.y * kGapBelow});
    } else {
        // AddChild is the reflected way in (InsertChildAt is C++-only), so the button goes to the end of its box. It is
        // held to the stock button's measured size when that is known.
        Call ds(last->button, "GetDesiredSize");
        const Vec2 want = ds.invoke() ? ds.get<Vec2>("ReturnValue") : Vec2{};
        Fixed sized = s.kit.MakeFixed();
        UObject* child = s.button.get();
        if (sized && want.x > 1 && want.y > 1) {
            sized.Width(float(want.x)); sized.Height(float(want.y));
            sized.Content(s.button);
            child = sized.get();
        }
        Call add(parent, "AddChild"); add.set<UObject*>("Content", child).invoke();
        Slot slot(add.get<UObject*>("ReturnValue"));
        if (!slot) { HSM_LOG("ui: pause menu: %s refused the report button", pcls.c_str()); return false; }
        slot.Padding({0, 8, 0, 0}).Align(HAlign::Center, VAlign::Center);
    }
    HSM_LOG("ui: pause menu report button added to %s", pcls.c_str());
    return true;
}

// The multiplayer Esc menu, on the menu's root canvas (or the first canvas in it), in the stock labels' typeface.
bool BuildEsc(UObject* menu) {
    if (!s.kit) s.kit = Kit(menu, Vis::Visible);
    if (!s.kit) { HSM_LOG("ui: pause menu has no widget tree; no multiplayer Esc menu"); return false; }
    if (!s.kit.HasFont()) {
        UObject* stock = FindTextBlock(menu, 12);
        if (!stock || !s.kit.FontFrom(stock, "the pause menu")) if (Text probe = s.kit.MakeText()) s.kit.FontFrom(probe.get(), "a new TextBlock");
    }
    Canvas canvas = s.kit.FirstCanvas();
    if (!canvas) { HSM_LOG("ui: pause menu has no canvas panel; the stock menu stays (no multiplayer Esc menu)"); return false; }
    if (!s.esc.Build(s.kit, canvas)) return false;
    HSM_LOG("ui: multiplayer Esc menu built on the pause menu");
    return true;
}

}  // namespace

PauseAction PauseMenuTick(UObject* menu, Session* session, bool reportButton) {
    if (!menu) {   // closed: the same instance may be shown again, so its widgets are kept
        if (s.esc && s.esc.Shown()) s.esc.Show(false);
        s.open = false;
        return PauseAction::None;
    }
    if (s.menu != menu || ObjectAt(s.menuIndex) != menu || ClassOf(menu) != s.menuClass) {   // a new menu instance
        s = State{};
        s.menu = menu; s.menuIndex = ObjectIndex(menu); s.menuClass = ClassOf(menu);
    }
    const bool opened = !s.open;
    s.open = true;
    if (session && !s.escTried) { s.escTried = true; BuildEsc(menu); }
    const bool esc = session && s.esc;
    if (esc && opened) s.esc.Show(true);                    // Esc in a match: the multiplayer menu first
    if (!esc && s.esc && s.esc.Shown()) s.esc.Show(false);  // the session ended with the menu open
    if (esc || reportButton) {
        if (!s.tried) {
            s.tried = true;
            if (!Build(menu)) { if (s.button) s.button.Show(false); s.button = Button{}; }   // not tried again in this instance
        }
        if (s.button) {
            s.label.Set(esc ? "MULTIPLAYER MENU" : "REPORT A BUG");
            s.button.Show(!(esc && s.esc.Shown()));
            if (s.button.Clicked()) {
                if (esc) s.esc.Show(true);
                else return PauseAction::ReportBug;
            }
        }
    } else if (s.button) {
        s.button.Show(false);
    }
    if (!esc || !s.esc.Shown()) return PauseAction::None;
    switch (s.esc.Tick(*session, reportButton)) {
        case mp::EscPanel::Action::None: break;
        case mp::EscPanel::Action::Resume: return PauseAction::Resume;
        case mp::EscPanel::Action::GameMenu: s.esc.Show(false); HSM_LOG("ui: Esc menu: GAME MENU (the stock pause menu)"); break;
        case mp::EscPanel::Action::ReportBug: return PauseAction::ReportBug;
        case mp::EscPanel::Action::Leave: return PauseAction::Leave;
    }
    return PauseAction::None;
}

}  // namespace hsm::ui

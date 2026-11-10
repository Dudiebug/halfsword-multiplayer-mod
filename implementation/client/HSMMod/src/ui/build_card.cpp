#include "ui/build_card.h"
#include "ui/umg.h"
#include "ui/skin.h"
#include "log.h"
#include <windows.h>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr ULONGLONG kScanEveryMs = 1000;
constexpr int kZOrder = 55;           // under the chat and the banners
constexpr float kWidth = 430;
constexpr float kTextSize = 15;
// Ink on the game's parchment: the player's name dark, the rest in the rubric. Classic look: the launcher's terminal
// palette as linear colours, #E8C8A0 and #C07040.
Rgba Bright() { return style::ClassicMenus() ? Rgba{0.807f, 0.578f, 0.352f, 1} : style::kMenuText; }
Rgba Main() { return style::ClassicMenus() ? Rgba{0.527f, 0.162f, 0.051f, 1} : style::kMenuAccentInk; }

struct Widgets {
    UClass* hudClass = nullptr;
    UObject* hud = nullptr; int32_t hudIndex = -1;
    ULONGLONG nextScan = 0, nextLive = 0;
    bool built = false;
    Kit kit;
    Fixed box;
    Text head, player, build, mods;
} w;

BuildCardQueue g_queue;

Text MakeLine(Box& column, Rgba color) {
    Text t = w.kit.MakeText();
    if (!t) return t;
    if (style::ClassicMenus()) t.Font(kTextSize, 1, {0, 0, 0, 0.9f}); else t.Font(kTextSize);
    skin::Font(t, skin::Face::Body);
    t.Color(color);
    t.Align(Justify::Left);
    t.Wrap(true);
    column.Add(t).Align(HAlign::Fill, VAlign::Top).Padding({0, 1, 0, 1});
    return t;
}

bool Build() {
    w.built = true;
    w.kit = Kit(w.hud, Vis::HitTestInvisible);
    Canvas root = w.kit.Root();
    if (!root) { HSM_LOG("buildcard: UI_HUD root is not a CanvasPanel; cards are off"); return false; }
    if (UObject** src = Field<UObject*>(w.hud, "TextHurt"); src && *src) w.kit.FontFrom(*src, "UI_HUD.TextHurt");
    w.box = w.kit.MakeFixed();
    w.box.Width(kWidth);
    root.Add(w.box).Anchor({{0.012, 0.03}, {0.012, 0.03}}, {0, 0}).AutoSize().Z(kZOrder);
    // The game's parchment button: its gilt frame round the card.
    Panel back = w.kit.MakePanel();
    skin::Paint(back, skin::Art::Card);
    skin::Pad(back, skin::Art::Card, {16, 12, 16, 12});
    w.box.Content(back).Align(HAlign::Fill, VAlign::Top);
    Box column = w.kit.MakeColumn();
    back.Content(column).Align(HAlign::Fill, VAlign::Top);
    w.head = MakeLine(column, Main());
    if (style::ClassicMenus()) {
        w.head.Set("> FIGHTER ENTRY");
    } else {
        w.head.Set("Fighter Entry");
        w.head.Font(20);
        skin::Font(w.head, skin::Face::Heading);
    }
    w.player = MakeLine(column, Bright());
    w.build = MakeLine(column, Main());
    w.mods = MakeLine(column, Main());
    if (!w.mods) { HSM_LOG("buildcard: could not make the widgets"); return false; }
    w.box.Show(false);
    HSM_LOG("buildcard: added to UI_HUD");
    return true;
}

// Sits on the UI_HUD that is on screen, as the chat does: the game swaps it on a respawn or a new round.
bool Attach() {
    const ULONGLONG now = GetTickCount64();
    if (!w.hudClass) {
        if (now < w.nextScan) return false;
        if (!(w.hudClass = FindClass("UI_HUD_C"))) { w.nextScan = now + kScanEveryMs; return false; }
    }
    if (!w.hud || ObjectAt(w.hudIndex) != w.hud || ClassOf(w.hud) != w.hudClass) {
        if (w.hud) { UClass* cls = w.hudClass; w = Widgets{}; w.hudClass = cls; }
        if (now < w.nextScan) return false;
        w.nextScan = now + kScanEveryMs;
        w.hud = OnScreen(FindInstances("UI_HUD_C"));
        if (!w.hud) return false;
        w.hudIndex = ObjectIndex(w.hud);
    }
    if (w.built && now >= w.nextLive) {
        w.nextLive = now + kScanEveryMs;
        if (!InViewport(w.hud))
            if (UObject* on = OnScreen(FindInstances("UI_HUD_C")); on && on != w.hud) {
                UClass* cls = w.hudClass; w = Widgets{}; w.hudClass = cls; w.hud = on; w.hudIndex = ObjectIndex(on);
                w.nextLive = now + kScanEveryMs;
            }
    }
    return w.built || Build();
}

}  // namespace

void BuildCardPush(BuildCard card) {
    HSM_LOG("buildcard: %s: build '%s', mods '%s'", card.player.c_str(), card.build.c_str(), card.mods.c_str());
    g_queue.Push(std::move(card));
}

void BuildCardClear() { g_queue.Clear(); }

void BuildCardToggle() { g_queue.Toggle(uint32_t(GetTickCount64())); }

void BuildCardTick(bool hudVisible) {
    if (g_queue.Idle() && !w.built) return;   // nothing to show: no name scans, no widgets
    if (!hudVisible) { (void)g_queue.Tick(uint32_t(GetTickCount64()), false); if (w.box) w.box.Show(false); return; }
    if (!Attach() || !w.box) return;
    const BuildCardView v = g_queue.Tick(uint32_t(GetTickCount64()), true);
    w.box.Show(v.visible);
    if (!v.visible) return;
    w.box.Opacity(v.opacity);
    const char* caret = v.typing ? "_" : "";
    w.player.Set(v.player + (v.typing && v.build.empty() ? caret : ""));
    w.build.Set(v.build + (v.typing && !v.build.empty() && v.mods.empty() ? caret : ""));
    w.mods.Set(v.mods + (v.typing && !v.mods.empty() ? caret : ""));
}

}  // namespace hsm::ui

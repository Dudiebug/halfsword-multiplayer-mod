#include "ui/pause_panel.h"
#include "ui/umg.h"
#include "ui/mp_widgets.h"
#include "ui/skin.h"
#include "config.h"
#include "gameplay/session.h"
#include "match/hsm_hud_style.h"
#include "log.h"
#include <windows.h>
#include <array>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr ULONGLONG kScanEveryMs = 1000;
constexpr int32_t kZOrder = 1000;       // over the HUD's own widgets, as the Esc menu was over the stock pause menu
constexpr float kSheetWidth = 660;
constexpr float kModBodyHeight = 440;

enum class View : uint8_t { Main, Multiplayer, ModSettings };

struct State {
    UClass* hudClass = nullptr;
    UObject* hud = nullptr; int32_t hudIndex = -1;
    ULONGLONG nextScan = 0, nextLive = 0;
    bool built = false, ready = false;
    Kit kit;
    mp::Maker maker;
    Canvas root;
    Panel panel;                                         // the dimmed screen, with the sheet in its middle
    Text title, hint;
    Box mainColumn, footer;
    Fixed modBox;                                        // the MOD SETTINGS page: a scroll held to a height
    std::array<mp::Flat, kPauseButtonCount> buttons;
    mp::Flat back, helpToggle, lookToggle;
    Text helpNote, lookNote;
    mp::VoiceBlock voice;
    mp::EscPanel multiplayer;                            // the MULTIPLAYER MENU page: the Esc menu's players, voice and network
    View view = View::Main;
    bool shown = false;
    PauseConfirm confirm;
    bool hitFixed = false;                               // the HUD and its canvas were made hit-testable for the panel
    uint8_t hudVis = 0, rootVis = 0;
    style::MenuLook look = style::MenuLook::Game; bool lookSaved = true;
} g;

void Forget() {
    UClass* cls = g.hudClass;
    g = State{};
    g.hudClass = cls;
}

uint8_t VisibilityOf(UObject* w) {
    Call v(w, "GetVisibility");
    return v.invoke() ? v.get<uint8_t>("ReturnValue") : uint8_t(Vis::Visible);
}
void SetVisibilityOf(UObject* w, Vis v) { Call(w, "SetVisibility").set<uint8_t>("InVisibility", uint8_t(v)).invoke(); }

// The HUD's widgets take no clicks (the HUD kits are HitTestInvisible: neither the widget nor its children are hit). While the
// panel is up its parents must let the clicks through to it: such a widget is made SelfHitTestInvisible (not itself hit, its
// children are), and goes back when the panel closes.
void HitTestOn() {
    if (g.hitFixed) return;
    g.hitFixed = true;
    g.hudVis = VisibilityOf(g.hud);
    g.rootVis = VisibilityOf(g.root.get());
    if (g.hudVis == uint8_t(Vis::HitTestInvisible)) SetVisibilityOf(g.hud, Vis::SelfHitTestInvisible);
    if (g.rootVis == uint8_t(Vis::HitTestInvisible)) SetVisibilityOf(g.root.get(), Vis::SelfHitTestInvisible);
    HSM_LOG("ui: pause panel: UI_HUD visibility %u, its canvas %u (HitTestInvisible is 3)%s", unsigned(g.hudVis), unsigned(g.rootVis),
            g.hudVis == uint8_t(Vis::HitTestInvisible) || g.rootVis == uint8_t(Vis::HitTestInvisible) ? ": made SelfHitTestInvisible while the panel is up" : ": left as it is");
}
void HitTestOff() {
    if (!g.hitFixed) return;
    g.hitFixed = false;
    if (g.hud && ObjectAt(g.hudIndex) == g.hud) {
        if (g.hudVis == uint8_t(Vis::HitTestInvisible)) SetVisibilityOf(g.hud, Vis::HitTestInvisible);
        if (g.rootVis == uint8_t(Vis::HitTestInvisible) && g.root) SetVisibilityOf(g.root.get(), Vis::HitTestInvisible);
    }
}

void SetView(View v) {
    g.view = v;
    g.panel.Show(v != View::Multiplayer);
    g.mainColumn.Show(v == View::Main);
    g.hint.Show(v == View::Main);
    g.modBox.Show(v == View::ModSettings);
    g.footer.Show(v == View::ModSettings);
    g.title.Set(skin::Title(v == View::ModSettings ? "Mod Settings" : "Paused"));
    if (v == View::Multiplayer) g.multiplayer.Show(true); else g.multiplayer.Show(false);
    if (v != View::ModSettings) g.voice.StopBinding();
    g.confirm.Clear();
}

void Hide() {
    if (!g.shown) return;
    g.shown = false;
    if (g.panel) g.panel.Show(false);
    g.multiplayer.Show(false);
    g.voice.StopBinding();
    g.confirm.Clear();
    g.view = View::Main;
    HitTestOff();
    HSM_LOG("pause: panel hidden");
}

bool Build() {
    g.built = true;
    g.kit = Kit(g.hud, Vis::Visible);
    g.root = g.kit.Root();
    if (!g.root) { HSM_LOG("ui: pause panel: UI_HUD root is not a CanvasPanel; the game's pause menu stays"); return false; }
    ProbeFont(g.kit, g.hud);
    g.maker = mp::Maker(&g.kit);
    mp::Maker& m = g.maker;
    g.panel = g.kit.MakePanel();
    Fixed holder = g.kit.MakeFixed();
    Panel sheet = g.kit.MakePanel();
    Box page = g.kit.MakeColumn(), header = g.kit.MakeRow();
    Fixed rule = g.kit.MakeFixed();
    Panel ruleLine = g.kit.MakePanel();
    g.mainColumn = g.kit.MakeColumn();
    g.modBox = g.kit.MakeFixed();
    Box scroll = g.kit.MakeScroll(), inner = g.kit.MakeColumn();
    g.footer = g.kit.MakeRow();
    if (!g.panel || !holder || !sheet || !page || !header || !rule || !ruleLine || !g.mainColumn || !g.modBox || !scroll || !inner || !g.footer) {
        HSM_LOG("ui: pause panel widgets could not be made; the game's pause menu stays");
        return false;
    }
    g.root.Add(g.panel).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(kZOrder);
    g.panel.Color({0, 0, 0, style::DarkMenus() ? 0.94f : 0.72f});   // the fight dimmed behind the sheet (dark look: nearly hidden)
    g.panel.Padding({40, 32, 40, 28});
    g.panel.Content(holder).Align(HAlign::Center, VAlign::Center);
    holder.Width(kSheetWidth);
    holder.Content(sheet).Align(HAlign::Fill, VAlign::Fill);
    skin::Paint(sheet, skin::Art::Sheet);
    skin::Pad(sheet, skin::Art::Sheet, {40, 22, 40, 22});
    sheet.Content(page).Align(HAlign::Fill, VAlign::Fill);

    mp::AddV(page, header, 0);
    g.title = m.NewText(skin::Title("Paused"), 44, style::kMenuAccentInk);
    if (g.title) mp::AddH(header, g.title, 1, 0).Align(HAlign::Left, VAlign::Center);
    rule.Height(3);
    rule.Content(ruleLine);
    ruleLine.Color(style::kMenuAccent);
    mp::AddV(page, rule, 12);

    // The list. Each button is a card of the multiplayer page's look, the width of the sheet.
    const auto order = PauseButtonOrder();
    for (size_t i = 0; i < g.buttons.size(); ++i) {
        mp::Flat& f = g.buttons[i];
        f = m.MakeFlat(PauseButtonLabel(order[i]), 21, {16, 9, 16, 9});
        if (!f.card) continue;
        f.card.face.ContentAlign(HAlign::Center, VAlign::Center);
        f.label.Align(Justify::Center);
        mp::AddV(g.mainColumn, f.card.button, i == 0 ? 0 : (order[i] == PauseButton::LeaveMatch ? 22 : 8));   // the leaving buttons stand apart
    }
    mp::AddV(page, g.mainColumn, 18);
    g.hint = m.NewText("Esc closes this.", 15, style::kMenuDim);
    if (g.hint) mp::AddV(page, g.hint, 14);

    // MOD SETTINGS: a scrolling page under the title, BACK in the footer.
    mp::AddV(page, g.modBox, 14);
    g.modBox.Height(kModBodyHeight);
    g.modBox.Content(scroll).Align(HAlign::Fill, VAlign::Fill);
    scroll.Add(inner).Padding({0, 0, 12, 0});
    m.Section(inner, "HELP IMPROVE THE MOD", 0);
    g.helpToggle = m.MakeFlat("HELP IMPROVE THE MOD: OFF", 17);
    if (g.helpToggle.card) mp::AddV(inner, g.helpToggle.card.button, 8).Align(HAlign::Left, VAlign::Top);
    g.helpNote = m.NewText("When on, your HSMMod log goes to the maintainers when someone in your lobby files a bug report or your game "
                           "crashes. Steam IDs, IPs and your Windows user name are removed; player names stay. Off unless you turn it on.",
                           14, style::kMenuDim);
    if (g.helpNote) { g.helpNote.Wrap(true); mp::AddV(inner, g.helpNote, 8); }
    m.Section(inner, "MENU LOOK", 28);
    g.look = style::CurrentMenuLook();
    g.lookToggle = m.MakeFlat("", 17);
    if (g.lookToggle.card) mp::AddV(inner, g.lookToggle.card.button, 8).Align(HAlign::Left, VAlign::Top);
    g.lookNote = m.NewText("", 14, style::kMenuDim);
    if (g.lookNote) { g.lookNote.Wrap(true); mp::AddV(inner, g.lookNote, 8); }
    g.voice.Build(m, inner, 28);                         // the same voice block as the multiplayer page
    mp::AddV(page, g.footer, 16);
    g.back = m.MakeFlat("BACK", 19);
    if (g.back.card) mp::AddH(g.footer, g.back.card.button, 0, 10).Align(HAlign::Left, VAlign::Center);

    // MULTIPLAYER MENU: the Esc menu's players, voice and network tabs, BACK and CHANGE LOADOUT in its footer.
    if (!g.multiplayer.Build(g.kit, g.root, mp::EscPanel::Layout::Pause)) {
        HSM_LOG("ui: pause panel: the multiplayer page could not be made; the game's pause menu stays");
        return false;
    }
    g.panel.Show(false);
    g.modBox.Show(false);
    g.footer.Show(false);
    g.ready = true;
    HSM_LOG("ui: pause panel built on UI_HUD");
    return true;
}

// Sits on the UI_HUD that is on screen, as the chat and the build card do: the game swaps it on a respawn or a new round.
bool Attach() {
    const ULONGLONG now = GetTickCount64();
    if (!g.hudClass) {
        if (now < g.nextScan) return false;
        if (!(g.hudClass = FindClass("UI_HUD_C"))) { g.nextScan = now + kScanEveryMs; return false; }
    }
    if (!g.hud || ObjectAt(g.hudIndex) != g.hud || ClassOf(g.hud) != g.hudClass) {
        if (g.hud) Forget();
        if (now < g.nextScan) return false;
        g.nextScan = now + kScanEveryMs;
        g.hud = OnScreen(FindInstances("UI_HUD_C"));
        if (!g.hud) return false;
        g.hudIndex = ObjectIndex(g.hud);
    }
    if (g.built && now >= g.nextLive) {
        g.nextLive = now + kScanEveryMs;
        if (!InViewport(g.hud))
            if (UObject* on = OnScreen(FindInstances("UI_HUD_C")); on && on != g.hud) {
                Forget();
                g.hud = on; g.hudIndex = ObjectIndex(on);
                g.nextLive = now + kScanEveryMs;
                HSM_LOG("ui: pause panel: UI_HUD replaced by the game; moving to the new one");
            }
    }
    return g.built ? g.ready : Build();
}

// The first click on a button with a confirm label arms it, the second does it. Returns true when `b` is to be carried out.
bool Pressed(PauseButton b) {
    const uint32_t now = GetTickCount();
    HSM_LOG("pause: button %s", PauseButtonLabel(b));
    const bool needs = PauseButtonConfirm(b) != nullptr;
    const bool done = g.confirm.Click(b, now);
    if (needs && !done) HSM_LOG("pause: %s waits for a second click", PauseButtonLabel(b));
    return !needs || done;
}

std::optional<PauseButton> TickList(const PausePanelWant& want) {
    const uint32_t now = GetTickCount();
    const auto order = PauseButtonOrder();
    std::optional<PauseButton> chosen;
    for (size_t i = 0; i < g.buttons.size(); ++i) {
        mp::Flat& f = g.buttons[i];
        if (!f.card) continue;
        const PauseButton b = order[i];
        const bool show = PauseButtonShown(b, want.reportOn);
        f.card.button.Show(show);
        if (!show) continue;
        const bool armed = g.confirm.Armed(b, now);
        f.label.Set(armed ? PauseButtonConfirm(b) : PauseButtonLabel(b));
        f.card.selected = armed;
        mp::Paint(f.card);
        if (chosen || !mp::Clicked(f.card)) continue;
        if (b == PauseButton::Multiplayer) { HSM_LOG("pause: button %s", PauseButtonLabel(b)); SetView(View::Multiplayer); return std::nullopt; }
        if (b == PauseButton::ModSettings) { HSM_LOG("pause: button %s", PauseButtonLabel(b)); SetView(View::ModSettings); return std::nullopt; }
        if (Pressed(b)) chosen = b;
    }
    return chosen;
}

void TickModSettings(Session& session, const PausePanelWant& want) {
    for (mp::Flat* f : {&g.back, &g.helpToggle, &g.lookToggle}) mp::Paint(f->card);
    // HELP IMPROVE THE MOD: the relay's switch, where bug reports exist (the multiplayer page shows it always).
    const bool help = session.HelpImprove();
    const bool helpShown = want.reportOn;
    if (g.helpToggle.card) g.helpToggle.card.button.Show(helpShown);
    if (g.helpNote) g.helpNote.Show(helpShown);
    if (helpShown) {
        g.helpToggle.label.Set(help ? "HELP IMPROVE THE MOD: ON" : "HELP IMPROVE THE MOD: OFF");
        g.helpToggle.label.Color(help ? style::kMenuOk : style::kMenuText);
        g.helpToggle.card.selected = help;
        if (mp::Clicked(g.helpToggle.card)) { session.SetHelpImprove(!help); HSM_LOG("pause: HELP IMPROVE THE MOD set %s", !help ? "on" : "off"); }
    }
    // MENU LOOK: written to HSMMod.ini, used from the next start (the widgets are built once).
    if (g.lookToggle.card) {
        if (mp::Clicked(g.lookToggle.card)) {
            g.look = g.look == style::MenuLook::Game ? style::MenuLook::Dark
                   : g.look == style::MenuLook::Dark ? style::MenuLook::Classic : style::MenuLook::Game;
            g.lookSaved = SaveUiStyle(g.look);
            HSM_LOG("pause: menu look set to %s for the next start%s", UiStyleName(g.look), g.lookSaved ? "" : " (HSMMod.ini could not be written)");
        }
        g.lookToggle.label.Set(g.look == style::MenuLook::Classic ? "MENU LOOK: CLASSIC"
                               : g.look == style::MenuLook::Game ? "MENU LOOK: GAME ART" : "MENU LOOK: DARK");
        const bool pending = g.look != style::CurrentMenuLook();
        g.lookNote.Set(!g.lookSaved ? "HSMMod.ini could not be written, so this will not stick. Set ui_style in it by hand."
                       : pending ? "Restart the game to see it."
                                 : "Game art: the game's own parchment and lettering. Dark: that art, darkened. Classic: the "
                                   "earlier look.");
        g.lookNote.Color(!g.lookSaved || pending ? style::kMenuWarn : style::kMenuDim);
    }
    g.voice.Tick(session);
    if (mp::Clicked(g.back.card)) { HSM_LOG("pause: button BACK"); SetView(View::Main); }
}

}  // namespace

bool PausePanelReady(bool onScreen) {
    return g.ready && g.hud && ObjectAt(g.hudIndex) == g.hud && (!onScreen || InViewport(g.hud));
}

std::optional<PauseButton> PausePanelTick(Session& session, const PausePanelWant& want) {
    if (!want.match) {                                   // not in a session match: nothing is kept up
        if (g.shown) Hide();
        return std::nullopt;
    }
    if (!Attach()) return std::nullopt;
    if (!want.open || want.hidden) { Hide(); return std::nullopt; }
    if (!g.shown) {
        g.shown = true;
        HitTestOn();
        SetView(View::Main);
        HSM_LOG("pause: panel shown");
    }
    if (g.view == View::Multiplayer) {
        if (g.multiplayer.Tick(session, want.reportOn) == mp::EscPanel::Action::Back) { HSM_LOG("pause: button BACK"); SetView(View::Main); }
        return std::nullopt;
    }
    if (g.view == View::ModSettings) { TickModSettings(session, want); return std::nullopt; }
    return TickList(want);
}

}  // namespace hsm::ui

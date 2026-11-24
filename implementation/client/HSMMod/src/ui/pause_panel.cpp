#include "ui/pause_panel.h"
#include "ui/umg.h"
#include "ui/dropdown.h"
#include "ui/hud_host.h"
#include "ui/mp_widgets.h"
#include "ui/skin.h"
#include "ui/notices.h"
#include "config.h"
#include "gameplay/session.h"
#include "match/hsm_hud_style.h"
#include "log.h"
#include <windows.h>
#include <array>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr int32_t kZOrder = 1000;       // over the HUD's own widgets, as the Esc menu was over the stock pause menu
constexpr float kSheetWidth = 660;
constexpr float kModBodyHeight = 440;
constexpr float kPlateWidth = 230;       // a dropdown's closed control (the voice rows' value width)

enum class View : uint8_t { Main, Players, ModSettings };

HudHost host{"ui: pause panel"};   // the UI_HUD the panel sits on (kept when the panel is made again)

struct State {
    bool built = false, ready = false;
    Kit kit;
    mp::Maker maker;
    Canvas root;
    Panel panel;                                         // the dimmed screen, with the sheet in its middle
    Text title, titleNote, hint;                         // the hint is the line under the sheet (match/hsm_pause_panel.h PauseHintLine)
    Box mainColumn, footer;
    Fixed modBox;                                        // the MOD SETTINGS page: a scroll held to a height
    std::array<mp::Flat, kPauseButtonCount> buttons;
    mp::Flat back, helpToggle, lookToggle, lookPlate;   // MENU LOOK is a button, or a dropdown's plate while the lists are built
    DropdownLayer drops;                                 // the open list for MENU LOOK and the voice rows
    Text helpNote, lookNote;
    mp::VoiceBlock voice;
    mp::EscPanel multiplayer;                            // the PLAYERS page: the Esc menu's players and network
    View view = View::Main;
    bool shown = false;
    PauseConfirm confirm;
    bool hitFixed = false;                               // the HUD and its canvas were made hit-testable for the panel
    uint8_t hudVis = 0, rootVis = 0;
    style::MenuLook look = style::MenuLook::Game; bool lookSaved = true;
} g;

void Forget() { g = State{}; }

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
    g.hudVis = VisibilityOf(host.Obj());
    g.rootVis = VisibilityOf(g.root.get());
    if (g.hudVis == uint8_t(Vis::HitTestInvisible)) SetVisibilityOf(host.Obj(), Vis::SelfHitTestInvisible);
    if (g.rootVis == uint8_t(Vis::HitTestInvisible)) SetVisibilityOf(g.root.get(), Vis::SelfHitTestInvisible);
    HSM_LOG("ui: pause panel: UI_HUD visibility %u, its canvas %u (HitTestInvisible is 3)%s", unsigned(g.hudVis), unsigned(g.rootVis),
            g.hudVis == uint8_t(Vis::HitTestInvisible) || g.rootVis == uint8_t(Vis::HitTestInvisible) ? ": made SelfHitTestInvisible while the panel is up" : ": left as it is");
}
void HitTestOff() {
    if (!g.hitFixed) return;
    g.hitFixed = false;
    if (host.Alive()) {
        if (g.hudVis == uint8_t(Vis::HitTestInvisible)) SetVisibilityOf(host.Obj(), Vis::HitTestInvisible);
        if (g.rootVis == uint8_t(Vis::HitTestInvisible) && g.root) SetVisibilityOf(g.root.get(), Vis::HitTestInvisible);
    }
}

void SetView(View v) {
    g.view = v;
    g.panel.Show(v != View::Players);
    g.mainColumn.Show(v == View::Main);
    g.titleNote.Show(v == View::Main);
    g.hint.Show(v == View::Main);
    g.modBox.Show(v == View::ModSettings);
    g.footer.Show(v == View::ModSettings);
    g.title.Set(skin::Title(v == View::ModSettings ? "Mod Settings" : kPauseTitle));
    if (v == View::Players) g.multiplayer.Show(true); else g.multiplayer.Show(false);
    if (v != View::ModSettings) g.voice.StopBinding();
    g.drops.Close("page");
    g.confirm.Clear();
}

void Hide() {
    if (!g.shown) return;
    g.shown = false;
    if (g.panel) g.panel.Show(false);
    g.multiplayer.Show(false);
    g.voice.StopBinding();
    g.drops.Close("page");
    g.confirm.Clear();
    g.view = View::Main;
    HitTestOff();
    HSM_LOG("pause: panel hidden");
}

bool Build() {
    g.built = true;
    g.kit = Kit(host.Obj(), Vis::Visible);
    g.root = g.kit.Root();
    if (!g.root) { HSM_LOG("ui: pause panel: UI_HUD root is not a CanvasPanel; the game's pause menu stays"); return false; }
    ProbeFont(g.kit, host.Obj());
    g.maker = mp::Maker(&g.kit);
    mp::Maker& m = g.maker;
    g.panel = g.kit.MakePanel();
    Fixed holder = g.kit.MakeFixed();
    Panel sheet = g.kit.MakePanel();
    Box stack = g.kit.MakeColumn();                      // the sheet, and the hint line under it
    Box page = g.kit.MakeColumn(), header = g.kit.MakeRow();
    Fixed rule = g.kit.MakeFixed();
    Panel ruleLine = g.kit.MakePanel();
    g.mainColumn = g.kit.MakeColumn();
    g.modBox = g.kit.MakeFixed();
    Box scroll = g.kit.MakeScroll(), inner = g.kit.MakeColumn();
    g.footer = g.kit.MakeRow();
    if (!g.panel || !holder || !sheet || !stack || !page || !header || !rule || !ruleLine || !g.mainColumn || !g.modBox || !scroll || !inner || !g.footer) {
        HSM_LOG("ui: pause panel widgets could not be made; the game's pause menu stays");
        return false;
    }
    g.root.Add(g.panel).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(kZOrder);
    g.panel.Color({0, 0, 0, style::DarkMenus() ? 0.94f : 0.72f});   // the fight dimmed behind the sheet (dark look: nearly hidden)
    g.panel.Padding({40, 32, 40, 28});
    g.panel.Content(holder).Align(HAlign::Center, VAlign::Center);
    holder.Width(kSheetWidth);
    holder.Content(stack).Align(HAlign::Fill, VAlign::Fill);
    mp::AddV(stack, sheet, 0);
    skin::Paint(sheet, skin::Art::Sheet);
    skin::Pad(sheet, skin::Art::Sheet, {40, 22, 40, 22});
    sheet.Content(page).Align(HAlign::Fill, VAlign::Fill);

    mp::AddV(page, header, 0);
    g.title = m.NewText(skin::Title(kPauseTitle), style::type::kPage, style::kMenuAccentInk);
    if (g.title) mp::AddH(header, g.title, 1, 0).Align(HAlign::Left, VAlign::Center);
    g.titleNote = m.NewText(kPauseTitleNote, style::type::kEmphasis, style::kMenuDim);      // the world runs on behind the menu: said once, under the title
    if (g.titleNote) { g.titleNote.Wrap(true); mp::AddV(page, g.titleNote, 4); }
    rule.Height(3);
    rule.Content(ruleLine);
    ruleLine.Color(style::kMenuAccent);
    mp::AddV(page, rule, 12);

    // The list. Each button is a card of the multiplayer page's look, the width of the sheet.
    const auto order = PauseButtonOrder();
    for (size_t i = 0; i < g.buttons.size(); ++i) {
        mp::Flat& f = g.buttons[i];
        f = m.MakeFlat(PauseButtonLabel(order[i]), style::type::kEmphasis, {16, 9, 16, 9});
        if (!f.card) continue;
        f.card.face.ContentAlign(HAlign::Center, VAlign::Center);
        f.label.Align(Justify::Center);
        mp::AddV(g.mainColumn, f.card.button, PauseButtonGapBefore(order[i]));   // the leaving buttons stand apart
    }
    mp::AddV(page, g.mainColumn, 18);
    g.hint = m.NewText(kPauseDefaultHint, style::type::kBody, style::kMenuText);       // on the dimmed fight, not on the sheet: light, with a shadow
    if (g.hint) {
        g.hint.Wrap(true);
        g.hint.Align(Justify::Center);
        g.hint.Shadow({1, 1}, {0, 0, 0, 0.9f});
        mp::AddV(stack, g.hint, 14);
    }

    // MOD SETTINGS: a scrolling page under the title, BACK in the footer.
    mp::AddV(page, g.modBox, 14);
    g.modBox.Height(kModBodyHeight);
    g.modBox.Content(scroll).Align(HAlign::Fill, VAlign::Fill);
    scroll.Add(inner).Padding({0, 0, 12, 0});
    m.Section(inner, "HELP IMPROVE THE MOD", 0);
    g.helpToggle = m.MakeFlat("HELP IMPROVE THE MOD: OFF", style::type::kRow);
    if (g.helpToggle.card) mp::AddV(inner, g.helpToggle.card.button, 8).Align(HAlign::Left, VAlign::Top);
    g.helpNote = m.NewText("Sends your HSMMod log if someone in your lobby files a bug report or the game crashes.\n"
                           "Steam IDs, IPs and your Windows user name are removed; player names stay.",
                           style::type::kHint, style::kMenuDim);
    if (g.helpNote) { g.helpNote.Wrap(true); mp::AddV(inner, g.helpNote, 8); }
    m.Section(inner, "MENU LOOK", 28);
    g.look = style::CurrentMenuLook();
    if (!g.drops.Build(g.kit, g.root, kZOrder + 1)) HSM_LOG("ui: pause panel: the lists could not be made; MENU LOOK and the voice settings keep their buttons");
    if (g.drops) {                                       // MENU LOOK picks from a list (docs/UI_STYLE.md Dropdowns)
        g.lookPlate = m.MakePlate(style::type::kRow);
        Fixed plateBox = g.kit.MakeFixed();
        if (g.lookPlate.card && plateBox) {
            plateBox.Width(kPlateWidth);
            plateBox.Content(g.lookPlate.card.button).Align(HAlign::Fill, VAlign::Fill);
            mp::AddV(inner, plateBox, 8).Align(HAlign::Left, VAlign::Top);
        } else {
            g.lookPlate = mp::Flat{};
        }
    }
    if (!g.lookPlate.card) {
        g.lookToggle = m.MakeFlat("", style::type::kRow);
        if (g.lookToggle.card) mp::AddV(inner, g.lookToggle.card.button, 8).Align(HAlign::Left, VAlign::Top);
    }
    g.lookNote = m.NewText("", style::type::kHint, style::kMenuDim);
    if (g.lookNote) { g.lookNote.Wrap(true); mp::AddV(inner, g.lookNote, 8); }
    g.voice.Build(m, inner, 28, g.drops ? &g.drops : nullptr);   // the same voice block as the multiplayer page
    mp::AddV(page, g.footer, 16);
    g.back = m.MakeFlat(kPauseBackLabel, style::type::kEmphasis);
    if (g.back.card) mp::AddH(g.footer, g.back.card.button, 0, 10).Align(HAlign::Left, VAlign::Center);

    // PLAYERS: the Esc menu's players and network tabs, BACK TO MENU in its footer.
    if (!g.multiplayer.Build(g.kit, g.root, mp::EscPanel::Layout::Pause)) {
        HSM_LOG("ui: pause panel: the players page could not be made; the game's pause menu stays");
        return false;
    }
    g.panel.Show(false);
    g.modBox.Show(false);
    g.footer.Show(false);
    g.ready = true;
    HSM_LOG("ui: pause panel built on UI_HUD");
    return true;
}

// Sits on the UI_HUD that is on screen: the game swaps it on a respawn or a new round, and the panel went with the old one.
bool Attach() {
    if (host.Follow(true, g.built) == HudHost::Step::Changed) Forget();
    if (!host.Obj()) return false;
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

// Dimmed, not disabled: a disabled button never sees the click, so it could not say why it does nothing.
void Dimmed(mp::Flat& f, bool on) {
    f.card.live = on;
    f.card.button.Enabled(true);
    f.card.button.Opacity(on ? 1.f : style::kDisabledAlpha);
}

// The main list: what each row shows comes from PauseRowOf (match/hsm_pause_panel.h), the line under the sheet from PauseHintLine.
std::optional<PauseButton> TickList(Session& session, const PausePanelWant& want) {
    const uint32_t now = GetTickCount();
    const auto order = PauseButtonOrder();
    const MpLoadout lv = session.LoadoutView();
    PauseRowIn in;
    in.reportOn = want.reportOn; in.loadoutTrial = want.loadoutTrial; in.host = want.host; in.cryptsRun = want.cryptsRun;
    in.lobby = want.lobby; in.coop = want.coop;
    in.changeShown = lv.shown; in.changeAvailable = lv.available; in.changeWhy = lv.hint;
    std::optional<PauseButton> chosen;
    bool hovering = false;
    PauseButton hover = PauseButton::Resume;
    std::string hoverHint;
    for (size_t i = 0; i < g.buttons.size(); ++i) {
        mp::Flat& f = g.buttons[i];
        if (!f.card) continue;
        const PauseButton b = order[i];
        const PauseRow row = PauseRowOf(b, in);
        f.card.button.Show(row.shown);
        if (!row.shown) continue;
        Dimmed(f, row.enabled);
        if (!row.enabled && g.confirm.Armed(b, now)) g.confirm.Clear();   // it was armed, then the rule said no
        const bool armed = g.confirm.Armed(b, now);
        f.label.Set(armed ? PauseButtonConfirm(b, want.host) : PauseButtonLabel(b));
        f.card.selected = armed;
        f.label.Color(mp::InkOn(f.card, style::kMenuText));
        mp::Paint(f.card);
        if (f.card.button.Hovered()) { hovering = true; hover = b; hoverHint = row.hint; }
        if (chosen || !mp::Clicked(f.card)) continue;
        if (!row.enabled) {                              // a click on a dimmed button says why, and does nothing else
            g.confirm.Clear();
            HSM_LOG("pause: a click on %s, which is dimmed: %s", PauseButtonLabel(b), row.why.text.empty() ? "(no reason)" : row.why.text.c_str());
            if (!row.why.text.empty()) Notify(row.why);
            continue;
        }
        if (b == PauseButton::Players) { HSM_LOG("pause: button %s", PauseButtonLabel(b)); SetView(View::Players); return std::nullopt; }
        if (b == PauseButton::ModSettings) { HSM_LOG("pause: button %s", PauseButtonLabel(b)); SetView(View::ModSettings); return std::nullopt; }
        if (Pressed(b)) chosen = b;
    }
    if (g.hint) g.hint.Set(PauseHintLine(g.confirm, now, hovering, hover, hoverHint, want.host));
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
        g.helpToggle.card.selected = help;
        g.helpToggle.label.Color(mp::InkOn(g.helpToggle.card, style::kMenuText));
        if (mp::Clicked(g.helpToggle.card)) { session.SetHelpImprove(!help); HSM_LOG("pause: HELP IMPROVE THE MOD set %s", !help ? "on" : "off"); }
    }
    // MENU LOOK: written to HSMMod.ini, used from the next start (the widgets are built once).
    if (g.lookToggle.card || g.lookPlate.card) {
        auto setLook = [](style::MenuLook look) {
            g.look = look;
            g.lookSaved = SaveUiStyle(g.look);
            HSM_LOG("pause: menu look set to %s for the next start%s", UiStyleName(g.look), g.lookSaved ? "" : " (HSMMod.ini could not be written)");
        };
        if (g.lookPlate.card) {
            const uint32_t owner = dropdown_owner::kMenuLook;
            const bool plateClicked = mp::Clicked(g.lookPlate.card);
            size_t pick = 0;
            if (g.drops.TakePick(owner, pick)) setLook(style::MenuLookPicked(pick));
            if (plateClicked) g.drops.Open(owner, "MENU LOOK", g.lookPlate.card.button, style::MenuLookChoices(), style::MenuLookChoiceAt(g.look));
            mp::PaintPlate(g.lookPlate, style::MenuLookName(g.look), g.drops.IsOpen(owner), true);
            mp::Paint(g.lookPlate.card);
        } else if (mp::Clicked(g.lookToggle.card)) {
            setLook(style::NextMenuLook(g.look));
        }
        const bool pending = g.look != style::CurrentMenuLook();
        if (g.lookToggle.card) g.lookToggle.label.Set(PauseLookLabel(g.look, pending));
        const std::string lookSays = !g.lookSaved ? "The menu look could not be saved, so it will not stick."
                                   : pending ? "Restart the game to see it." : "";   // no note until a look is picked
        g.lookNote.Set(lookSays);
        g.lookNote.Show(!lookSays.empty());
        g.lookNote.Color(!g.lookSaved || pending ? style::kMenuWarn : style::kMenuDim);
    }
    g.voice.Tick(session);
    if (mp::Clicked(g.back.card)) { HSM_LOG("pause: button BACK"); SetView(View::Main); }
}

}  // namespace

bool PausePanelReady(bool onScreen) {
    return g.ready && host.Alive() && (!onScreen || InViewport(host.Obj()));
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
    g.drops.Tick(uint32_t(g.view));                      // an open list takes its click first; another page closes it
    if (g.view == View::Players) {
        if (g.multiplayer.Tick(session, want.reportOn) == mp::EscPanel::Action::Back) { HSM_LOG("pause: button BACK"); SetView(View::Main); }
        return std::nullopt;
    }
    if (g.view == View::ModSettings) { TickModSettings(session, want); return std::nullopt; }
    return TickList(session, want);
}

}  // namespace hsm::ui

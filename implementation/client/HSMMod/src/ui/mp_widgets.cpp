#include "ui/mp_widgets.h"
#include "ui/dropdown.h"
#include "ui/skin.h"
#include "gameplay/fetch_link.h"
#include "gameplay/session.h"
#include "match/hsm_hud_style.h"
#include "match/hsm_pause_panel.h"
#include "log.h"
#include <windows.h>
#include <algorithm>

namespace hsm::ui::mp {
namespace {
constexpr float kGlowStep = 0.18f;            // the hover fade: a share of the way per frame (about 0.1 s at 60 fps)
constexpr Rgba kClear{1, 1, 1, 0};
constexpr Rgba kNone{0, 0, 0, 0};
constexpr float kMeterWidth = 300;
constexpr uint32_t kConfirmMs = 4000;
}  // namespace

bool KeyDown(int vk) {
    if (!(GetAsyncKeyState(vk) & 0x8000)) return false;   // the key first: asking whose window is in front costs more
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// ---------------------------------------------------------------- the card look

Text Maker::NewText(const std::string& text, float size, Rgba color) {
    Text t = kit_ ? kit_->MakeText() : Text{};
    if (!t) return t;
    t.Set(text);
    if (style::DarkMenus()) t.Font(size, 1, style::kOutlineInk); else t.Font(size);   // dark: the scene shows behind
    skin::Font(t, style::type::IsHeading(size) ? skin::Face::Heading : skin::Face::Body);   // the typeface follows the role
    t.Color(color);
    return t;
}

Text Maker::Section(Box& column, const std::string& text, float padTop) {
    Text t = NewText(skin::Title(text), style::type::kSection, style::kMenuAccentInk);   // a rubric, as on the game's scrolls
    if (t) AddV(column, t, padTop);
    return t;
}

Card Maker::MakeCard(const Widget& body, Margin pad) {
    if (!kit_) return Card{};
    Card c{kit_->MakeButton(), kit_->MakePanel(), kit_->MakePanel()};
    if (!c || !body) { HSM_LOG("ui: card widgets could not be made"); return Card{}; }
    c.button.Background(kClear);
    c.button.Content(c.frame).Padding({0, 0, 0, 0}).Align(HAlign::Fill, VAlign::Fill);
    c.frame.Content(c.face);
    c.face.Color(kNone);
    c.face.Padding(pad);
    c.face.Content(body);
    return c;
}

Flat Maker::MakeFlat(const std::string& text, float size, Margin pad) {
    Flat f;
    f.label = NewText(text, size, style::kMenuText);
    f.card = MakeCard(f.label, pad);
    return f;
}

Flat Maker::MakeArrow(bool next, float size) {
    // points left, as the game's Options book draws "previous"; the classic look keeps its text arrows
    ue::UObject* art = style::ClassicMenus() ? nullptr : GameTexture("Arrow_001");
    if (!art || !kit_) return MakeFlat(next ? ">" : "<", size, {14, 2, 14, 2});
    Flat f;
    Box stack = kit_->MakeStack();
    Icon arrow = kit_->MakeIcon();
    f.label = NewText("", size, style::kMenuText);
    if (!stack || !arrow || !f.label) return MakeFlat(next ? ">" : "<", size, {14, 2, 14, 2});
    arrow.Texture(Look{art, {}, {}, false, next});
    arrow.Size({size * 2.2, size * 1.7});
    stack.Add(arrow).Align(HAlign::Center, VAlign::Center);
    stack.Add(f.label).Align(HAlign::Center, VAlign::Center);
    f.card = MakeCard(stack, {2, 0, 2, 0});
    if (f.card) f.card.arrow = true;
    return f;
}

Flat Maker::MakePlate(float size) {
    Flat f = MakeFlat("", size, {12, 5, 12, 5});
    if (!f.card) return f;
    f.label.Align(Justify::Center);
    f.card.face.ContentAlign(HAlign::Center, VAlign::Center);
    return f;
}

Rgba Mix(Rgba a, Rgba b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }

void Paint(Card& c) {
    if (!c) return;
    const bool hover = c.live && (c.button.Hovered() || c.button.Focused());   // the highlight follows the keys and the gamepad too
    c.glow += ((hover ? 1.f : 0.f) - c.glow) * kGlowStep;
    if (c.glow < 0.01f) c.glow = 0; else if (c.glow > 0.99f) c.glow = 1;
    if (c.arrow) {                              // a bare arrow: no card behind it, a little larger under the mouse
        c.frame.Padding({0, 0, 0, 0});
        c.frame.Color(kNone);
        c.face.Color(kNone);
        c.button.Scale(1 + 0.12f * c.glow);
        return;
    }
    // The game's parchment button, brighter under the mouse; the chosen one is its amber pressed button. Without the
    // art: the plain card colours, the chosen one framed in the rubric.
    if (skin::Loaded(skin::Art::Card)) {
        skin::Pad(c.frame, skin::Art::Card, {6, 6, 6, 6});   // inside the gilt frame and its corner studs
        skin::Paint(c.frame, c.selected ? skin::Art::CardPicked : skin::Art::Card, c.selected ? Rgba{1, 1, 1, 1} : skin::CardTint(c.glow));
        return;
    }
    c.frame.Padding({2, 2, 2, 2});
    c.frame.Texture(Look{});
    const bool picked = c.selected && !style::ClassicMenus();   // the game and dark looks paint the whole card in the picked colour
    c.frame.Color(c.selected ? style::kMenuPickedFrame : Mix(style::kMenuFrame, style::kMenuFrameHover, c.glow));
    c.face.Color(picked ? style::kMenuPicked : Mix(style::kMenuCard, style::kMenuCardHover, c.glow));
}

bool Clicked(Card& c) { return c && c.button.Clicked(); }

void Live(Flat& f, bool on) {
    if (!f.card) return;
    f.card.live = on;
    f.card.button.Enabled(on);
    f.card.button.Opacity(on ? 1.f : style::kDisabledAlpha);
}

void Dimmed(Card& c, bool on) {
    if (!c) return;
    c.live = on;
    c.button.Enabled(true);
    c.button.Opacity(on ? 1.f : style::kDisabledAlpha);
}

void PaintPlate(Flat& f, const std::string& value, bool open, bool usable) {
    if (!f.card) return;
    f.label.Set(usable ? value + (open ? "  ^" : "  v") : value);
    f.card.selected = open && usable;
    f.label.Color(InkOn(f.card, usable ? style::kMenuText : style::kMenuDim));
    Dimmed(f.card, usable);
}

Slot AddV(Box& column, const Widget& child, float padTop) { return column.Add(child).Padding({0, padTop, 0, 0}); }

Slot AddH(Box& row, const Widget& child, float fill, float padRight) {
    Slot s = row.Add(child);
    if (fill > 0) s.Fill(fill); else s.Auto();
    s.Padding({0, 0, padRight, 0});
    return s;
}

// ---------------------------------------------------------------- the voice block

bool VoiceBlock::Build(Maker& m, Box& column, float padTop, DropdownLayer* drops) {
    Kit& kit = m.kit();
    drops_ = drops;
    root_ = kit.MakeColumn();
    if (!root_) { HSM_LOG("ui: voice block widgets could not be made"); return false; }
    AddV(column, root_, padTop);
    m.Section(root_, "VOICE CHAT", 0);
    for (size_t k = 0; k < rows_.size(); ++k) {
        RowView& v = rows_[k];
        const bool list = drops && *drops && VoiceRowIsChoice(VoiceRow(k));   // voice chat, microphone and device pick from a list
        v.box = kit.MakeColumn();
        Box row = kit.MakeRow();
        Fixed valueBox = kit.MakeFixed();
        v.label = m.NewText("", style::type::kRow, style::kMenuText); v.value = m.NewText("", style::type::kRow, style::kMenuText); v.hint = m.NewText("", style::type::kHint, style::kMenuDim);
        v.prev = m.MakeArrow(false, style::type::kRow); v.next = m.MakeArrow(true, style::type::kRow);
        v.bind = m.MakeFlat("CHANGE", style::type::kBody, {12, 3, 12, 3});
        if (list) v.plate = m.MakePlate(style::type::kRow);
        if (!v.box || !row || !valueBox || !v.label || !v.value || !v.hint || !v.prev.card || !v.next.card || !v.bind.card || (list && !v.plate.card)) { v = RowView{}; continue; }
        v.value.Align(Justify::Center);
        v.hint.Wrap(true);
        valueBox.Width(230);
        if (list) {
            valueBox.Content(v.plate.card.button).Align(HAlign::Fill, VAlign::Center);
            v.value = Text{};                    // the plate shows the value; a text in no tree is freed by garbage collection, so it is never written
        } else {
            valueBox.Content(v.value).Align(HAlign::Fill, VAlign::Center);
        }
        AddH(row, v.label, 1, 12).Align(HAlign::Fill, VAlign::Center);
        AddH(row, v.prev.card.button, 0, 6);
        AddH(row, valueBox, 0, 6).Align(HAlign::Fill, VAlign::Center);
        AddH(row, v.next.card.button, 0, 6);
        AddH(row, v.bind.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        AddV(v.box, row, 0);
        AddV(v.box, v.hint, 2);
        AddV(root_, v.box, 8);
    }
    Box meterRow = kit.MakeRow();
    Fixed meterBox = kit.MakeFixed();
    Box stack = kit.MakeStack();
    meter_ = kit.MakeBar();
    mark_ = kit.MakeIcon();
    meterText_ = m.NewText("", style::type::kHint, style::kMenuDim);
    if (meterRow && meterBox && stack && meter_ && mark_ && meterText_) {
        meterBox.Width(kMeterWidth);
        meterBox.Height(14);
        meterBox.Content(stack).Align(HAlign::Fill, VAlign::Fill);
        stack.Add(meter_).Align(HAlign::Fill, VAlign::Fill);
        markSlot_ = stack.Add(mark_);
        markSlot_.Align(HAlign::Left, VAlign::Fill);
        mark_.Size({3, 14});
        mark_.Color(style::kMenuText);
        AddH(meterRow, meterBox, 0, 12).Align(HAlign::Left, VAlign::Center);
        AddH(meterRow, meterText_, 1, 0).Align(HAlign::Fill, VAlign::Center);
        AddV(root_, meterRow, 12);
    } else {
        meter_ = Bar{};
    }
    note_ = m.NewText("", style::type::kHint, style::kMenuAccentInk);
    if (note_) { note_.Wrap(true); AddV(root_, note_, 8); }
    return true;
}

void VoiceBlock::Show(bool on) {
    if (root_) root_.Show(on);
    if (!on) binding_ = false;
}

// Push to talk: the next key pressed (a letter, a digit, F1-F12 or a side mouse button) becomes the key; Esc cancels.
void VoiceBlock::PollBinding(Session& session, const MpVoice& v) {
    if (v.mic != voice::Mic::PushToTalk || KeyDown(VK_ESCAPE)) { binding_ = false; return; }
    for (int vk : voice::VoiceKeyCandidates()) {
        const bool down = KeyDown(vk);
        const auto held = std::find(heldAtBind_.begin(), heldAtBind_.end(), vk);
        if (held != heldAtBind_.end()) { if (!down) heldAtBind_.erase(held); continue; }
        if (!down) continue;
        MpVoice n = v;
        n.key = vk;
        session.SetVoicePrefs(n);
        HSM_LOG("voice: push-to-talk key set to %s from the menu", voice::VoiceKeyIni(vk).c_str());
        binding_ = false;
        return;
    }
}

void VoiceBlock::Tick(Session& session) {
    if (!root_) return;
    MpVoice v = session.VoiceView();
    if (binding_ && game::fetchlink::KeysHeld()) binding_ = false;   // Fetch's menu took the keyboard: its keys are not a binding
    if (binding_) { PollBinding(session, v); if (!binding_) v = session.VoiceView(); }
    const VoiceScene sc = BuildVoiceScene(v, binding_);
    for (size_t i = 0; i < rows_.size(); ++i) {
        RowView& r = rows_[i];
        if (!r.box) continue;
        Paint(r.prev.card); Paint(r.next.card); Paint(r.bind.card); Paint(r.plate.card);
        int dir = Clicked(r.prev.card) ? -1 : 0;
        if (Clicked(r.next.card)) dir = 1;
        const bool bindClicked = Clicked(r.bind.card);
        const bool plateClicked = Clicked(r.plate.card);
        const VoiceRowView* row = nullptr;
        for (const VoiceRowView& x : sc.rows) if (size_t(x.id) == i) row = &x;
        r.box.Show(row != nullptr);
        if (!row) continue;
        const bool key = row->id == VoiceRow::Key;
        r.label.Set(row->label);
        r.label.Color(row->enabled ? style::kMenuText : style::kMenuDim);
        r.value.Set(row->value);
        r.value.Color(key && binding_ ? style::kMenuAccentInk : style::kMenuText);
        r.hint.Set(row->hint);
        r.hint.Show(!row->hint.empty());
        const bool list = bool(r.plate.card);
        const uint32_t owner = dropdown_owner::kVoiceRow + uint32_t(i);
        r.prev.card.button.Show(!key && !list);
        r.next.card.button.Show(!key && !list);
        r.plate.card.button.Show(list);
        r.bind.card.button.Show(key);
        r.bind.label.Set(binding_ ? "CANCEL" : "CHANGE");
        r.bind.card.selected = binding_;
        r.bind.label.Color(InkOn(r.bind.card, style::kMenuText));
        Live(r.prev, row->enabled);
        Live(r.next, row->enabled);
        if (list) {                              // a dropdown: the plate opens the list, a pick sets the option
            PaintPlate(r.plate, row->value, drops_->IsOpen(owner), row->enabled);
            if (plateClicked && row->enabled) drops_->Open(owner, row->label, r.plate.card.button, row->choices, row->choice);
            size_t pick = 0;
            if (drops_->TakePick(owner, pick) && row->enabled) {
                const MpVoice n = WithVoiceChoice(v, row->id, pick);
                if (row->id == VoiceRow::Reach) session.SetVoiceReach(n.reach); else session.SetVoicePrefs(n);
            }
            continue;
        }
        if (key && bindClicked) {
            binding_ = !binding_;
            heldAtBind_.clear();
            if (binding_) for (int vk : voice::VoiceKeyCandidates()) if (KeyDown(vk)) heldAtBind_.push_back(vk);
        }
        if (!dir || !row->enabled || key) continue;
        const MpVoice n = StepVoice(v, row->id, dir);
        switch (row->id) {
            case VoiceRow::Reach: session.SetVoiceReach(n.reach); break;
            case VoiceRow::Monitor: session.SetVoiceMonitor(n.monitor); break;
            default: session.SetVoicePrefs(n); break;
        }
    }
    if (meter_) {
        meter_.Percent(sc.meter);
        meter_.FillColor(sc.meterHot ? style::kMenuOk : style::kMenuFrame);
        mark_.Visibility(sc.showThreshold ? Vis::HitTestInvisible : Vis::Hidden);
        const float at = std::clamp(sc.threshold, 0.f, 1.f) * (kMeterWidth - 3);
        if (markSlot_ && std::abs(at - markAt_) > 0.5f) { markAt_ = at; markSlot_.Padding({at, 0, 0, 0}); }
    }
    if (meterText_) meterText_.Set(sc.meterText);
    if (note_) { note_.Set(sc.note); note_.Show(!sc.note.empty()); }
}

// ---------------------------------------------------------------- the Esc menu

bool EscPanel::Build(Kit& kit, Canvas root, Layout layout) {
    m_ = Maker(&kit);
    layout_ = layout;
    panel_ = kit.MakePanel();
    Panel sheet = kit.MakePanel();
    Box page = kit.MakeColumn(), header = kit.MakeRow(), tabs = kit.MakeRow(), body = kit.MakeScroll(), footer = kit.MakeRow();
    Fixed rule = kit.MakeFixed();
    Panel ruleLine = kit.MakePanel();
    for (Box& p : pages_) p = kit.MakeColumn();
    bool pagesOk = true;
    for (Box& p : pages_) pagesOk = pagesOk && bool(p);
    if (!root || !panel_ || !sheet || !page || !header || !tabs || !body || !footer || !rule || !ruleLine || !pagesOk) {
        HSM_LOG("ui: Esc menu widgets could not be made");
        panel_ = Panel{};
        return false;
    }
    root.Add(panel_).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(1000);
    // The fight dimmed behind the game's old paper, as its own pause scroll sits over it (dark look: hidden, as a menu).
    panel_.Color({0, 0, 0, style::DarkMenus() ? 0.94f : 0.72f});
    panel_.Padding({56, 32, 56, 28});
    panel_.Content(sheet).Align(HAlign::Fill, VAlign::Fill);
    skin::Paint(sheet, skin::Art::Sheet);
    skin::Pad(sheet, skin::Art::Sheet, {40, 22, 40, 20});
    sheet.Content(page).Align(HAlign::Fill, VAlign::Fill);

    AddV(page, header, 0);
    title_ = m_.NewText(skin::Title(layout == Layout::Pause ? "Players" : "Multiplayer"), style::type::kPage, style::kMenuAccentInk);   // the page is named for the button that leads here
    sub_ = m_.NewText("", style::type::kRow, style::kMenuDim);
    if (title_) AddH(header, title_, 0, 28).Align(HAlign::Left, VAlign::Center);
    if (sub_) AddH(header, sub_, 1, 0).Align(HAlign::Left, VAlign::Center);
    rule.Height(3);
    rule.Content(ruleLine);
    ruleLine.Color(style::kMenuAccent);
    AddV(page, rule, 12);

    AddV(page, tabs, 14);
    const char* const tabNames[3] = {"PLAYERS", "VOICE", "NETWORK"};
    for (size_t i = 0; i < tabs_.size(); ++i) {
        tabs_[i] = m_.MakeFlat(tabNames[i], style::type::kRow);
        if (tabs_[i].card) AddH(tabs, tabs_[i].card.button, 0, 8);
    }
    AddV(page, body, 14).Fill();
    for (Box& p : pages_) body.Add(p);

    // PLAYERS: the vote running now, then a row per player.
    Box& players = pages_[0];
    voteBar_ = kit.MakePanel();
    Box vrow = kit.MakeRow();
    voteText_ = m_.NewText("", style::type::kRow, style::kMenuText);
    voteYes_ = m_.MakeFlat("YES", style::type::kRow);
    voteNo_ = m_.MakeFlat("NO", style::type::kRow);
    if (voteBar_ && vrow && voteText_ && voteYes_.card && voteNo_.card) {
        voteText_.Wrap(true);
        voteBar_.Color({style::kMenuAccent.r, style::kMenuAccent.g, style::kMenuAccent.b, 0.18f});
        voteBar_.Padding({14, 8, 14, 8});
        voteBar_.Content(vrow);
        AddH(vrow, voteText_, 1, 12).Align(HAlign::Fill, VAlign::Center);
        AddH(vrow, voteYes_.card.button, 0, 8).Align(HAlign::Left, VAlign::Center);
        AddH(vrow, voteNo_.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        AddV(players, voteBar_, 0);
        voteBar_.Show(false);
    } else {
        voteBar_ = Panel{};
    }
    for (PlayerView& r : rows_) {
        r.row = kit.MakeRow();
        Box names = kit.MakeColumn();
        Fixed volBox = kit.MakeFixed();
        r.talk = kit.MakeIcon();
        r.name = m_.NewText("", style::type::kRow, style::kMenuText); r.detail = m_.NewText("", style::type::kHint, style::kMenuDim); r.volume = m_.NewText("", style::type::kBody, style::kMenuText);
        const Margin small{12, 4, 12, 4};
        r.mute = m_.MakeFlat("MUTE", style::type::kBody, small); r.down = m_.MakeFlat("-", style::type::kBody, small); r.up = m_.MakeFlat("+", style::type::kBody, small);
        r.kick = m_.MakeFlat("KICK", style::type::kBody, small); r.ban = m_.MakeFlat("BAN", style::type::kBody, small); r.vote = m_.MakeFlat("VOTE KICK", style::type::kBody, small);
        r.voteBan = m_.MakeFlat("VOTE BAN", style::type::kBody, small);
        if (!r.row || !names || !volBox || !r.talk || !r.name || !r.detail || !r.volume || !r.mute.card || !r.down.card || !r.up.card ||
            !r.kick.card || !r.ban.card || !r.vote.card || !r.voteBan.card) {
            r = PlayerView{};
            continue;
        }
        r.talk.Size({10, 10});
        r.volume.Align(Justify::Center);
        volBox.Width(64);
        volBox.Content(r.volume).Align(HAlign::Fill, VAlign::Center);
        AddV(names, r.name, 0);
        AddV(names, r.detail, 1);
        AddH(r.row, r.talk, 0, 10).Align(HAlign::Left, VAlign::Center);
        AddH(r.row, names, 1, 12).Align(HAlign::Fill, VAlign::Center);
        for (Flat* f : {&r.mute, &r.down}) AddH(r.row, f->card.button, 0, 6).Align(HAlign::Left, VAlign::Center);
        AddH(r.row, volBox, 0, 6).Align(HAlign::Fill, VAlign::Center);
        AddH(r.row, r.up.card.button, 0, 18).Align(HAlign::Left, VAlign::Center);
        for (Flat* f : {&r.kick, &r.ban, &r.vote, &r.voteBan}) AddH(r.row, f->card.button, 0, 6).Align(HAlign::Left, VAlign::Center);
        AddV(players, r.row, 8);
        r.row.Show(false);
    }
    alone_ = m_.NewText("Nobody else is here yet.", style::type::kBody, style::kMenuDim);
    if (alone_) AddV(players, alone_, 12);

    // VOICE: the same block as the multiplayer page, its lists in a layer of its own over the panel (the pause panel has no VOICE tab).
    if (layout == Layout::Overlay) {
        drops_ = std::make_shared<DropdownLayer>();
        if (!drops_->Build(kit, root, 1001)) drops_.reset();
    }
    voice_.Build(m_, pages_[1], 0, drops_.get());

    // NETWORK: read-only lines.
    for (Text& t : netLines_) {
        t = m_.NewText("", style::type::kRow, style::kMenuText);
        if (t) { AddV(pages_[2], t, 6); t.Show(false); }
    }

    if (layout == Layout::Overlay) {   // the pause panel has CHANGE LOADOUT on its own list, with the reason as its hint
        loadoutNote_ = m_.NewText("", style::type::kBody, style::kMenuDim);
        if (loadoutNote_) { loadoutNote_.Wrap(true); AddV(page, loadoutNote_, 12); loadoutNote_.Show(false); }
    }
    AddV(page, footer, 16);
    const float foot = layout == Layout::Pause ? style::type::kEmphasis : style::type::kRow;   // BACK TO MENU is a button of the pause list's size
    resume_ = m_.MakeFlat("RESUME", foot);
    loadout_ = m_.MakeFlat("CHANGE LOADOUT", foot);
    gameMenu_ = m_.MakeFlat("GAME MENU", foot);
    report_ = m_.MakeFlat("REPORT A BUG", foot);
    help_ = m_.MakeFlat("HELP IMPROVE THE MOD: OFF", foot);
    leave_ = m_.MakeFlat("LEAVE SESSION", foot);
    back_ = m_.MakeFlat(layout == Layout::Pause ? kPauseBackLabel : "BACK", foot);
    // The pause panel's page: BACK TO MENU alone (CHANGE LOADOUT is on its list), and no VOICE tab: the voice settings are in MOD SETTINGS.
    if (layout == Layout::Pause && tabs_[1].card) tabs_[1].card.button.Show(false);
    const std::vector<Flat*> footerButtons = layout == Layout::Pause ? std::vector<Flat*>{&back_}
        : std::vector<Flat*>{&resume_, &loadout_, &gameMenu_, &report_, &help_, &leave_};
    for (Flat* f : footerButtons) if (f->card) AddH(footer, f->card.button, 0, 10).Align(HAlign::Left, VAlign::Center);
    panel_.Show(false);
    return true;
}

void EscPanel::Show(bool on) {
    if (!panel_) return;
    shown_ = on;
    panel_.Show(on);
    if (!on) { voice_.StopBinding(); if (drops_) drops_->Close("page"); }
    nextData_ = 0;
    leaveArmedAt_ = 0;
    confirmRow_ = -1;
}

// The players, the vote and the network lines, four times a second (names and pings change slowly).
void EscPanel::Refresh(Session& session) {
    players_ = session.Players();
    vote_ = session.Vote();
    const std::vector<PlayerRowView> rows = BuildPlayerRows(players_, vote_);
    const uint32_t now = GetTickCount();
    if (confirmRow_ >= 0 && now - confirmAt_ >= kConfirmMs) confirmRow_ = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        PlayerView& r = rows_[i];
        if (!r.row) continue;
        r.row.Show(i < rows.size());
        if (i >= rows.size()) continue;
        const PlayerRowView& v = rows[i];
        r.name.Set(v.name);
        r.name.Color(v.voteTarget ? style::kMenuAccentInk : style::kMenuText);
        r.detail.Set(v.detail);
        r.detail.Show(!v.detail.empty());
        r.talk.Color(v.talking ? style::kMenuOk : kNone);
        r.mute.card.button.Show(v.showMute);
        r.mute.label.Set(v.muteLabel);
        r.mute.card.selected = v.muteLabel == "UNMUTE";
        r.mute.label.Color(InkOn(r.mute.card, style::kMenuText));
        r.down.card.button.Show(v.showVolume);
        r.up.card.button.Show(v.showVolume);
        r.volume.Show(v.showVolume);
        r.volume.Set(v.volume);
        r.kick.card.button.Show(v.showKick);
        r.ban.card.button.Show(v.showBan);
        r.vote.card.button.Show(v.showVote);
        r.voteBan.card.button.Show(v.showVoteBan);
        const bool confirm = confirmRow_ == int(i);
        r.kick.label.Set(confirm && !confirmBan_ ? "CONFIRM KICK" : "KICK");
        r.ban.label.Set(confirm && confirmBan_ ? "CONFIRM BAN" : "BAN");
        r.kick.card.selected = confirm && !confirmBan_;
        r.ban.card.selected = confirm && confirmBan_;
        r.kick.label.Color(InkOn(r.kick.card, style::kMenuText));
        r.ban.label.Color(InkOn(r.ban.card, style::kMenuText));
    }
    if (alone_) alone_.Show(players_.size() <= 1);
    if (voteBar_) {
        const VoteBar b = BuildVoteBar(vote_);
        voteBar_.Show(b.show);
        voteText_.Set(b.text);
        voteYes_.card.button.Show(b.showButtons);
        voteNo_.card.button.Show(b.showButtons);
        voteYes_.card.selected = b.yesOn;
        voteNo_.card.selected = b.noOn;
        voteYes_.label.Color(InkOn(voteYes_.card, style::kMenuText));
        voteNo_.label.Color(InkOn(voteNo_.card, style::kMenuText));
    }
    const std::vector<std::string> lines = session.NetworkLines();
    for (size_t i = 0; i < netLines_.size(); ++i) {
        if (!netLines_[i]) continue;
        netLines_[i].Show(i < lines.size());
        if (i < lines.size()) netLines_[i].Set(lines[i]);
    }
    if (sub_) sub_.Set(lines.empty() ? std::string() : lines[0] + ", " + std::to_string(players_.size()) + (players_.size() == 1 ? " player" : " players"));
}

void EscPanel::HandlePlayers(Session& session, uint32_t now) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        PlayerView& r = rows_[i];
        if (!r.row) continue;
        for (Flat* f : {&r.mute, &r.down, &r.up, &r.kick, &r.ban, &r.vote, &r.voteBan}) Paint(f->card);
        const bool mute = Clicked(r.mute.card), down = Clicked(r.down.card), up = Clicked(r.up.card);
        const bool kick = Clicked(r.kick.card), ban = Clicked(r.ban.card), vote = Clicked(r.vote.card), voteBan = Clicked(r.voteBan.card);
        if (i >= players_.size()) continue;
        MpPlayer& p = players_[i];
        if (mute) { p.muted = !p.muted; session.SetPlayerListen(p.slot, p.muted, p.volumePct); nextData_ = 0; }
        if (down || up) { p.volumePct = voice::StepVolumePct(p.volumePct, up ? 1 : -1); session.SetPlayerListen(p.slot, p.muted, p.volumePct); nextData_ = 0; }
        if ((kick || ban) && p.canKick) {
            if (confirmRow_ == int(i) && confirmBan_ == ban && now - confirmAt_ < kConfirmMs) { session.KickPlayer(p.slot, ban); confirmRow_ = -1; }
            else { confirmRow_ = int(i); confirmBan_ = ban; confirmAt_ = now; }
            nextData_ = 0;
        }
        if (vote && p.canVote && !vote_.active) { session.VoteKickPlayer(p.slot); nextData_ = 0; }
        if (voteBan && p.canVoteBan && !vote_.active) { session.VoteBanPlayer(p.slot); nextData_ = 0; }
    }
    if (voteBar_) {
        Paint(voteYes_.card); Paint(voteNo_.card);
        const bool yes = Clicked(voteYes_.card), no = Clicked(voteNo_.card);
        if ((yes || no) && vote_.active && vote_.mine >= -1) { session.CastVote(yes); nextData_ = 0; }
    }
}

EscPanel::Action EscPanel::Tick(Session& session, bool reportOn) {
    if (!panel_ || !shown_) return Action::None;
    const uint32_t now = GetTickCount();
    if (drops_) drops_->Tick(uint32_t(tab_));   // an open list takes its click first, and a change of tab closes it
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (!tabs_[i].card) continue;
        if (Clicked(tabs_[i].card)) { tab_ = int(i); nextData_ = 0; }
        tabs_[i].card.selected = tab_ == int(i);
        Paint(tabs_[i].card);                   // after the click: the chosen tab's art is its highlight
        tabs_[i].label.Color(InkOn(tabs_[i].card, style::kMenuDim));
        pages_[i].Show(tab_ == int(i));
    }
    if (tab_ != 1) voice_.StopBinding();
    HandlePlayers(session, now);
    if (!nextData_ || int32_t(now - nextData_) >= 0) { nextData_ = now + 250; Refresh(session); }
    if (tab_ == 1) voice_.Tick(session);

    const bool pausePage = layout_ == Layout::Pause;
    for (Flat* f : {&resume_, &loadout_, &gameMenu_, &report_, &help_, &leave_, &back_}) if (!pausePage || f == &back_) Paint(f->card);
    if (report_.card && !pausePage) report_.card.button.Show(reportOn);
    // CHANGE LOADOUT (the stock-menu overlay; the pause panel has it on its list): only in a match; dimmed, with the reason under
    // the buttons, while the rule says no. A click re-reads the passport (edited in the game's own menu) and asks the host for
    // the new kit; the answer is a notice.
    if (loadout_.card && !pausePage) {
        const MpLoadout lv = session.LoadoutView();
        loadout_.card.button.Show(lv.shown);
        Live(loadout_, lv.available);
        if (lv.shown && lv.available && Clicked(loadout_.card)) session.ChangeLoadout();
        if (loadoutNote_) {
            const std::string& text = lv.hint;
            loadoutNote_.Show(lv.shown && !text.empty());
            if (lv.shown && !text.empty()) loadoutNote_.Set(text);
        }
    }
    if (help_.card && !pausePage) {             // the multiplayer page's switch: on gets the picked card and its ink
        const bool help = session.HelpImprove();
        help_.card.button.Show(reportOn);
        help_.label.Set(help ? "HELP IMPROVE THE MOD: ON" : "HELP IMPROVE THE MOD: OFF");
        help_.card.selected = help;
        help_.label.Color(InkOn(help_.card, style::kMenuText));
        if (reportOn && Clicked(help_.card)) { session.SetHelpImprove(!help); nextData_ = 0; }
    }
    if (pausePage) return Clicked(back_.card) ? Action::Back : Action::None;
    const bool resume = Clicked(resume_.card), game = Clicked(gameMenu_.card), report = Clicked(report_.card), leave = Clicked(leave_.card);
    if (leaveArmedAt_ && now - leaveArmedAt_ >= kConfirmMs) leaveArmedAt_ = 0;
    if (leave_.card) {
        leave_.label.Set(leaveArmedAt_ ? "CLICK AGAIN TO LEAVE" : "LEAVE SESSION");
        leave_.card.selected = leaveArmedAt_ != 0;
        leave_.label.Color(InkOn(leave_.card, style::kMenuText));
    }
    if (resume) return Action::Resume;
    if (game) return Action::GameMenu;
    if (report && reportOn) return Action::ReportBug;
    if (leave) {
        if (leaveArmedAt_) { leaveArmedAt_ = 0; return Action::Leave; }
        leaveArmedAt_ = now ? now : 1;
    }
    return Action::None;
}

}  // namespace hsm::ui::mp

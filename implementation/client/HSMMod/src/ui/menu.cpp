#include "ui/menu.h"
#include "config.h"
#include "ui/chat.h"
#include "ui/dropdown.h"
#include "ui/notices.h"
#include "ui/umg.h"
#include "ui/mp_widgets.h"
#include "ui/skin.h"
#include "util/hsm_names.h"
#include "gameplay/damage_pin.h"
#include "gameplay/fetch_link.h"
#include "gameplay/mod_maps.h"
#include "gameplay/mod_rules.h"
#include "enhancer.h"
#include "gameplay/mod_pack.h"
#include "gameplay/session.h"
#include "log.h"
#include "ue4ss_load.h"
#include "net/report_client.h"
#include "net/update_check.h"
#include "match/hsm_class.h"
#include "match/hsm_hud_style.h"
#include "match/hsm_lobby_mods.h"
#include "match/hsm_lobby_nav.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_menu_pick.h"
#include "session/hsm_mod_switch.h"
#include "session/hsm_password.h"
#include "util/hsm_bmp.h"
#include "paths.h"
#include <windows.h>
#include <utility>
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

// The page is drawn in the menu palette ("Iron & Blood", shared/match/hsm_hud_style.h). Cards and their hover fade
// come from ui/mp_widgets, shared with the Esc menu.
using mp::Card;
using mp::Flat;
using mp::Paint;
using mp::InkOn;
constexpr float kChatHeight = 170;                // the chat log's frame, in px
constexpr int kMemberRowsShown = 8;               // the players list: eight rows as ever ...
constexpr float kMoreMembersHeight = 150;         // ... and the rest of a bigger lobby (up to 20) in a scrolling frame, in px
constexpr Rgba kClear{1, 1, 1, 0};                  // a Button's own brush, hidden: the frame and face are ours
constexpr float kPlateWidth = 230;                  // a dropdown's closed control, in px: a row's value cell, and the plates in SETTINGS
constexpr int32_t kDropdownZ = 105;                 // the dropdown list's backdrop (the list sits one above): over the page (100), under the windows (110+)
constexpr float kStartGlow = 0.2f;                  // START under the mouse: this far from the accent to its ink (text stays >= 4.5)

// The injected Multiplayer entry on the main menu: either a Button in the stock menu button's style (`plain`), or our
// own card (the page's look) when the stock entry cannot be copied.
struct TextButton {
    Button plain;
    Card card;
    Text label;
    Slot inner{nullptr};                     // the plain button's content slot: inset to the ribbon's flat middle
    Button& button() { return card ? card.button : plain; }
    explicit operator bool() const { return (bool(plain) || bool(card)) && bool(label); }
};
struct ModeView { Card card; Text name, blurb; };
struct MapView { Card card; Text name, tag; };
struct BrowserView { Card card; Text name, detail, players, note; };
struct ChatRow { Box row; Text name, text; };
struct MemberView { Card card; Icon swatch, avatar, ready; Text name, badge, team; };
struct HostView { Card card; Text name, blurb; };
struct TabView { Card card; Text label; Panel line; };   // MODE | MAP | RULES | MODS | CLASSES: the chosen one underlined in the accent

// A player's Steam avatar as an engine texture, made once and kept while the engine keeps it (an Image's brush holds
// it; one no row shows any more is collected, and made again when it is needed).
struct Avatar {
    steam::SteamId id = 0;
    UObject* texture = nullptr;
    UClass* cls = nullptr;                           // with index: the slot still holds this texture
    int32_t index = -1;
    ULONGLONG nextTry = 0;
    int tries = 0;
};
constexpr int kAvatarTries = 20;                     // once a second while Steam fetches it
struct StepperView { Box box; Text label, value, hint; Flat prev, next, plate; };   // plate: a dropdown row's value, in place of value
struct ClassRow { Box box; Flat name; Text value; Flat prev, next; };
// The Damage row's value (0.8.0-pre26): a slider (0..200%) with the number beside it; a click on the number swaps in a
// text box for a custom value up to 1000% (Enter applies, Esc or clicking away cancels). Host only.
struct DamageView {
    Slider slider;
    Flat number;
    EditBox edit;
    bool editing = false, hadFocus = false, focused = false, enterDown = false, escDown = false, dragging = false;
    float shown = -1;                                // where the slider was last put (-1: not yet)
};
// The Gather time row's value (Crypts): the time as a button between the arrows; a click swaps in a text box for any whole
// minutes from 2 to 60 (Enter applies, Esc or clicking away cancels). Host only.
struct GatherView {
    Flat number;
    EditBox edit;
    bool editing = false, hadFocus = false, focused = false, enterDown = false, escDown = false;
};
// The Dead weight and Weapon weight rows' value: a slider (0..100%, 5% a step) with the number beside it. Host only.
struct WeightView {
    LobbyRow id = LobbyRow::DeadWeight;
    Slider slider;
    Text number;
    bool dragging = false;
    float shown = -1;                                // where the slider was last put (-1: not yet)
};
// MODS tab: one mod: its name, "kind · who runs it", why the lobby refuses it; the verdict on the right.
struct ModItemView { Box box; Text name, detail, note, verdict; };
constexpr size_t kModItemRows = 48;                  // more: "+N more" (the host's log has the whole list)
constexpr size_t kTierChipCount = 8;                 // the tier chips under Gear (or the Crypts card's Starting tier): Beggar ... Knight

// A name row: a text box with SET and CLEAR and a line under it (the host's lobby name, the player's nickname).
struct NameRow {
    Box box;
    EditBox text;
    Flat set, clear;
    Text note;
    bool focused = false, enterDown = false;
    std::string filled;                      // the saved name the box was last filled with
    std::string refused;                     // why the last SET was refused ("" when it was taken)
};

struct State {
    UObject* worldContext = nullptr;
    UClass* menuClass = nullptr;
    UObject* menu = nullptr;
    int32_t menuIndex = -1;
    ULONGLONG nextScan = 0, nextText = 0;
    bool built = false, panelOpen = false;
    Kit kit;
    mp::Maker maker;
    TextButton openButton;
    Panel panel;                             // the full-screen multiplayer page (a Border over the whole menu)
    // The CLASSES tab, which YOUR CLASS and the lobby's CHANGE go to: the classes, what the picked one does, and what each row means.
    Text className, classNote, classInfoName, classInfo;
    std::vector<Flat> classPicks;            // one per class, in ClassPickName's order
    std::vector<ClassRow> classFields;
    Bar classAttack, classSpeed, classTough;
    Text classBudget;
    std::vector<Flat> classTabs;              // the dropdown's items; classTabButton opens it
    Flat classTabButton, classNew, classDelete, warnOn, warnCancel;
    Box classTabList;
    bool classTabOpen = false, classDeleteArmed = false, warnOpen = false;
    Panel warn;                               // the Unrestricted disclaimer
    Flat classUnrestricted;                   // the old MODS checkbox: kept in the tree, hidden; the Classes row asks for the disclaimer now
    bool unrestrictedAsked = false;           // the Classes row was stepped onto Unrestricted: the disclaimer comes first
    Text warnTitle, warnBody, warnMods;
    enum class WarnKind { Unrestricted, Hse, Switch };   // what the warning panel asks about: Switch is the restart with mods off (Lobby::SwitchAsk)
    WarnKind warnKind = WarnKind::Unrestricted;
    Text classWhere, classTabNote;
    int classTab = kClassTabPlain;
    uint32_t classFrame = 0;
    Box classNameRow;
    EditBox classNameBox;
    Flat classNameSave;
    std::string classNameFor, classNameNote;  // the class the box was filled for; why its name was refused
    bool classNameEnterDown = false, classNameFocused = false;
    Flat classOpen, classChange;
    Box classRow;                            // in a lobby that uses classes: the class we fight as, and CHANGE
    Text classRowText;
    bool classShown = false;                 // YOUR CLASS has put the classes in the right column: outside a lobby, or in one that has no CLASSES tab
    bool wasIn = false;                      // the last drawn scene was in a lobby (arriving or leaving closes the class page)
    int classFieldSel = 7;                   // the row the description box explains (RUN)
    Box updateRow;                           // the update notice and its UPDATE NOW button (net/update_check.h)
    Flat updateNow;
    Text title, me, badge, status, notice, updateNote, pillText, playersTitle, idleHint, hostOnly, waiting, summary;
    Panel pill, pillFrame;
    // Not in a lobby: HOST on the left, the browser on the right. In one: players and chat left, the tabs right.
    Box mainRow, idleLeft, inLeft, idleRight, inRight, settingsView;
    HostView hostFriends, hostPublic;
    Flat refresh, invite, readyUp, balance, shuffle, leave, close, settingsOpen;   // shuffle: SHUFFLE NOW (Lobby::ShuffleTeamsNow); close: the footer's bottom-left button (FooterFor)
    std::array<Flat, 1 + kWeaponTypeCount> weaponChips;   // the Weapons row's ticks: Any, then the game's types 1..7 (SettingRow::chips)
    std::array<Box, 2> weaponChipRows;
    std::array<Flat, kTierChipCount> gearTierChips, startTierChips;   // the tiers, under Gear (while the tier is used) and under the Crypts card's Starting tier
    std::array<Box, 2> gearTierRows, startTierRows;
    std::array<Flat, kFilterChips> chips;
    Text browserCount, browserEmpty;
    BrowserFilter filter;                    // the chips' state, kept while the game runs
    std::array<TabView, kTabCount> tabs;
    std::array<Box, kTabCount> tabPages;
    Box tabBar, tabFoot;                     // the tabs, and the summary with START: only in a lobby, unlike the classes
    Fixed tabRule;
    size_t tab = kModeTab;
    std::array<Flat, kSectionCount> subTabs;   // RULES: MATCH | COMBAT | TEAMS | GEAR | WORLD, the ones the mode has rows in
    Box subTabRow;
    LobbySection rulesTab = LobbySection::Match;
    std::array<Box, kSectionCount> sections;
    Text noMaps;
    bool settingsShown = false;
    Flat reportOpen, reportSend, reportCancel;   // REPORT A BUG, and the box it opens
    Box reportBox;
    EditBox reportText;
    Text reportNote;
    uint32_t reportSeen = 0;   // Session::ReportStatusSeq last shown in reportNote
    bool reportOpenNow = false;
    bool reportTyping = false;   // the report box has the keyboard (MenuTyping)
    Flat helpToggle;                         // HELP IMPROVE THE MOD: ON/OFF (SETTINGS)
    Flat lookToggle;                         // MENU LOOK: CLASSIC / GAME ART / DARK (SETTINGS); ui_style, used from the next start
    Text lookNote;
    style::MenuLook look = style::MenuLook::Classic; bool lookSaved = true;   // what HSMMod.ini will say at the next start; written
    DropdownLayer drops;                     // the one list every dropdown on the page opens in (ui/dropdown.h); built first, under the windows
    mp::VoiceBlock voice;                    // VOICE CHAT: your own reach, knocked-out toggle, the microphone, the meter
    Text helpNote;
    Panel ask;                               // the one-time HELP IMPROVE THE MOD question over the whole menu (0.8.0-pre7)
    Flat askYes, askNo;
    bool askOpen = false, escDown = false;
    Panel pw;                                // the lobby password question over the whole menu: joining a locked lobby
    Text pwNote;                             // why it asks again ("Wrong password")
    EditBox pwText;
    Flat pwOk, pwCancel;
    bool pwOpen = false, pwEscDown = false, pwEnterDown = false, pwFocused = false;
    Box pwHostBox;                           // the host's password row in the lobby (SET / CLEAR); only the host sees it
    EditBox pwHostText;
    Flat pwSet, pwClear;
    Text pwHostNote;
    Text rulesNote;                          // RULES in Progression: why it holds only three rows
    bool pwHostFocused = false, pwHostEnterDown = false;
    NameRow lobbyName;                       // the host's lobby name row in the lobby (SET / CLEAR); only the host sees it
    NameRow nickname;                        // SETTINGS: the nickname the other players see instead of the Steam name
    Flat hideToggle;                         // SETTINGS: HIDE PLAYER NAMES: ON/OFF
    Text hideNote;
    Flat myRegion;                           // SETTINGS: MY REGION: AUTO (NA EAST); steps Auto, NA East ... (Session::SetRegionPick); without the lists
    Flat myRegionPlate, lookPlate;           // SETTINGS: MY REGION and MENU LOOK as dropdowns (the buttons are then not made)
    Text myRegionNote;
    Box chatBox;                             // the lobby chat: CHAT, the last lines, the text box and SEND (only in a lobby)
    Box chatScroll;
    std::array<ChatRow, kChatLogLines> chatRows;
    EditBox chatText;
    Flat chatSend;
    uint32_t chatSeen = 0;                   // ChatLinesAdded() at the last draw
    bool chatFocused = false, enterDown = false;   // the box had the keyboard at the last frame; Enter was down
    std::vector<BrowserView> rows;           // kBrowserRows
    std::vector<uint64_t> rowLobbies;        // the Steam lobby each shown row joins
    std::vector<MemberView> members;         // kMaxLobbyMembers
    Fixed moreMembers;                       // the scrolling frame of the rows past kMemberRowsShown; shown in a bigger lobby only
    std::vector<steam::SteamId> memberIds;
    std::vector<Avatar> avatars;
    bool avatarFailLogged = false;
    std::vector<ModeView> modes;             // kModeOrder, then Brawl, then Progression
    std::vector<StepperView> steppers;       // one per LobbyRow, in its order
    DamageView damage;
    GatherView gather;
    std::array<WeightView, 2> weights;       // Dead weight, Weapon weight
    std::vector<MapView> maps;               // the pickable maps, kMaps order, then the modded ones
    Card start;
    Text startLabel;
    LobbyScene scene;                        // the last drawn: what the clicks act on
    // MODS tab: the lobby's mod rule (the host steps it), the session's mods, who the host is still checking.
    std::array<StepperView, kModRuleRows> modRules;
    Text modSummary, modStatus, modEmpty, modMore, modTruncated, modPendingTitle, modPrivacy;
    std::vector<ModItemView> modItems;       // kModItemRows, always in the tree (hidden when unused)
    std::array<Text, game::kMaxLobbyMembers> modPending;
    LobbyModsScene modsScene;                // the last drawn: what the clicks act on
} g;

bool StillAlive(UObject* o, int32_t index, UClass* cls) { return o && ObjectAt(index) == o && ClassOf(o) == cls; }

bool Clicked(TextButton& b) { return b.button().Clicked(); }
using mp::Clicked;                                  // a Card's click (TextButton's is above)

Slot AddToVerticalBox(Box& box, const Widget& child, float padTop) { return box.Add(child).Padding({0, padTop, 0, 0}); }

// A vertical box child that takes the space left over (the page body, a tab's page).
void FillVerticalBox(Box& box, const Widget& child, float padTop) { AddToVerticalBox(box, child, padTop).Fill(); }

// fill > 0: a share of the row; 0: the child's own width.
Slot AddToHorizontalBox(Box& box, const Widget& child, float fill, float padRight) {
    Slot s = box.Add(child);
    if (fill > 0) s.Fill(fill); else s.Auto();
    s.Padding({0, 0, padRight, 0});
    return s;
}

Text NewText(const std::string& text, float size, Rgba color) { return g.maker.NewText(text, size, color); }

// A section label: a rubric in the game's blackletter (MATCH, PUBLIC LOBBIES, CHAT).
Text Section(Box& column, const std::string& text, float padTop) {
    Text t = NewText(skin::Title(text), style::type::kSection, style::kMenuAccentInk);
    if (t) { skin::Font(t, skin::Face::Heading); t.Wrap(true); AddToVerticalBox(column, t, padTop); }   // a narrow column wraps it
    return t;
}

Card MakeCard(const Widget& body, Margin pad) { return g.maker.MakeCard(body, pad); }

Flat MakeFlat(const std::string& text, float size, Margin pad = {16, 7, 16, 7}) {
    Flat f = g.maker.MakeFlat(text, size, pad);
    if (f.label) f.label.Color(style::kMenuText);
    return f;
}

void AddFlat(Box& column, Flat& f, float padTop) { if (f.card) AddToVerticalBox(column, f.card.button, padTop).Align(HAlign::Left, VAlign::Fill); }
void AddFlatH(Box& row, Flat& f, float padRight) { if (f.card) AddToHorizontalBox(row, f.card.button, 0, padRight).Align(HAlign::Left, VAlign::Center); }

// A card across its column: a big name and a line under it (HOST FOR FRIENDS, HOST PUBLIC).
HostView MakeHost(const std::string& name, const std::string& blurb) {
    HostView v;
    Box body = g.kit.MakeColumn();
    v.name = NewText(name, style::type::kEmphasis, style::kMenuText);
    v.blurb = NewText(blurb, style::type::kHint, style::kMenuDim);
    if (!body || !v.name || !v.blurb) return HostView{};
    v.blurb.Wrap(true);
    AddToVerticalBox(body, v.name, 0);
    AddToVerticalBox(body, v.blurb, 4);
    v.card = MakeCard(body, {18, 14, 18, 16});
    return v;
}

// Live or not: a dead card is dimmed and takes no clicks.
void Live(Card& c, bool on) {
    if (!c) return;
    c.live = on;
    c.button.Enabled(on);
    c.button.Opacity(on ? 1.f : style::kDisabledAlpha);
}

// Dimmed (mp::Dimmed, mp_widgets.h): not disabled, so a click can still say why. The click does nothing (HandleClicks' act checks
// `allowed`); only the look and the hover glow follow `on`.

// What a click on a control the player cannot use says. The ONE place a reason goes out: a notice (notices.h), keyed so a
// second click replaces the first.
void Explain(const std::string& why) {
    if (why.empty()) return;
    HSM_LOG("ui: a click on a dimmed control: %s", why.c_str());
    Notify(notices::StartWhy(why));
}

// The menu's structure around the anchor, to the log once per run: the next log shows how the stock entries are built
// (user widgets, buttons, labels, brushes), which is what the Multiplayer entry copies.
void LogMenuTree(UObject* w, UObject* anchor, int depth, int& budget) {
    if (budget <= 0) return;
    --budget;
    HSM_LOG("ui: menu tree %*s%s%s", depth * 2, "", Describe(w).c_str(), w == anchor ? "   <-- anchor" : "");
    if (depth >= 4) return;
    for (UObject* child : ChildrenOf(w)) LogMenuTree(child, anchor, depth + 1, budget);
}

void LogLookOnce(const std::string& source) {
    static bool logged = false;
    if (logged) return;
    logged = true;
    HSM_LOG("ui: Multiplayer button look: %s", source.c_str());
}

// The Multiplayer entry made to look like the menu's own buttons, from what the anchor (a stock button) turns out to be:
// - its label is a real TextBlock (UI_MainMenuSNF's buttons, or a ribbon with text over it): the same button style, and
//   the label takes that TextBlock's font and colour;
// - it is art with the label baked in (this build's UI_Startup_Menu ribbons; copying the style would show the stock
//   label): the page's own card (dark face, thin frame, lit under the mouse), with the menu's own typeface when any
//   TextBlock in the menu has one. Never the plain white default button.
TextButton MakeOpenButton(const std::string& text, UObject* anchor, bool startup) {
    TextButton b;
    b.label = g.kit.MakeText();
    if (!b.label) { HSM_LOG("ui: widget spawn failed"); return TextButton{}; }
    UObject* stockLabel = startup ? FindTextBlock(anchor, 4) : nullptr;
    std::string source;
    if (!startup || stockLabel) {
        b.plain = g.kit.MakeButton();
        if (!b.plain) { HSM_LOG("ui: widget spawn failed"); return TextButton{}; }
        b.plain.StyleFrom(anchor);
        source = "the anchor's button style";
        if (stockLabel && b.label.StyleFrom(stockLabel)) source += ", font and colour of its label " + ObjectName(stockLabel);
        else source += ", the kit's font";
        // The anchor's ribbon (Quit) is sized for its four letters at that font; "MULTIPLAYER" or "LOBBY (HOST) 6/6"
        // ran past its ends. The label shrinks to fit instead (a ScaleBox), when the kit can make one.
        Shrink fit = g.kit.MakeShrink();
        if (fit) {
            fit.Content(b.label).Align(HAlign::Center, VAlign::Center);
            b.inner = b.plain.Content(fit);
        } else {
            b.inner = b.plain.Content(b.label);
        }
        b.inner.Align(HAlign::Center, VAlign::Center);
    } else {
        b.card = MakeCard(b.label, {12, 4, 12, 4});
        if (!b.card) return TextButton{};
        b.card.face.ContentAlign(HAlign::Center, VAlign::Center);
        UObject* menuText = FindTextBlock(g.menu, 12);
        source = "the page's card (the anchor's label is baked into its art)";
        if (menuText && b.label.StyleFrom(menuText)) source += ", font of the menu's TextBlock " + ObjectName(menuText);
        else source += ", the kit's font";
        b.label.Color(style::kMenuText);
    }
    LogLookOnce(source);
    b.label.Set(text);
    return b;
}

// Insert the Multiplayer button right after `anchor`, in whatever container the menu uses.
bool InjectOpenButton(UObject* anchor, bool startup) {
    UObject* freeMode = anchor;
    Call gp(freeMode, "GetParent"); gp.invoke();
    UObject* parent = gp.get<UObject*>("ReturnValue");
    if (!parent) { HSM_LOG("ui: menu anchor button has no parent panel"); return false; }
    {
        static bool treeLogged = false;
        int budget = 80;
        if (!treeLogged) { treeLogged = true; LogMenuTree(parent, anchor, 0, budget); }
    }
    g.openButton = MakeOpenButton("MULTIPLAYER", anchor, startup);
    if (!g.openButton) return false;
    std::string pcls = ObjectName(reinterpret_cast<UObject*>(ClassOf(parent)));
    if (pcls == "CanvasPanel") {
        Call add(parent, "AddChildToCanvas"); add.set<UObject*>("Content", g.openButton.button().get()).invoke();
        Slot slot(add.get<UObject*>("ReturnValue"));
        UObject** freeSlotPtr = Field<UObject*>(freeMode, "Slot");
        if (slot && freeSlotPtr && *freeSlotPtr) {
            UObject* fs = *freeSlotPtr;
            Call pos(fs, "GetPosition"); pos.invoke(); Vec2 p = pos.get<Vec2>("ReturnValue");
            Call size(fs, "GetSize"); size.invoke(); Vec2 s = size.get<Vec2>("ReturnValue");
            Call anc(fs, "GetAnchors"); anc.invoke(); Anchors a = anc.get<Anchors>("ReturnValue");
            Call al(fs, "GetAlignment"); al.invoke(); Vec2 alignment = al.get<Vec2>("ReturnValue");
            // The ribbon is copied from Quit's, sized for four letters: ours is drawn wider and a little taller, about
            // the same centre, so "Multiplayer" reads at the size of the entries above it.
            const Vec2 big = g.openButton.plain ? Vec2{s.x * 1.7, s.y * 1.2} : s;
            slot.Anchor(a, alignment).Size(big).Position(Vec2{p.x + (alignment.x - 0.5) * (big.x - s.x), p.y + s.y * 1.15 + alignment.y * (big.y - s.y)});
            // Our own card has no ribbon art to set the type size: the label is a share of the entry's height.
            if (g.openButton.card && s.y > 0) g.openButton.label.Font(float(std::clamp(s.y * 0.45, 14.0, 40.0)));
            // The ribbon's curled ends and its shadow take the edges: the label keeps to the flat middle.
            if (g.openButton.inner && s.x > 0 && s.y > 0)
                g.openButton.inner.Padding({float(big.x * 0.17), float(big.y * 0.16), float(big.x * 0.17), float(big.y * 0.22)});
        }
    } else {
        // UPanelWidget::InsertChildAt is C++-only in this engine build (tools/check_reflection.py); AddChild is the
        // reflected way in, so the button goes to the end of the box rather than reshuffling the stock layout.
        Call add(parent, "AddChild"); add.set<UObject*>("Content", g.openButton.button().get()).invoke();
        if (!add.get<UObject*>("ReturnValue")) { HSM_LOG("ui: %s refused the Multiplayer button", pcls.c_str()); return false; }
    }
    HSM_LOG("ui: Multiplayer button added to %s", pcls.c_str());
    return true;
}

// The lobby chat under the players: the last lines in a framed box of fixed height (it scrolls when long lines wrap
// past it), then the text box with SEND. Enter sends too (HandleClicks).
void BuildChat(Box& left) {
    g.chatBox = g.kit.MakeColumn();
    Fixed frame = g.kit.MakeFixed();
    Panel border = g.kit.MakePanel(), panel = g.kit.MakePanel();
    g.chatScroll = g.kit.MakeScroll();
    Box lines = g.kit.MakeColumn(), input = g.kit.MakeRow();
    g.chatText = g.kit.MakeEditBox();
    g.chatSend = MakeFlat("SEND", style::type::kBody, {14, 5, 14, 5});
    if (!g.chatBox || !frame || !border || !panel || !g.chatScroll || !lines || !input || !g.chatText || !g.chatSend.card) {
        HSM_LOG("ui: lobby chat widgets could not be made; chat disabled");
        g.chatBox = Box{};
        return;
    }
    Section(g.chatBox, "CHAT", 0);
    frame.Height(kChatHeight);
    border.Color(style::kMenuFrame);
    border.Padding({1, 1, 1, 1});
    border.Content(panel);
    panel.Color(style::kMenuCard);
    panel.Padding({10, 6, 10, 6});
    panel.Content(g.chatScroll);
    frame.Content(border).Align(HAlign::Fill, VAlign::Fill);
    AddToVerticalBox(g.chatBox, frame, 6);
    g.chatScroll.Add(lines);
    for (ChatRow& r : g.chatRows) {
        r.row = g.kit.MakeRow();
        r.name = NewText("", style::type::kBody, style::kMenuDim);
        r.text = NewText("", style::type::kBody, style::kMenuText);
        if (!r.row || !r.name || !r.text) continue;
        r.text.Wrap(true);
        AddToHorizontalBox(r.row, r.name, 0, 8).Align(HAlign::Left, VAlign::Top);
        AddToHorizontalBox(r.row, r.text, 1, 0).Align(HAlign::Fill, VAlign::Top);
        AddToVerticalBox(lines, r.row, 2);
        r.row.Show(false);
    }
    g.chatText.Hint("Say something (or /bug what went wrong)");
    AddToHorizontalBox(input, g.chatText, 1, 8).Align(HAlign::Fill, VAlign::Center);
    AddToHorizontalBox(input, g.chatSend.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
    AddToVerticalBox(g.chatBox, input, 6);
    AddToVerticalBox(left, g.chatBox, 18);
    g.chatBox.Show(false);
}

// Not in a lobby, left: the two ways to host, big, and what comes next.
void BuildIdleLeft() {
    Box& col = g.idleLeft;
    Section(col, "HOST A LOBBY", 0);
    g.hostFriends = MakeHost("HOST FOR FRIENDS", "Friends join through Steam or your invite. It is not listed for anyone else.");
    g.hostPublic = MakeHost("HOST PUBLIC", "Listed in the public lobbies for anyone with the mod.");
    for (HostView* h : {&g.hostFriends, &g.hostPublic})
        if (h->card) AddToVerticalBox(col, h->card.button, 10).Align(HAlign::Fill, VAlign::Fill);
    g.idleHint = NewText("Host a lobby or join one to choose the mode and the map.", style::type::kBody, style::kMenuDim);
    if (g.idleHint) { g.idleHint.Wrap(true); AddToVerticalBox(col, g.idleHint, 18); }
}

// Not in a lobby, right: PUBLIC LOBBIES with REFRESH, the filter chips, then the rows (incompatible ones dimmed with
// the reason).
void BuildIdleRight() {
    Box& col = g.idleRight;
    Box head = g.kit.MakeRow(), chips = g.kit.MakeRow(), list = g.kit.MakeScroll();
    if (!head || !chips || !list) { HSM_LOG("ui: the lobby browser could not be made"); return; }
    AddToVerticalBox(col, head, 0);
    Text title = NewText(skin::Title("Public Lobbies"), style::type::kSection, style::kMenuAccentInk);
    skin::Font(title, skin::Face::Heading);
    if (title) AddToHorizontalBox(head, title, 0, 16).Align(HAlign::Left, VAlign::Center);
    g.browserCount = NewText("", style::type::kHint, style::kMenuDim);
    if (g.browserCount) AddToHorizontalBox(head, g.browserCount, 1, 12).Align(HAlign::Left, VAlign::Center);
    g.refresh = MakeFlat("REFRESH", style::type::kBody, {14, 5, 14, 5});
    AddFlatH(head, g.refresh, 0);
    AddToVerticalBox(col, chips, 12);
    for (size_t i = 0; i < g.chips.size(); ++i) {   // the region first (kRegionChip is 0), then the others
        g.chips[i] = MakeFlat("", style::type::kHint, {12, 4, 12, 4});
        AddFlatH(chips, g.chips[i], 8);
    }
    FillVerticalBox(col, list, 12);
    for (size_t i = 0; i < kBrowserRows; ++i) {
        BrowserView v;
        Box row = g.kit.MakeRow(), text = g.kit.MakeColumn();
        v.name = NewText("", style::type::kRow, style::kMenuText); v.detail = NewText("", style::type::kHint, style::kMenuDim);
        v.players = NewText("", style::type::kBody, style::kMenuDim); v.note = NewText("", style::type::kBody, style::kMenuOk);
        if (row && text && v.name && v.detail && v.players && v.note) {
            AddToVerticalBox(text, v.name, 0); AddToVerticalBox(text, v.detail, 2);
            AddToHorizontalBox(row, text, 1, 14);
            AddToHorizontalBox(row, v.players, 0, 18).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.note, 0, 0).Align(HAlign::Left, VAlign::Center);
            v.card = MakeCard(row, {14, 8, 16, 8});
        }
        if (v.card) { list.Add(v.card.button).Padding({0, 0, 0, 6}); v.card.button.Show(false); }
        g.rows.push_back(v);                    // kept even if empty: g.rows[i] is the scene's browser[i]
    }
    g.rowLobbies.assign(g.rows.size(), 0);
    g.browserEmpty = NewText("", style::type::kBody, style::kMenuDim);
    if (g.browserEmpty) { g.browserEmpty.Wrap(true); list.Add(g.browserEmpty).Padding({0, 6, 0, 0}); }
}

// The host's password row under the lobby's buttons: a text box with SET and CLEAR, and a line saying whether the lobby is
// locked. Shown to the host only (Draw). Setting or clearing it kicks nobody (Session::SetLobbyPassword).
void BuildPasswordRow(Box& left) {
    g.pwHostBox = g.kit.MakeColumn();
    Box input = g.kit.MakeRow();
    g.pwHostText = g.kit.MakeEditBox();
    g.pwSet = MakeFlat("SET", style::type::kBody, {14, 5, 14, 5});
    g.pwClear = MakeFlat("CLEAR", style::type::kBody, {14, 5, 14, 5});
    g.pwHostNote = NewText("", style::type::kHint, style::kMenuDim);
    if (!g.pwHostBox || !input || !g.pwHostText || !g.pwSet.card || !g.pwClear.card || !g.pwHostNote) {
        HSM_LOG("ui: the lobby password row could not be made");
        g.pwHostBox = Box{};
        return;
    }
    Section(g.pwHostBox, "LOBBY PASSWORD", 0);
    g.pwHostText.Hint("Password (up to 32 characters)");
    AddToHorizontalBox(input, g.pwHostText, 1, 8).Align(HAlign::Fill, VAlign::Center);
    AddFlatH(input, g.pwSet, 8); AddFlatH(input, g.pwClear, 0);
    AddToVerticalBox(g.pwHostBox, input, 6);
    g.pwHostNote.Wrap(true);
    AddToVerticalBox(g.pwHostBox, g.pwHostNote, 4);
    g.pwHostNote.Show(false);               // only a refusal is said here; the box and its buttons say the rest
    AddToVerticalBox(left, g.pwHostBox, 14);
    g.pwHostBox.Show(false);
}

// A name row under `parent`: the rubric, a text box with SET and CLEAR, and a line under it. HandleNameRow runs it.
void BuildNameRow(NameRow& r, Box& parent, const std::string& title, const std::string& hint, float padTop) {
    r.box = g.kit.MakeColumn();
    Box input = g.kit.MakeRow();
    r.text = g.kit.MakeEditBox();
    r.set = MakeFlat("SET", style::type::kBody, {14, 5, 14, 5});
    r.clear = MakeFlat("CLEAR", style::type::kBody, {14, 5, 14, 5});
    r.note = NewText("", style::type::kHint, style::kMenuDim);
    if (!r.box || !input || !r.text || !r.set.card || !r.clear.card || !r.note) {
        HSM_LOG("ui: the %s row could not be made", title.c_str());
        r.box = Box{};
        return;
    }
    Section(r.box, title, 0);
    r.text.Hint(hint);
    AddToHorizontalBox(input, r.text, 1, 8).Align(HAlign::Fill, VAlign::Center);
    AddFlatH(input, r.set, 8); AddFlatH(input, r.clear, 0);
    AddToVerticalBox(r.box, input, 6);
    r.note.Wrap(true);
    AddToVerticalBox(r.box, r.note, 4);
    AddToVerticalBox(parent, r.box, padTop);
}

// In a lobby, left: the players (the host clicks one to move them to the next team), the lobby's buttons, the chat.
void BuildInLeft() {
    Box& col = g.inLeft;
    g.playersTitle = Section(col, "PLAYERS", 0);
    // The first eight rows are laid out as ever. A bigger lobby (up to 20) lists the rest under them in a scrolling frame,
    // which is hidden until a ninth player is in.
    g.moreMembers = g.kit.MakeFixed();
    Box moreList = g.kit.MakeScroll(), moreRows = g.kit.MakeColumn();
    const bool more = g.moreMembers && moreList && moreRows;
    if (!more) { g.moreMembers = Fixed{}; HSM_LOG("ui: the players list's scrolling frame could not be made; rows past eight follow the others"); }
    for (int i = 0; i < game::kMaxLobbyMembers; ++i) {
        if (i == kMemberRowsShown && more) {
            g.moreMembers.Height(kMoreMembersHeight);
            g.moreMembers.Content(moreList).Align(HAlign::Fill, VAlign::Fill);
            moreList.Add(moreRows);
            AddToVerticalBox(col, g.moreMembers, 6);
            g.moreMembers.Show(false);
        }
        Box& into = i >= kMemberRowsShown && more ? moreRows : col;
        MemberView v;
        Box row = g.kit.MakeRow();
        v.swatch = g.kit.MakeIcon(); v.avatar = g.kit.MakeIcon(); v.ready = g.kit.MakeIcon();
        v.name = NewText("", style::type::kRow, style::kMenuText); v.badge = NewText("HOST", style::type::kHint, style::kMenuAccentInk); v.team = NewText("", style::type::kHint, style::kMenuDim);
        if (row && v.swatch && v.avatar && v.ready && v.name && v.badge && v.team) {
            v.swatch.Size({5, 32}); v.avatar.Size({32, 32}); v.ready.Size({10, 10}); v.ready.Color(style::kMenuOk);
            v.avatar.Visibility(Vis::Hidden);
            AddToHorizontalBox(row, v.swatch, 0, 10).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.avatar, 0, 12).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.name, 1, 10).Align(HAlign::Fill, VAlign::Center);
            AddToHorizontalBox(row, v.badge, 0, 12).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.team, 0, 12).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.ready, 0, 0).Align(HAlign::Left, VAlign::Center);
            v.card = MakeCard(row, {8, 5, 12, 5});
        }
        if (v.card) { AddToVerticalBox(into, v.card.button, 6).Align(HAlign::Fill, VAlign::Fill); v.card.button.Show(false); }
        g.members.push_back(v);                 // kept even if empty: g.members[i] is the scene's roster[i]
    }
    g.memberIds.assign(g.members.size(), 0);
    Box actions1 = g.kit.MakeRow(), actions2 = g.kit.MakeRow();
    g.readyUp = MakeFlat("Mark me ready", style::type::kBody, {14, 6, 14, 6});
    g.balance = MakeFlat("AUTO-BALANCE", style::type::kBody, {14, 6, 14, 6});
    g.shuffle = MakeFlat("SHUFFLE NOW", style::type::kBody, {14, 6, 14, 6});
    if (actions1 && actions2) {      // INVITE FRIENDS and LEAVE LOBBY are in the footer now (BuildPanel)
        AddToVerticalBox(col, actions1, 14);
        AddToVerticalBox(col, actions2, 8);
        AddFlatH(actions1, g.readyUp, 0);
        AddFlatH(actions2, g.balance, 8); AddFlatH(actions2, g.shuffle, 0);
    }
    g.classRow = g.kit.MakeRow();
    g.classRowText = NewText("", style::type::kBody, style::kMenuText);
    g.classChange = MakeFlat("CHANGE", style::type::kBody, {14, 5, 14, 5});
    if (g.classRow && g.classRowText && g.classChange.card) {
        AddToHorizontalBox(g.classRow, g.classRowText, 1, 8).Align(HAlign::Fill, VAlign::Center);
        AddFlatH(g.classRow, g.classChange, 0);
        AddToVerticalBox(col, g.classRow, 14);
        g.classRow.Show(false);
    }
    BuildNameRow(g.lobbyName, col, "LOBBY NAME", "Lobby name (up to 32 characters)", 14);
    if (g.lobbyName.box) g.lobbyName.box.Show(false);
    BuildPasswordRow(col);
    BuildChat(col);
}

// The one-time question (0.8.0-pre7): the first MULTIPLAYER click shows it over the menu before the page opens. YES turns
// help_improve on, NO THANKS (or Esc) leaves it off; either answer is saved and it is never asked again. Off by default.
void BuildAsk(Canvas& root) {
    g.ask = g.kit.MakePanel();
    Fixed width = g.kit.MakeFixed();
    Panel frame = g.kit.MakePanel(), face = g.kit.MakePanel();
    Box column = g.kit.MakeColumn(), buttons = g.kit.MakeRow();
    Text title = NewText(skin::Title("Help Improve the Mod?"), style::type::kModal, style::kMenuAccentInk);
    Text body = NewText("When someone in your lobby files a bug report, or your game crashes, your HSMMod log is sent to the "
                        "maintainers so the bug can be found and fixed.", style::type::kRow, style::kMenuText);
    Text privacy = NewText("Steam IDs, IP addresses and your Windows user name are removed first. Player names stay. "
                           "Nothing is sent unless you say yes. You can change this any time in SETTINGS on the "
                           "multiplayer page or in the Esc menu during a match.", style::type::kHint, style::kMenuDim);
    g.askYes = MakeFlat("YES, SEND MY LOG", style::type::kRow);
    g.askNo = MakeFlat("NO THANKS", style::type::kRow);
    if (!g.ask || !width || !frame || !face || !column || !buttons || !title || !body || !privacy || !g.askYes.card || !g.askNo.card) {
        HSM_LOG("ui: the help-improve question could not be made; the page opens without it");
        g.ask = Panel{};
        return;
    }
    root.Add(g.ask).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(110);
    g.ask.Color({0, 0, 0, 0.80f});
    g.ask.ContentAlign(HAlign::Center, VAlign::Center);
    g.ask.Content(width);
    width.Width(760);
    width.Content(frame);
    skin::Paint(frame, skin::Art::Note);
    skin::Pad(frame, skin::Art::Note, {10, 6, 10, 6});
    frame.Content(face);
    face.Color(kClear);
    face.Padding({36, 28, 36, 28});
    face.Content(column);
    AddToVerticalBox(column, title, 0);
    body.Wrap(true); AddToVerticalBox(column, body, 14);
    privacy.Wrap(true); AddToVerticalBox(column, privacy, 10);
    AddToVerticalBox(column, buttons, 22);
    AddToHorizontalBox(buttons, g.askYes.card.button, 0, 14);
    AddToHorizontalBox(buttons, g.askNo.card.button, 0, 0);
    g.ask.Show(false);
}

constexpr const char* kUnrestrictedWarning = "This turns off class balance in the lobbies you host. Fighters can go far past what the game was built for: up to five times most stats. Expect broken fights, "
                                             "physics glitches and possibly crashes, for you and everyone who joins. Only play this with people who know what they're joining.";

// What Unrestricted or allowing HS Enhancer means, before it's on.
void BuildWarn(Canvas& root) {
    g.warn = g.kit.MakePanel();
    Fixed width = g.kit.MakeFixed();
    Panel frame = g.kit.MakePanel(), face = g.kit.MakePanel();
    Box column = g.kit.MakeColumn(), buttons = g.kit.MakeRow();
    Text title = g.warnTitle = NewText("", style::type::kModal, style::kMenuAccentInk);
    Text body = g.warnBody = NewText("", style::type::kRow, style::kMenuText);
    Text mods = g.warnMods = NewText("", style::type::kBody, style::kMenuDim);
    g.warnOn = MakeFlat("TURN IT ON", style::type::kRow);
    g.warnCancel = MakeFlat("CANCEL", style::type::kRow);
    if (!g.warn || !width || !frame || !face || !column || !buttons || !title || !body || !mods || !g.warnOn.card || !g.warnCancel.card) {
        HSM_LOG("ui: the Unrestricted warning could not be made, so the checkbox stays off");
        g.warn = Panel{};
        return;
    }
    root.Add(g.warn).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(112);
    g.warn.Color({0, 0, 0, 0.85f});
    g.warn.ContentAlign(HAlign::Center, VAlign::Center);
    g.warn.Content(width);
    width.Width(720);
    width.Content(frame);
    skin::Paint(frame, skin::Art::Note);
    skin::Pad(frame, skin::Art::Note, {10, 6, 10, 6});
    frame.Content(face);
    face.Color(kClear);
    face.Padding({36, 28, 36, 28});
    face.Content(column);
    AddToVerticalBox(column, title, 0);
    body.Wrap(true); AddToVerticalBox(column, body, 14);
    mods.Wrap(true); AddToVerticalBox(column, mods, 10);
    AddToVerticalBox(column, buttons, 20);
    AddToHorizontalBox(buttons, g.warnOn.card.button, 0, 14);
    AddToHorizontalBox(buttons, g.warnCancel.card.button, 0, 0);
    g.warn.Show(false);
}

// While the warning is open it's all that takes clicks. true: it is.
bool HandleWarn(Session& session) {
    game::Lobby& lobby = session.lobby();
    if (g.warnOpen && g.warnKind == State::WarnKind::Switch && !lobby.SwitchAskPending().lobby) { g.warnOpen = false; g.warn.Show(false); }   // the lobby went away
    if (!g.warnOpen) return false;
    const bool on = Clicked(g.warnOn.card), cancelClick = Clicked(g.warnCancel.card), esc = mp::KeyDown(VK_ESCAPE) && !game::fetchlink::KeysHeld();   // Esc in Fetch's menu is Fetch's
    const bool cancel = cancelClick || esc;
    Paint(g.warnOn.card); Paint(g.warnCancel.card);
    if (on) {
        switch (g.warnKind) {
            case State::WarnKind::Unrestricted: session.SetClassesUnrestricted(true); break;
            case State::WarnKind::Hse: session.SetAllowHse(true); break;
            case State::WarnKind::Switch: lobby.ConfirmSwitch(); break;
        }
    } else if (cancel && g.warnKind == State::WarnKind::Switch) {
        const bool leave = cancelClick && lobby.SwitchAskPending().rejoin && !lobby.SwitchAskPending().back;   // LEAVE LOBBY: out before the host's grace runs out. Esc only closes the question
        lobby.CancelSwitchAsk();
        if (leave) lobby.Leave();
    }
    if (on || cancel) { g.warnOpen = false; g.warn.Show(false); }
    return g.warnOpen;
}

// The password question (a locked lobby: clicked in the browser, an invite, or a wrong password): the same modal as the
// help-improve question, with a text box. JOIN (or Enter) tries the password, CANCEL (or Esc) drops the join.
void BuildPassword(Canvas& root) {
    g.pw = g.kit.MakePanel();
    Fixed width = g.kit.MakeFixed();
    Panel frame = g.kit.MakePanel(), face = g.kit.MakePanel();
    Box column = g.kit.MakeColumn(), buttons = g.kit.MakeRow();
    Text title = NewText(skin::Title("This Lobby Is Locked"), style::type::kModal, style::kMenuAccentInk);
    Text body = NewText("Type the lobby's password to join. The host gave it to you.", style::type::kRow, style::kMenuText);
    g.pwNote = NewText("", style::type::kRow, style::kMenuWarn);
    g.pwText = g.kit.MakeEditBox();
    g.pwOk = MakeFlat("JOIN", style::type::kRow);
    g.pwCancel = MakeFlat("CANCEL", style::type::kRow);
    if (!g.pw || !width || !frame || !face || !column || !buttons || !title || !body || !g.pwNote || !g.pwText || !g.pwOk.card || !g.pwCancel.card) {
        HSM_LOG("ui: the lobby password question could not be made; locked lobbies cannot be joined from this page");
        g.pw = Panel{};
        return;
    }
    root.Add(g.pw).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(111);
    g.pw.Color({0, 0, 0, 0.80f});
    g.pw.ContentAlign(HAlign::Center, VAlign::Center);
    g.pw.Content(width);
    width.Width(620);
    width.Content(frame);
    skin::Paint(frame, skin::Art::Note);
    skin::Pad(frame, skin::Art::Note, {10, 6, 10, 6});
    frame.Content(face);
    face.Color(kClear);
    face.Padding({36, 28, 36, 28});
    face.Content(column);
    AddToVerticalBox(column, title, 0);
    body.Wrap(true); AddToVerticalBox(column, body, 14);
    g.pwText.Hint("Password");
    AddToVerticalBox(column, g.pwText, 14);
    g.pwNote.Wrap(true); AddToVerticalBox(column, g.pwNote, 10);
    AddToVerticalBox(column, buttons, 20);
    AddToHorizontalBox(buttons, g.pwOk.card.button, 0, 14);
    AddToHorizontalBox(buttons, g.pwCancel.card.button, 0, 0);
    g.pw.Show(false);
}

// MODE tab: the cards two to a row, in their groups (VERSUS, CO-OP).
void BuildModeTab(Box& page) {
    Grid versus = g.kit.MakeGrid(), coop = g.kit.MakeGrid();
    if (!versus || !coop) return;
    Section(page, "VERSUS", 0);
    versus.SlotPadding({4, 4, 4, 4});
    AddToVerticalBox(page, versus, 6);
    Section(page, "CO-OP", 16);
    coop.SlotPadding({4, 4, 4, 4});
    AddToVerticalBox(page, coop, 6);
    int32_t at[2] = {0, 0};
    for (size_t i = 0; i < kModeCardCount; ++i) {
        ModeView v;
        Box body = g.kit.MakeColumn();
        v.name = NewText("", style::type::kEmphasis, style::kMenuText); v.blurb = NewText("", style::type::kHint, style::kMenuDim);
        if (body && v.name && v.blurb) {
            v.blurb.Wrap(true);
            AddToVerticalBox(body, v.name, 0); AddToVerticalBox(body, v.blurb, 4);
            v.card = MakeCard(body, {14, 10, 14, 12});
        }
        const int grp = ModeGroupAt(i) == ModeGroup::Coop ? 1 : 0;
        Grid& grid = grp ? coop : versus;
        if (v.card) grid.Add(v.card.button, at[grp] / 2, at[grp] % 2).Align(HAlign::Fill, VAlign::Fill);
        ++at[grp];
        g.modes.push_back(v);                   // kept even if empty: g.modes[i] is the scene's modes[i]
    }
}

// The Damage row's value cell: "slider | number", and the text box in their place while the number is edited.
bool BuildDamageValue(Fixed& cell) {
    DamageView& d = g.damage;
    Box row = g.kit.MakeRow();
    d.slider = g.kit.MakeSlider();
    d.number = MakeFlat("100%", style::type::kBody, {8, 2, 8, 2});
    d.edit = g.kit.MakeEditBox();
    if (!row || !d.slider || !d.number.card || !d.edit) { g.damage = DamageView{}; return false; }
    AddToHorizontalBox(row, d.slider, 1, 6).Align(HAlign::Fill, VAlign::Center);
    AddToHorizontalBox(row, d.number.card.button, 0, 0).Align(HAlign::Right, VAlign::Center);
    AddToHorizontalBox(row, d.edit, 1, 0).Align(HAlign::Fill, VAlign::Center);
    d.edit.Show(false);
    cell.Content(row).Align(HAlign::Fill, VAlign::Center);
    return true;
}

// The Gather time row's value cell: the time, and the text box in its place while it is typed.
bool BuildGatherValue(Fixed& cell) {
    GatherView& d = g.gather;
    Box row = g.kit.MakeRow();
    d.number = MakeFlat("10 min", style::type::kRow, {8, 2, 8, 2});
    d.edit = g.kit.MakeEditBox();
    if (!row || !d.number.card || !d.edit) { g.gather = GatherView{}; return false; }
    AddToHorizontalBox(row, d.number.card.button, 1, 0).Align(HAlign::Center, VAlign::Center);
    AddToHorizontalBox(row, d.edit, 1, 0).Align(HAlign::Fill, VAlign::Center);
    d.edit.Show(false);
    cell.Content(row).Align(HAlign::Fill, VAlign::Center);
    return true;
}

// A weight row's value cell: "slider | number". Empty when a widget could not be made.
bool BuildWeightValue(LobbyRow id, Fixed& cell) {
    WeightView& w = g.weights[id == LobbyRow::DeadWeight ? 0 : 1];
    Box row = g.kit.MakeRow();
    w.slider = g.kit.MakeSlider();
    w.number = NewText(WeightLabel(kWeightDefaultPct), style::type::kBody, style::kMenuText);
    if (!row || !w.slider || !w.number) { w = WeightView{}; return false; }
    w.id = id;
    AddToHorizontalBox(row, w.slider, 1, 6).Align(HAlign::Fill, VAlign::Center);
    AddToHorizontalBox(row, w.number, 0, 0).Align(HAlign::Right, VAlign::Center);
    cell.Content(row).Align(HAlign::Fill, VAlign::Center);
    return true;
}

// A stepper row, "label and hint | < | value | >" (the RULES and MODS tabs). The caller puts the value cell's content
// in `valueBox` (v.value, or the Damage slider) and adds v.box to its column. Empty when a widget could not be made.
StepperView MakeStepper(Fixed& valueBox) {
    StepperView v;
    v.box = g.kit.MakeRow();
    Box text = g.kit.MakeColumn();
    valueBox = g.kit.MakeFixed();
    v.label = NewText("", style::type::kRow, style::kMenuText); v.value = NewText("", style::type::kRow, style::kMenuText); v.hint = NewText("", style::type::kHint, style::kMenuDim);
    v.prev = g.maker.MakeArrow(false, style::type::kRow); v.next = g.maker.MakeArrow(true, style::type::kRow);
    if (!v.box || !text || !valueBox || !v.label || !v.value || !v.hint || !v.prev.card || !v.next.card) return StepperView{};
    v.value.Align(Justify::Center);
    v.hint.Wrap(true);
    valueBox.Width(kPlateWidth);
    AddToVerticalBox(text, v.label, 0);
    AddToVerticalBox(text, v.hint, 2);
    AddToHorizontalBox(v.box, text, 1, 16).Align(HAlign::Fill, VAlign::Center);
    AddToHorizontalBox(v.box, v.prev.card.button, 0, 6).Align(HAlign::Left, VAlign::Center);
    AddToHorizontalBox(v.box, valueBox, 0, 6).Align(HAlign::Fill, VAlign::Center);
    AddToHorizontalBox(v.box, v.next.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
    return v;
}

// A dropdown row's value: the plate fills the value cell and the value's Text is dropped (in no tree it is freed by garbage
// collection, so it is never written). False (the row keeps its value Text, and its arrows step) when no list is built.
bool BuildPlateValue(Fixed& valueBox, StepperView& v) {
    if (!g.drops) return false;
    v.plate = g.maker.MakePlate(style::type::kRow);
    if (!v.plate.card) { v.plate = Flat{}; return false; }
    valueBox.Content(v.plate.card.button).Align(HAlign::Fill, VAlign::Center);
    v.value = Text{};
    return true;
}

// Chips under a stepper row, in two rows of four: the Weapons types, the eight tiers. Made once; DrawChipRows fills and shows them.
template <size_t N, class Name>
void BuildChipRows(Box& home, std::array<Box, 2>& rows, std::array<Flat, N>& chips, Name name) {
    for (size_t r = 0; r < rows.size(); ++r) {
        rows[r] = g.kit.MakeRow();
        if (rows[r]) AddToVerticalBox(home, rows[r], r ? 4 : 6);
    }
    for (size_t k = 0; k < chips.size(); ++k) {
        chips[k] = MakeFlat(name(k), style::type::kHint, {10, 4, 10, 4});
        Box& row = rows[k < (N + 1) / 2 ? 0 : 1];
        if (row) AddFlatH(row, chips[k], 6);
    }
}

// RULES tab: the sub-tabs (MATCH, COMBAT, TEAMS, GEAR, WORLD: the ones the mode has rows in), and a stepper per setting of the
// chosen one, "label and hint | < | value | >". Every row is made here and Draw shows the chosen sub-tab's.
void BuildRulesTab(Box& page) {
    g.subTabRow = g.kit.MakeRow();
    if (g.subTabRow) AddToVerticalBox(page, g.subTabRow, 0);
    for (size_t sct = 0; sct < kSectionCount; ++sct) {
        g.subTabs[sct] = MakeFlat(SectionName(LobbySection(sct)), style::type::kBody, {14, 6, 14, 6});
        if (g.subTabRow) AddFlatH(g.subTabRow, g.subTabs[sct], 8);
    }
    g.rulesNote = NewText("", style::type::kHint, style::kMenuDim);   // Progression: "The game's own rules, in the Tavern." (hidden otherwise; never removed)
    if (g.rulesNote) { g.rulesNote.Wrap(true); AddToVerticalBox(page, g.rulesNote, 12); g.rulesNote.Show(false); }
    for (size_t sct = 0; sct < kSectionCount; ++sct) {
        g.sections[sct] = g.kit.MakeColumn();
        if (g.sections[sct]) AddToVerticalBox(page, g.sections[sct], 2);
    }
    for (int i = 0; i <= int(LobbyRow::Sync); ++i) {
        Box& home = g.sections[size_t(SectionOf(LobbyRow(i)))];
        Fixed valueBox;
        StepperView v = MakeStepper(valueBox);
        if (!home || !v.box) { g.steppers.push_back(StepperView{}); continue; }
        // The Damage row shows the slider instead: its value text is in no tree, so garbage collection frees it, and a
        // later Set on it crashed (0.8.0-pre26). Drop it; the UpdatePanel writes to an empty Text are no-ops.
        if (LobbyRow(i) == LobbyRow::Damage && BuildDamageValue(valueBox)) v.value = Text{};
        else if ((LobbyRow(i) == LobbyRow::DeadWeight || LobbyRow(i) == LobbyRow::WeaponWeight) && BuildWeightValue(LobbyRow(i), valueBox)) v.value = Text{};   // the same
        else if (LobbyRow(i) == LobbyRow::Gather && BuildGatherValue(valueBox)) v.value = Text{};   // the same
        else if (lobby_detail::RowKindOf(LobbyRow(i)) == RowKind::Choice && BuildPlateValue(valueBox, v)) {}   // a named value: a dropdown
        else valueBox.Content(v.value).Align(HAlign::Fill, VAlign::Center);
        AddToVerticalBox(home, v.box, 10);
        if (LobbyRow(i) == LobbyRow::Weapons)   // the types to tick, in two rows of four under the row's text
            BuildChipRows(home, g.weaponChipRows, g.weaponChips, [](size_t k) { return std::string(WeaponTypeName(uint8_t(k))); });
        if (LobbyRow(i) == LobbyRow::Gear)      // the tier, while it is used (random or identical gear, bots)
            BuildChipRows(home, g.gearTierRows, g.gearTierChips, [](size_t k) { return TierChips(0)[k].label; });
        if (LobbyRow(i) == LobbyRow::Tier)      // the Crypts card's Starting tier: the chips alone
            BuildChipRows(home, g.startTierRows, g.startTierChips, [](size_t k) { return TierChips(0)[k].label; });
        g.steppers.push_back(v);
    }
}

// MAP tab: the map cards two to a row (the refused ones dimmed with the reason), the modded maps after them.
void BuildMapTab(Box& page) {
    Grid grid = g.kit.MakeGrid();
    if (!grid) return;
    grid.SlotPadding({4, 4, 4, 4});
    AddToVerticalBox(page, grid, 0);
    int32_t at = 0;
    auto add = [&](const std::string& name) {
        MapView v;
        Box body = g.kit.MakeColumn();
        v.name = NewText(name, style::type::kRow, style::kMenuText); v.tag = NewText("", style::type::kHint, style::kMenuDim);
        if (body && v.name && v.tag) {
            AddToVerticalBox(body, v.name, 0); AddToVerticalBox(body, v.tag, 2);
            v.card = MakeCard(body, {12, 8, 12, 9});
        }
        if (v.card) grid.Add(v.card.button, at / 2, at % 2).Align(HAlign::Fill, VAlign::Fill);
        ++at;
        g.maps.push_back(v);                    // kept even if empty: g.maps[j] is s.maps[j]
    };
    for (size_t i = 0; i < MapCount(); ++i) if (MapAt(i).pick) add(MapAt(i).display);
    for (size_t i = 0; i < kMaxModMaps; ++i) add("");   // hidden until a modded map fills it
    g.noMaps = NewText("Progression plays in the Tavern: there is no map to pick.", style::type::kBody, style::kMenuDim);
    if (g.noMaps) { g.noMaps.Wrap(true); AddToVerticalBox(page, g.noMaps, 6); g.noMaps.Show(false); }
}

// MODS tab: RULE (three steppers, the host's to change) and the rule in force; IN THIS LOBBY (a row per mod anyone
// runs); WAITING (host: who it is still checking); the privacy line. Every row is made here and only hidden: a widget
// out of the tree is freed by garbage collection (0.8.0-pre26).
void BuildModsTab(Box& page) {
    Section(page, "RULE", 0);
    for (size_t i = 0; i < kModRuleRows; ++i) {
        Fixed valueBox;
        StepperView v = MakeStepper(valueBox);
        if (!v.box) continue;
        if (!ModRuleIsChoice(ModRuleRow(i)) || !BuildPlateValue(valueBox, v))   // Mods allowed and Classes are dropdowns; the two switches keep their arrows
            valueBox.Content(v.value).Align(HAlign::Fill, VAlign::Center);
        AddToVerticalBox(page, v.box, i ? 10 : 2);
        g.modRules[i] = v;
    }
    g.modSummary = NewText("", style::type::kBody, style::kMenuText);
    g.modStatus = NewText("", style::type::kHint, style::kMenuWarn);
    for (Text* t : {&g.modSummary, &g.modStatus}) if (*t) { t->Wrap(true); AddToVerticalBox(page, *t, 8); }
    g.classUnrestricted = MakeFlat("[  ] UNRESTRICTED", 15, {10, 4, 10, 4});
    AddFlat(page, g.classUnrestricted, 12);
    if (g.classUnrestricted.card) g.classUnrestricted.card.button.Show(false);   // the Classes row (a stepper above) is the switch now; the widget stays
    Section(page, "IN THIS LOBBY", 18);
    for (size_t i = 0; i < kModItemRows; ++i) {
        ModItemView v;
        v.box = g.kit.MakeRow();
        Box text = g.kit.MakeColumn();
        v.name = NewText("", style::type::kRow, style::kMenuText); v.detail = NewText("", style::type::kHint, style::kMenuDim);
        v.note = NewText("", style::type::kHint, style::kMenuWarn); v.verdict = NewText("", style::type::kHint, style::kMenuDim);
        if (!v.box || !text || !v.name || !v.detail || !v.note || !v.verdict) { g.modItems.push_back(ModItemView{}); continue; }
        v.detail.Wrap(true); v.note.Wrap(true);
        AddToVerticalBox(text, v.name, 0);
        AddToVerticalBox(text, v.detail, 1);
        AddToVerticalBox(text, v.note, 1);
        AddToHorizontalBox(v.box, text, 1, 12).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(v.box, v.verdict, 0, 0).Align(HAlign::Right, VAlign::Top);
        AddToVerticalBox(page, v.box, i ? 8 : 4);
        v.box.Show(false);
        g.modItems.push_back(v);
    }
    g.modEmpty = NewText("", style::type::kHint, style::kMenuDim);
    g.modMore = NewText("", style::type::kHint, style::kMenuDim);
    g.modTruncated = NewText("", style::type::kHint, style::kMenuWarn);
    for (Text* t : {&g.modEmpty, &g.modMore, &g.modTruncated}) if (*t) { t->Wrap(true); AddToVerticalBox(page, *t, 6); }
    g.modPendingTitle = Section(page, "WAITING", 18);
    for (Text& t : g.modPending) {
        t = NewText("", style::type::kHint, style::kMenuWarn);
        if (t) { t.Wrap(true); AddToVerticalBox(page, t, 4); t.Show(false); }
    }
    g.modPrivacy = NewText("", style::type::kHint, style::kMenuDim);
    if (g.modPrivacy) { g.modPrivacy.Wrap(true); AddToVerticalBox(page, g.modPrivacy, 18); }
}

void BuildClassView(Box& rowOf);

// In a lobby, right: the MODE | MAP | RULES | MODS | CLASSES tabs, the chosen tab's page, and the summary with START at the foot.
void BuildInRight() {
    Box& col = g.inRight;
    Box tabs = g.kit.MakeRow(), foot = g.kit.MakeRow(), footText = g.kit.MakeColumn();
    Fixed under = g.kit.MakeFixed();
    Panel underLine = g.kit.MakePanel();
    if (!tabs || !foot || !footText || !under || !underLine) { HSM_LOG("ui: the lobby tabs could not be made"); return; }
    g.tabBar = tabs; g.tabRule = under; g.tabFoot = foot;
    AddToVerticalBox(col, tabs, 0);
    std::array<const char*, kTabCount> kTabNames{};
    for (size_t i = 0; i < kTabCount; ++i) kTabNames[i] = TabName(i);     // MODE, MAP, RULES, MODS, CLASSES (match/hsm_lobby_nav.h)
    for (size_t i = 0; i < kTabCount; ++i) {
        TabView& t = g.tabs[i];
        Box body = g.kit.MakeColumn();
        Fixed lineBox = g.kit.MakeFixed();
        t.label = NewText(skin::Title(kTabNames[i]), style::type::kEmphasis, style::kMenuDim);
        skin::Font(t.label, skin::Face::Heading);
        t.line = g.kit.MakePanel();
        if (!body || !lineBox || !t.label || !t.line) continue;
        AddToVerticalBox(body, t.label, 0);
        lineBox.Height(3);
        lineBox.Content(t.line).Align(HAlign::Fill, VAlign::Fill);
        AddToVerticalBox(body, lineBox, 6).Align(HAlign::Fill, VAlign::Fill);
        t.card = MakeCard(body, {14, 6, 14, 0});
        if (t.card) AddToHorizontalBox(tabs, t.card.button, 0, 6);
    }
    under.Height(1);
    under.Content(underLine).Align(HAlign::Fill, VAlign::Fill);
    underLine.Color(style::kMenuFrame);
    AddToVerticalBox(col, under, 0);
    for (size_t i = 0; i < kTabCount; ++i) {
        if (i == kClassesTab) {                 // no scroll box round this one: two of its columns scroll, and the third stays put
            g.tabPages[i] = g.kit.MakeRow();
            if (g.tabPages[i]) { FillVerticalBox(col, g.tabPages[i], 14); BuildClassView(g.tabPages[i]); }
            continue;
        }
        Box scroll = g.kit.MakeScroll();
        g.tabPages[i] = g.kit.MakeColumn();
        if (!scroll || !g.tabPages[i]) continue;
        scroll.Add(g.tabPages[i]).Padding({0, 0, 12, 0});
        FillVerticalBox(col, scroll, 14);
        if (i == kModeTab) BuildModeTab(g.tabPages[i]);
        if (i == kMapTab) BuildMapTab(g.tabPages[i]);
        if (i == kRulesTab) BuildRulesTab(g.tabPages[i]);
        if (i == kModsTab) BuildModsTab(g.tabPages[i]);
        g.tabPages[i] = scroll;                 // what the tab shows and collapses is the page's scroll box
    }
    AddToVerticalBox(col, foot, 14);
    AddToHorizontalBox(foot, footText, 1, 18).Align(HAlign::Fill, VAlign::Center);
    g.summary = NewText("", style::type::kBody, style::kMenuText);
    g.hostOnly = NewText("", style::type::kHint, style::kMenuDim);
    g.waiting = NewText("", style::type::kBody, style::kMenuDim);
    for (Text* t : {&g.summary, &g.hostOnly, &g.waiting}) if (*t) { t->Wrap(true); AddToVerticalBox(footText, *t, 2); }
    g.startLabel = NewText("START MATCH", style::type::kCard, style::kMenuLight);
    g.start = MakeCard(g.startLabel, {38, 12, 38, 12});
    if (g.start) {
        g.startLabel.Font(style::type::kCard, 1, style::kOutlineInk);
        g.startLabel.Align(Justify::Center);
        AddToHorizontalBox(foot, g.start.button, 0, 0).Align(HAlign::Right, VAlign::Center);
    }
}

// A column of the CLASSES tab. It sits in the page's right panel, so it has no panel of its own.
Box AddClassColumn(Box& row, float fill, float padRight, bool scroll) {
    Box outer = scroll ? g.kit.MakeScroll() : g.kit.MakeColumn();
    if (!outer) return Box{};
    AddToHorizontalBox(row, outer, fill, padRight).Align(HAlign::Fill, VAlign::Fill);
    if (!scroll) return outer;
    Box inner = g.kit.MakeColumn();
    if (inner) outer.Add(inner).Padding({0, 0, 12, 0});
    return inner;
}

// A labelled bar: ATTACK, SPEED, TOUGHNESS.
Bar AddClassBar(Box& col, const char* name) {
    Box row = g.kit.MakeRow();
    Fixed labelBox = g.kit.MakeFixed(), barBox = g.kit.MakeFixed();
    Text label = NewText(name, style::type::kBody, style::kMenuDim);
    Bar bar = g.kit.MakeBar();
    if (!row || !labelBox || !barBox || !label || !bar) return Bar{};
    labelBox.Width(120);
    labelBox.Content(label).Align(HAlign::Fill, VAlign::Center);
    barBox.Width(240); barBox.Height(12);
    barBox.Content(bar).Align(HAlign::Fill, VAlign::Fill);
    bar.FillColor(style::kMenuAccentInk);   // the bar's unfilled part is pale, so a pale fill can't be seen on it
    AddToHorizontalBox(row, labelBox, 0, 8).Align(HAlign::Left, VAlign::Center);
    AddToHorizontalBox(row, barBox, 0, 0).Align(HAlign::Left, VAlign::Center);
    AddToVerticalBox(col, row, 6);
    return bar;
}

// Three columns: the classes, what the picked one does, and the description box. DONE, outside a lobby, goes back to the public lobbies.
void BuildClassView(Box& rowOf) {
    Box pickCol = AddClassColumn(rowOf, 0.65f, 16, true), statCol = AddClassColumn(rowOf, 1.35f, 16, true), infoCol = AddClassColumn(rowOf, 0.85f, 0, false);
    if (!pickCol || !statCol || !infoCol) return;
    Section(pickCol, "CHOOSE YOUR CLASS", 0);
    g.classTabButton = MakeFlat("CLASSES  v", style::type::kRow, {12, 5, 12, 5});
    AddFlat(pickCol, g.classTabButton, 8);
    g.classTabList = g.kit.MakeColumn();
    if (g.classTabList) {
        for (const ClassTab& t : ClassTabs()) {
            Flat f = MakeFlat(t.title, style::type::kBody, {18, 4, 12, 4});
            AddFlat(g.classTabList, f, 2);
            g.classTabs.push_back(f);
        }
        AddToVerticalBox(pickCol, g.classTabList, 2);
        g.classTabList.Show(false);
    }
    g.classTabNote = NewText("", style::type::kHint, style::kMenuDim);
    if (g.classTabNote) { g.classTabNote.Wrap(true); AddToVerticalBox(pickCol, g.classTabNote, 6); }
    for (int i = 0; i < ClassPickCount(); ++i) {
        Flat f = MakeFlat(ClassPickName(i), style::type::kRow, {14, 5, 14, 5});
        AddFlat(pickCol, f, 6);
        g.classPicks.push_back(f);
    }
    g.classNew = MakeFlat("+ NEW CLASS", style::type::kRow, {14, 5, 14, 5});
    AddFlat(pickCol, g.classNew, 8);
    g.classWhere = NewText("Used in lobbies with Classes on.", style::type::kHint, style::kMenuDim);
    if (g.classWhere) { g.classWhere.Wrap(true); AddToVerticalBox(pickCol, g.classWhere, 8); }
    g.className = NewText("", style::type::kCard, style::kMenuText);
    g.classNote = NewText("", style::type::kBody, style::kMenuDim);
    if (g.className) AddToVerticalBox(statCol, g.className, 0);
    if (g.classNote) { g.classNote.Wrap(true); AddToVerticalBox(statCol, g.classNote, 6); }
    g.classNameRow = g.kit.MakeRow();
    g.classNameBox = g.kit.MakeEditBox();
    g.classNameSave = MakeFlat("SAVE NAME", style::type::kHint, {10, 3, 10, 3});
    if (g.classNameRow && g.classNameBox && g.classNameSave.card) {
        g.classNameBox.Hint("Name this class");
        AddToHorizontalBox(g.classNameRow, g.classNameBox, 1, 8).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(g.classNameRow, g.classNameSave.card.button, 0, 8).Align(HAlign::Left, VAlign::Center);
        g.classDelete = MakeFlat("DELETE", style::type::kHint, {10, 3, 10, 3});
        if (g.classDelete.card) AddToHorizontalBox(g.classNameRow, g.classDelete.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        AddToVerticalBox(statCol, g.classNameRow, 8);
        g.classNameRow.Show(false);
    }
    Section(statCol, "AT A GLANCE", 16);
    g.classAttack = AddClassBar(statCol, "ATTACK");
    g.classSpeed = AddClassBar(statCol, "SPEED");
    g.classTough = AddClassBar(statCol, "TOUGHNESS");
    Section(statCol, "STATS", 18);
    for (int i = 0; i < kClassFieldCount; ++i) {
        ClassRow v;
        v.box = g.kit.MakeRow();
        Fixed valueBox = g.kit.MakeFixed();
        v.value = NewText("", style::type::kBody, style::kMenuText);
        v.name = MakeFlat(ClassField(ClassSheet{}, i).label, style::type::kHint, {8, 2, 8, 2});
        v.prev = g.maker.MakeArrow(false, style::type::kBody); v.next = g.maker.MakeArrow(true, style::type::kBody);
        if (!v.box || !valueBox || !v.value || !v.name.card || !v.prev.card || !v.next.card) { g.classFields.push_back(ClassRow{}); continue; }
        v.value.Align(Justify::Center);
        valueBox.Width(120);
        valueBox.Content(v.value).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(v.box, v.name.card.button, 1, 12).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(v.box, v.prev.card.button, 0, 4).Align(HAlign::Left, VAlign::Center);
        AddToHorizontalBox(v.box, valueBox, 0, 4).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(v.box, v.next.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        AddToVerticalBox(statCol, v.box, 3);
        g.classFields.push_back(v);
    }
    // Here and not by the bars, because this column doesn't scroll: it stays on screen while the stats are being changed.
    g.classBudget = NewText("", style::type::kEmphasis, style::kMenuText);
    if (g.classBudget) { g.classBudget.Wrap(true); AddToVerticalBox(infoCol, g.classBudget, 0); }
    Section(infoCol, "WHAT THIS DOES", 16);
    g.classInfoName = NewText("", style::type::kEmphasis, style::kMenuText);
    g.classInfo = NewText("", style::type::kBody, style::kMenuDim);
    if (g.classInfoName) AddToVerticalBox(infoCol, g.classInfoName, 8);
    if (g.classInfo) { g.classInfo.Wrap(true); AddToVerticalBox(infoCol, g.classInfo, 8); }
}

// A dropdown's closed control at the foot of a column, kPlateWidth wide. Empty when it could not be made.
Flat AddPlate(Box& col, float padTop) {
    Flat f = g.maker.MakePlate(style::type::kRow);
    Fixed box = g.kit.MakeFixed();
    if (!f.card || !box) return Flat{};
    box.Width(kPlateWidth);
    box.Content(f.card.button).Align(HAlign::Fill, VAlign::Fill);
    AddToVerticalBox(col, box, padTop).Align(HAlign::Left, VAlign::Top);
    return f;
}

// SETTINGS (the footer button): help improve the mod and voice chat, in place of the lobby while it is open.
void BuildSettingsView(Box& col) {
    Section(col, "HELP IMPROVE THE MOD", 0);
    // The setting behind help_improve in HSMMod.ini; what it sends is session.cpp's business.
    g.helpToggle = MakeFlat("HELP IMPROVE THE MOD: OFF", style::type::kRow);
    AddFlat(col, g.helpToggle, 8);
    g.helpNote = NewText("Sends your HSMMod log if someone in your lobby files a bug report or the game crashes.\n"
                         "Steam IDs, IPs and your Windows user name are removed; player names stay.",
                         14, style::kMenuDim);
    if (g.helpNote) { g.helpNote.Wrap(true); AddToVerticalBox(col, g.helpNote, 8); }
    // NICKNAME: what the other players see instead of the Steam name. HIDE PLAYER NAMES: what this screen shows for them.
    BuildNameRow(g.nickname, col, "NICKNAME", "Nickname (up to 24 characters)", 28);
    // MY REGION: where your lobbies are listed and what My region means in the browser (was REGION: AUTO in the page's header).
    if (g.drops) {                          // a dropdown: the section's name over the plate
        Section(col, "MY REGION", 28);
        g.myRegionPlate = AddPlate(col, 8);
    }
    if (!g.myRegionPlate.card) {
        g.myRegion = MakeFlat("MY REGION: AUTO", style::type::kRow);
        AddFlat(col, g.myRegion, g.drops ? 8 : 28);
    }
    g.myRegionNote = NewText("Auto: the nearest by ping.", style::type::kHint, style::kMenuDim);
    if (g.myRegionNote) { g.myRegionNote.Wrap(true); AddToVerticalBox(col, g.myRegionNote, 8); }
    Section(col, "HIDE PLAYER NAMES", 28);
    g.hideToggle = MakeFlat("HIDE PLAYER NAMES: OFF", style::type::kRow);
    AddFlat(col, g.hideToggle, 8);
    g.hideNote = NewText("On your screen only. For streams and clips.", style::type::kHint, style::kMenuDim);
    if (g.hideNote) { g.hideNote.Wrap(true); AddToVerticalBox(col, g.hideNote, 8); }
    // MENU LOOK: the game's own art (the default), dark, or the earlier look. The widgets are built once, so it takes effect at the next start.
    Section(col, "MENU LOOK", 28);
    g.look = style::CurrentMenuLook();
    if (g.drops) g.lookPlate = AddPlate(col, 8);
    if (!g.lookPlate.card) {
        g.lookToggle = MakeFlat("", style::type::kRow);
        AddFlat(col, g.lookToggle, 8);
    }
    g.lookNote = NewText("", style::type::kHint, style::kMenuDim);
    if (g.lookNote) { g.lookNote.Wrap(true); AddToVerticalBox(col, g.lookNote, 8); }
    // VOICE CHAT: the same block as the in-match Esc menu (ui/mp_widgets.h); shown with or without a lobby.
    g.voice.Build(g.maker, col, 28, g.drops ? &g.drops : nullptr);
}

// A column's dark panel around a box (a scroll box when `scroll`), as one share of the body row.
Box AddColumn(Box& row, float fill, float padRight, bool scroll) {
    Panel p = g.kit.MakePanel();
    Box outer = scroll ? g.kit.MakeScroll() : g.kit.MakeColumn();
    Box inner = g.kit.MakeColumn();
    if (!p || !outer || !inner) return Box{};
    skin::Paint(p, skin::Art::Scroll);
    skin::Pad(p, skin::Art::Scroll, {22, 12, 22, 18});   // inside the scroll's rods and edges
    p.Content(outer);
    AddToHorizontalBox(row, p, fill, padRight).Align(HAlign::Fill, VAlign::Fill);
    if (scroll) { outer.Add(inner).Padding({0, 0, 12, 0}); return inner; }
    return outer;
}

// The multiplayer page: a Border over the whole menu (opaque, so the stock buttons underneath can neither be seen nor
// clicked) with the header, the body (two panels, or SETTINGS) and the footer (BACK, SETTINGS, REPORT A BUG).
bool BuildPanel() {
    Canvas root = g.kit.Root();
    if (!root) { HSM_LOG("ui: menu root is not a CanvasPanel; panel disabled"); return false; }
    g.panel = g.kit.MakePanel();
    Box page = g.kit.MakeColumn();
    Box header = g.kit.MakeRow();
    Box body = g.kit.MakeColumn();
    g.mainRow = g.kit.MakeRow();
    Box settingsRow = g.kit.MakeRow();
    Box footer = g.kit.MakeRow();
    Fixed rule = g.kit.MakeFixed();
    Panel ruleLine = g.kit.MakePanel();
    g.pill = g.kit.MakePanel();
    g.pillFrame = g.kit.MakePanel();
    if (!g.panel || !page || !header || !body || !g.mainRow || !settingsRow || !footer || !rule || !ruleLine || !g.pill || !g.pillFrame) {
        HSM_LOG("ui: multiplayer page widgets could not be made; panel disabled");
        g.panel = Panel{};
        return false;
    }
    root.Add(g.panel).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(100);
    // The dropdown list: over the page, under its own windows (the question, the password, the warning: Z 110 and up).
    if (!g.drops.Build(g.kit, root, kDropdownZ)) HSM_LOG("ui: the lists could not be made; the dropdown rows step with their arrows");
    skin::Paint(g.panel, skin::Art::Backdrop, style::kMenuBackdrop);
    g.panel.Padding({48, 26, 48, 22});
    g.panel.Content(page);
    // The header and the footer each sit on a band of the game's old paper (the page's text is ink throughout).
    Panel headBand = g.kit.MakePanel(), footBand = g.kit.MakePanel();
    Box head = g.kit.MakeColumn(), foot = g.kit.MakeColumn();
    if (!headBand || !footBand || !head || !foot) {
        HSM_LOG("ui: multiplayer page widgets could not be made; panel disabled");
        g.panel = Panel{};
        return false;
    }
    skin::Paint(headBand, skin::Art::Sheet);
    skin::Pad(headBand, skin::Art::Sheet, {30, 4, 30, 4});
    headBand.Content(head);
    AddToVerticalBox(page, headBand, 0);
    skin::Paint(footBand, skin::Art::Sheet);
    skin::Pad(footBand, skin::Art::Sheet, {30, 2, 30, 2});
    footBand.Content(foot);

    // Header: the title, our Steam name, the state pill (outlined) and the version badge; under it the accent rule and
    // the lobby's last message.
    AddToVerticalBox(head, header, 0);
    g.title = NewText(skin::Title("Multiplayer"), style::type::kPage, style::kMenuAccentInk);
    g.me = NewText("", style::type::kRow, style::kMenuDim);
    g.pillText = NewText("", style::type::kBody, style::kMenuDim);
    g.badge = NewText("", style::type::kHint, style::kMenuDim);
    if (g.title) AddToHorizontalBox(header, g.title, 0, 28).Align(HAlign::Left, VAlign::Center);
    if (g.me) AddToHorizontalBox(header, g.me, 1, 20).Align(HAlign::Left, VAlign::Center);
    if (g.pillText) { skin::Paint(g.pill, skin::Art::Card); skin::Pad(g.pill, skin::Art::Card, {10, 6, 10, 6}); g.pill.Content(g.pillText); }
    g.pillFrame.Padding({1, 1, 1, 1});
    g.pillFrame.Content(g.pill);
    AddToHorizontalBox(header, g.pillFrame, 0, 18).Align(HAlign::Left, VAlign::Center);
    if (g.badge) AddToHorizontalBox(header, g.badge, 0, 0).Align(HAlign::Left, VAlign::Center);
    rule.Height(3);
    rule.Content(ruleLine);
    ruleLine.Color(style::kMenuAccent);
    AddToVerticalBox(head, rule, 10);
    g.status = NewText("", style::type::kBody, style::kMenuDim);
    if (g.status) AddToVerticalBox(head, g.status, 6);
    g.notice = NewText("", style::type::kEmphasis, style::kMenuAccentInk);   // Session::TakeNotice: kicked, banned, refused; until the page closes
    if (g.notice) { g.notice.Wrap(true); g.notice.Show(false); AddToVerticalBox(head, g.notice, 8); }
    g.updateNote = NewText("", style::type::kBody, style::kMenuOk);   // net/update_check.h: only when a newer release exists
    g.updateRow = g.kit.MakeRow();
    g.updateNow = MakeFlat("UPDATE NOW", style::type::kBody, {14, 5, 14, 5});
    if (g.updateNote && g.updateRow && g.updateNow.card) {
        g.updateNote.Wrap(true);
        AddToHorizontalBox(g.updateRow, g.updateNote, 1, 14).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(g.updateRow, g.updateNow.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        g.updateRow.Show(false);
        AddToVerticalBox(head, g.updateRow, 6);
    } else if (g.updateNote) {
        g.updateNote.Show(false);
        AddToVerticalBox(head, g.updateNote, 6);
    }

    FillVerticalBox(page, body, 16);
    FillVerticalBox(body, g.mainRow, 0);
    FillVerticalBox(body, settingsRow, 0);
    Box left = AddColumn(g.mainRow, 0.95f, 20, true);
    Box right = AddColumn(g.mainRow, 1.45f, 0, false);
    g.idleLeft = g.kit.MakeColumn(); g.inLeft = g.kit.MakeColumn();
    g.idleRight = g.kit.MakeColumn(); g.inRight = g.kit.MakeColumn();
    if (!left || !right || !g.idleLeft || !g.inLeft || !g.idleRight || !g.inRight) {
        HSM_LOG("ui: multiplayer page columns could not be made; panel disabled");
        g.panel = Panel{};
        return false;
    }
    AddToVerticalBox(left, g.idleLeft, 0);
    AddToVerticalBox(left, g.inLeft, 0);
    FillVerticalBox(right, g.idleRight, 0);
    FillVerticalBox(right, g.inRight, 0);
    BuildIdleLeft();
    BuildIdleRight();
    BuildInLeft();
    BuildInRight();
    g.settingsView = settingsRow;
    if (Box settings = AddColumn(settingsRow, 1, 0, true)) BuildSettingsView(settings);
    g.settingsView.Show(false);
    BuildAsk(root);
    BuildPassword(root);
    BuildWarn(root);

    // REPORT A BUG opens a text box over the footer: SEND and CANCEL; the text becomes a public GitHub issue
    // (net/report_client).
    g.reportSend = MakeFlat("SEND", style::type::kBody, {14, 5, 14, 5});
    g.reportCancel = MakeFlat("CANCEL", style::type::kBody, {14, 5, 14, 5});
    g.reportText = g.kit.MakeEditBox();
    g.reportBox = g.kit.MakeRow();
    g.reportNote = NewText("", style::type::kHint, style::kMenuDim);
    if (g.reportBox && g.reportText) {
        g.reportText.Hint("What went wrong? One line is enough");
        AddToHorizontalBox(g.reportBox, g.reportText, 1, 8).Align(HAlign::Fill, VAlign::Center);
        AddFlatH(g.reportBox, g.reportSend, 8); AddFlatH(g.reportBox, g.reportCancel, 0);
        AddToVerticalBox(foot, g.reportBox, 0);
        g.reportBox.Show(false);
    }
    if (g.reportNote) { g.reportNote.Wrap(true); AddToVerticalBox(foot, g.reportNote, 4); }

    AddToVerticalBox(foot, footer, 8);
    AddToVerticalBox(page, footBand, 14);
    // The bottom-left button says where it goes (FooterFor): BACK TO MAIN MENU on the lobby list, BACK TO LOBBY (LIST) from SETTINGS
    // and the class page, and LEAVE LOBBY on the lobby itself (where BACK is hidden); INVITE FRIENDS is beside it, in a lobby.
    g.close = MakeFlat("BACK TO MAIN MENU", style::type::kRow);
    g.leave = MakeFlat("LEAVE LOBBY", style::type::kRow);
    g.invite = MakeFlat("INVITE FRIENDS", style::type::kRow);
    g.classOpen = MakeFlat("YOUR CLASS", style::type::kRow);
    g.settingsOpen = MakeFlat("SETTINGS", style::type::kRow);
    g.reportOpen = MakeFlat("REPORT A BUG", style::type::kRow);
    AddFlatH(footer, g.close, 10); AddFlatH(footer, g.leave, 10); AddFlatH(footer, g.invite, 10); AddFlatH(footer, g.classOpen, 10);
    AddFlatH(footer, g.settingsOpen, 10); AddFlatH(footer, g.reportOpen, 0);
    g.panel.Show(false);
    return true;
}

void Reset() { g = State{}; SetLobbyTyping(false); }

// UI_Startup_Menu (this build's main menu): image ribbons, the Multiplayer entry goes by Button_3 (Quit; Button_4 is
// Free Mode, decomp UI/UI_Startup_Menu.cpp LatentResume_L44). UI_MainMenuSNF (older builds): text buttons, anchored on
// ButtonFreeMode with TextFreeMode's font. How the entry is styled is MakeOpenButton's; the page's text takes a new
// TextBlock's font.
void Build() {
    bool startup = SameNameAscii(ObjectName(reinterpret_cast<UObject*>(g.menuClass)), "UI_Startup_Menu_C");
    const char* anchorName = startup ? "Button_3" : "ButtonFreeMode";
    UObject** anchorPtr = Field<UObject*>(g.menu, anchorName);
    g.built = true;
    if (!anchorPtr || !*anchorPtr) { HSM_LOG("ui: %s not found on the main menu", anchorName); return; }
    UObject* anchor = *anchorPtr;
    g.kit = Kit(g.menu, Vis::Visible);
    g.maker = mp::Maker(&g.kit);
    ProbeFont(g.kit, g.menu);   // TextFreeMode on the older menu, else a new TextBlock: sizes by font, not by scale
    if (!InjectOpenButton(anchor, startup)) return;
    BuildPanel();
}

LobbyPhase PhaseOf(game::Lobby::State s) {
    switch (s) {
        case game::Lobby::State::Idle: return LobbyPhase::Idle;
        case game::Lobby::State::Creating: return LobbyPhase::Creating;
        case game::Lobby::State::Joining: return LobbyPhase::Joining;
        case game::Lobby::State::InLobby: return LobbyPhase::InLobby;
        case game::Lobby::State::Offline: break;
    }
    return LobbyPhase::Offline;
}

LobbyLocal Local(game::Lobby& lobby, const std::vector<JoinedBuild>& joined) {
    LobbyLocal l;
    l.phase = PhaseOf(lobby.state());
    l.host = lobby.IsHost();
    l.publicLobby = lobby.type() == steam::LobbyType::Public;
    l.myName = steam::PersonaName(steam::MySteamId());
    l.status = lobby.status();
    l.version = game::kModVersion;
    l.myBuild = game::ModBuild();
    l.allowBuildMismatch = game::AllowBuildMismatch();
    l.damage = game::VanillaDamageRange();
    l.maxPlayers = lobby.MaxPlayers();
    l.locked = lobby.Locked();
    if (lobby.InLobby()) {
        l.config = lobby.config();
        l.mapPick = lobby.MapPick();
        l.fightPick = lobby.FightPick();
        for (const ModMapEntry& e : game::modmaps::Maps()) {
            const auto& paks = game::modmaps::Paks();
            l.modMaps.push_back({e.path, e.display, e.pak >= 0 && size_t(e.pak) < paks.size() ? paks[size_t(e.pak)].name : std::string()});
        }
        l.modPickPending = l.host && game::modmaps::PickPending(l.mapPick);
        l.progression = lobby.Progression();
        l.playerCount = lobby.PlayerCount();   // the cards: Duel is for exactly 2, Last Man Standing for 2 or more
        const bool server = lobby.ServerLobby();
        for (const game::LobbyMember& m : lobby.members()) {
            if (server && m.host) continue;   // role=server: the server is not a player
            LobbyPlayer p;
            p.name = m.name; p.host = m.host; p.ready = m.ready; p.me = m.id == steam::MySteamId();
            p.team = m.team; p.hostOverride = m.hostOverride; p.build = m.build; p.steamName = m.steamName;
            for (const JoinedBuild& j : joined)   // the host has this player's Hello: its build is what the session judges by, and none means an older HSMMod
                if (j.key == m.id) { p.joined = true; p.build = j.build; }
            l.players.push_back(p);
        }
    }
    for (const game::LobbyListing& f : lobby.listings()) {
        LobbyFound o;
        o.name = f.name; o.map = f.map; o.mode = f.mode; o.scenario = f.scenario; o.players = f.players; o.maxPlayers = f.maxPlayers; o.locked = f.locked; o.compatible = f.compatible; o.reason = f.reason;
        o.mods = f.mods; o.region = f.region;
        o.switchable = f.switchable;
        if (f.switchable) o.switchTag = SwitchTag(f.plan);
        if (f.pack.any()) {   // the host's modpack, against this machine's files now (the hashes may still be running)
            const PackVerdict v = game::modpack::Check(f.pack).verdict;
            o.pack = PackRowText(f.pack, v);
            if (v != PackVerdict::Ok) { o.switchable = false; if (o.compatible) { o.compatible = false; o.reason = PackRowReason(v); } }   // no restart fixes a missing pack
        }
        l.found.push_back(o);
    }
    return l;
}


// What the page shows, for the footer: the lobby list or the lobby, or the Settings view or the class page opened over either.
PageView ViewOf(const LobbyScene& s) {
    return g.settingsShown ? PageView::Settings : g.classShown ? PageView::Class : s.in ? PageView::Lobby : PageView::List;
}

// The page a list was opened on, as one number: when it changes (another tab, a window over the page, the page closing) the list closes.
uint32_t DropContext() {
    return PageContextKey(g.panelOpen, ViewOf(g.scene), g.scene.in, g.tab, g.rulesTab, g.askOpen || g.pwOpen || g.warnOpen);
}

// A browser filter chip's text: the region and the mode open lists, and say so with the chevron (v closed, ^ open); the rest are the label.
std::string ChipText(size_t i, const FilterChip& chip) {
    if (!chip.choice || !g.drops) return chip.label;
    return chip.label + (g.drops.IsOpen(dropdown_owner::kFilterChip + uint32_t(i)) ? "  ^" : "  v");
}

// The tab whose page is on screen. Outside a lobby, and while the class page is open apart, that column only ever holds the classes.
size_t PageTab(const LobbyScene& s) { return g.classShown || !s.in ? kClassesTab : ClampTab(g.tab, ClassesTabShown(s)); }

// The tabs: the chosen one in the accent's ink and underlined, the others dim (brighter under the mouse). CLASSES is there only
// in a lobby that uses classes.
void PaintTabs() {
    const bool classesTab = ClassesTabShown(g.scene);
    for (size_t i = 0; i < kTabCount; ++i) {
        TabView& t = g.tabs[i];
        if (!t.card) continue;
        t.card.button.Show(TabShown(i, classesTab));
        const bool on = i == ClampTab(g.tab, classesTab);
        const bool hover = t.card.button.Hovered() || t.card.button.Focused();
        t.card.frame.Color(kClear);
        t.card.face.Color(kClear);
        t.label.Color(on ? style::kMenuAccentInk : hover ? style::kMenuText : style::kMenuDim);
        t.line.Color(on ? style::kMenuAccent : kClear);
        if (g.tabPages[i]) g.tabPages[i].Show(i == PageTab(g.scene));
    }
}

// The scene's row for the i-th stepper (a LobbyRow value). Every row, the lobby's own limit and the special ones too, is in the
// scene's list.
const SettingRow* RowFor(const LobbyScene& s, size_t i) {
    for (const SettingRow& r : s.settings) if (size_t(r.id) == i) return &r;
    return nullptr;
}

// The rows of this RULES sub-tab are on screen (the host's sliders and boxes are live only then).
bool RulesShown(LobbySection sct) { return g.tab == kRulesTab && !g.settingsShown && !g.classShown && g.rulesTab == sct; }

// The class page is on screen: the CLASSES tab, or the page opened apart (outside a lobby, or in a lobby without the tab).
bool ClassesOpen(const LobbyScene& s) { return !g.settingsShown && (g.classShown || (s.in && ClampTab(g.tab, ClassesTabShown(s)) == kClassesTab)); }

// Chips under a stepper row (BuildChipRows): shown with the row's chips, the ones that are on in the accent.
template <size_t N>
void DrawChipRows(std::array<Box, 2>& rows, std::array<Flat, N>& chips, const std::vector<RowChip>* shown, bool enabled) {
    const bool show = shown && !shown->empty();
    for (Box& b : rows) if (b) b.Show(show);
    for (size_t k = 0; k < chips.size(); ++k) {
        Flat& c = chips[k];
        if (!c.card) continue;
        const bool on = show && k < shown->size() && (*shown)[k].on;
        if (show && k < shown->size()) c.label.Set((*shown)[k].label);
        c.label.Color(on ? style::kMenuOnPicked : style::kMenuText);
        c.card.selected = on;
        Dimmed(c.card, show && enabled);
    }
}

void Draw(const LobbyScene& s) {
    // Nobody lands on the classes by arriving in a lobby or by coming out of one: the class page closes at either, and a CLASSES
    // tab that went away (we left, or the host turned Classes off) falls back to MODE.
    if (s.in != g.wasIn) { g.wasIn = s.in; g.classShown = false; }
    g.tab = ClampTab(g.tab, ClassesTabShown(s));
    g.pillText.Set(s.pill.text);
    g.pillText.Color(style::OnParchment(s.pill.color));
    // On the game's button art the ink says the state; the plain pill keeps its coloured outline.
    g.pillFrame.Color(skin::Loaded(skin::Art::Card) ? kClear : style::Faded(style::OnParchment(s.pill.color), 0.7f));
    g.me.Set(s.me);
    g.badge.Set(s.badge);
    g.myRegion.label.Set(MyRegionText(s.regionButton));
    g.status.Set(s.status);
    g.status.Show(!s.status.empty());

    g.mainRow.Show(!g.settingsShown);
    g.settingsView.Show(g.settingsShown);
    g.settingsOpen.card.selected = g.settingsShown;
    g.classOpen.card.selected = ClassesOpen(s);
    g.settingsOpen.label.Color(InkOn(g.settingsOpen.card, style::kMenuText));
    g.classOpen.label.Color(InkOn(g.classOpen.card, style::kMenuText));
    g.idleLeft.Show(!s.in);
    g.idleRight.Show(!s.in && !g.classShown);
    g.inLeft.Show(s.in);
    g.inRight.Show(s.in || g.classShown);
    const bool tabs = s.in && !g.classShown;       // the class page opened apart has no tabs, and no START under it
    if (g.tabBar) g.tabBar.Show(tabs);
    if (g.tabRule) g.tabRule.Show(tabs);
    if (g.tabFoot) g.tabFoot.Show(tabs);
    const Footer foot = FooterFor(s.in, ViewOf(s));   // the bottom-left button says where it goes; LEAVE LOBBY and INVITE FRIENDS are the lobby's own
    g.close.card.button.Show(!foot.back.empty());
    if (!foot.back.empty()) g.close.label.Set(foot.back);
    g.leave.card.button.Show(foot.leave);
    g.invite.card.button.Show(foot.invite);

    for (HostView* h : {&g.hostFriends, &g.hostPublic}) Live(h->card, s.idle);
    g.idleHint.Show(!s.in);
    Live(g.refresh.card, s.idle);
    for (size_t i = 0; i < g.chips.size(); ++i) {
        Flat& c = g.chips[i];
        if (!c.card) continue;
        const bool show = i < s.filters.size();
        c.card.button.Show(show);
        if (!show) continue;
        c.label.Set(ChipText(i, s.filters[i]));
        c.label.Color(s.filters[i].on ? style::kMenuOnPicked : style::kMenuText);
        c.card.selected = s.filters[i].on;
    }
    g.browserCount.Set(s.browserCount);
    g.browserEmpty.Set(s.browserEmpty);
    g.browserEmpty.Show(s.idle && s.browser.empty() && !s.browserEmpty.empty());
    for (size_t i = 0; i < g.rows.size(); ++i) {
        BrowserView& v = g.rows[i];
        bool show = i < s.browser.size();
        v.card.button.Show(show);
        if (!show) continue;
        const BrowserRow& r = s.browser[i];
        v.name.Set(r.name);
        v.detail.Set(r.detail);
        v.detail.Show(!r.detail.empty());
        v.players.Set(r.players);
        v.note.Set(r.joinable && !r.restart ? "JOIN" : r.note.text);   // a restart row keeps its "Restart: 2 mods off"
        v.note.Color(r.joinable && !r.restart ? style::kMenuOk : style::kMenuWarn);
        Live(v.card, r.joinable);        // incompatible and full lobbies stay readable, dimmed, with the reason
    }

    g.playersTitle.Set(skin::Title(s.playersTitle.empty() ? "PLAYERS" : s.playersTitle));
    if (g.moreMembers) g.moreMembers.Show(s.roster.size() > size_t(kMemberRowsShown));
    for (size_t i = 0; i < g.members.size(); ++i) {
        MemberView& v = g.members[i];
        bool show = i < s.roster.size();
        v.card.button.Show(show);
        if (!show) continue;
        const RosterRow& r = s.roster[i];
        v.name.Set(r.steam.empty() ? r.name : r.name + "  (" + r.steam + ")");   // the host sees the Steam name behind a nickname
        v.badge.Show(r.host);
        v.team.Set(r.team + (r.moved ? " (moved)" : ""));
        v.team.Show(!r.team.empty());
        v.swatch.Color(r.swatch);
        v.ready.Visibility(r.ready ? Vis::HitTestInvisible : Vis::Hidden);
        v.card.selected = r.me;                 // our own row, framed
        v.card.live = r.movable;                // the host clicks a player to move them to the next team
    }
    g.readyUp.card.button.Show(s.ready);
    g.readyUp.label.Set(s.readyLabel);
    g.readyUp.label.Color(s.readyOn ? style::kMenuOnPicked : style::kMenuText);
    g.readyUp.card.selected = s.readyOn;
    g.balance.card.button.Show(s.balance);
    if (g.shuffle.card) g.shuffle.card.button.Show(s.shuffle);
    if (g.classRow) { g.classRow.Show(!s.classLine.empty()); g.classRowText.Set(s.classLine); }
    if (g.pwHostBox) g.pwHostBox.Show(s.hostPassword);
    g.rulesNote.Set(s.rulesNote); g.rulesNote.Show(!s.rulesNote.empty());
    if (g.lobbyName.box) g.lobbyName.box.Show(s.hostPassword);   // the host's rows: password and name

    for (size_t i = 0; i < g.modes.size() && i < s.modes.size(); ++i) {
        ModeView& v = g.modes[i];
        const ModeCard& m = s.modes[i];
        if (v.card) v.card.button.Show(m.shown);   // Crypt Royale and Grave Robbers only on the Crypts map; the CRYPTS card when not on it
        v.name.Set(m.name);
        v.name.Color(m.selected ? style::kMenuOnPicked : style::kMenuText);
        v.blurb.Color(m.selected ? style::kMenuOnPicked : style::kMenuDim);
        v.blurb.Set(m.hint.empty() ? m.description : m.description + "\n" + m.hint);   // a card the player count rules out says why
        v.card.selected = m.selected;
        Dimmed(v.card, m.enabled);         // a click on a card the player cannot use says why (HandleClicks)
    }
    const std::vector<LobbySection> subs = RulesTabs(s);   // RULES: a sub-tab for each section the mode has rows in
    g.rulesTab = ClampRulesTab(subs, g.rulesTab);
    for (size_t sct = 0; sct < kSectionCount; ++sct) {
        Flat& t = g.subTabs[sct];
        const bool has = std::find(subs.begin(), subs.end(), LobbySection(sct)) != subs.end();
        const bool on = has && LobbySection(sct) == g.rulesTab;
        if (t.card) { t.card.button.Show(has); t.card.selected = on; t.label.Color(on ? style::kMenuOnPicked : style::kMenuText); }
        if (g.sections[sct]) g.sections[sct].Show(LobbySection(sct) == g.rulesTab);   // only the chosen sub-tab's rows
    }
    for (size_t i = 0; i < g.steppers.size(); ++i) {
        StepperView& v = g.steppers[i];
        if (!v.box) continue;
        const SettingRow* row = RowFor(s, i);
        v.box.Show(row != nullptr);
        if (!row) continue;
        v.label.Set(row->label);
        v.label.Color(row->enabled ? style::kMenuText : style::kMenuDim);
        v.value.Set(row->value);
        v.hint.Set(row->hint);
        v.hint.Show(!row->hint.empty());
        for (Flat* f : {&v.prev, &v.next}) {
            Dimmed(f->card, row->enabled);
            // a row of chips is changed by its chips, and a dropdown row by its list (its arrows come back when no list could be built)
            if (f->card) f->card.button.Show(RowHasArrows(*row) || (row->kind == RowKind::Choice && !v.plate.card));
        }
    }
    {   // the chips under a row: the Weapons types, the tiers under Gear, the Crypts card's Starting tier
        const SettingRow* w = RowFor(s, size_t(LobbyRow::Weapons));
        const SettingRow* gear = RowFor(s, size_t(LobbyRow::Gear));
        const SettingRow* start = RowFor(s, size_t(LobbyRow::Tier));
        DrawChipRows(g.weaponChipRows, g.weaponChips, w ? &w->chips : nullptr, w && w->enabled);
        DrawChipRows(g.gearTierRows, g.gearTierChips, gear ? &gear->tierChips : nullptr, gear && gear->enabled);
        DrawChipRows(g.startTierRows, g.startTierChips, start ? &start->tierChips : nullptr, start && start->enabled);
    }
    for (size_t i = 0; i < g.maps.size(); ++i) {
        MapView& v = g.maps[i];
        if (!v.card) continue;
        v.card.button.Show(i < s.maps.size());          // none while Progression is the choice
        if (i >= s.maps.size()) continue;
        const MapCard& m = s.maps[i];
        if (m.modded) v.name.Set(m.name);
        v.name.Color(m.selected ? style::kMenuOnPicked : style::kMenuText);
        v.tag.Set(m.allowed ? m.tag : m.reason);
        v.tag.Color(m.selected ? style::kMenuOnPicked : m.allowed ? style::kMenuDim : style::kMenuWarn);
        v.card.selected = m.selected;
        Dimmed(v.card, m.enabled);   // one dimmed look for a map the mode cannot use and one that is not a member's to pick (the tag says which)
    }
    g.noMaps.Show(s.in && s.maps.empty());
    g.summary.Set(s.summary);
    g.summary.Show(!s.summary.empty());
    g.hostOnly.Set(s.hostOnly);
    g.hostOnly.Show(!s.hostOnly.empty());
    g.start.button.Show(s.in);                  // always on screen in a lobby; blocked, it is dimmed and a click says why
    g.start.button.Opacity(s.start ? 1.f : style::kDisabledAlpha);
    g.waiting.Set(s.waiting);
    g.waiting.Color(s.waitingWarn ? style::kMenuWarn : style::kMenuDim);
    g.waiting.Show(!s.waiting.empty());
    g.scene = s;
}

// The engine reads image files, not pixels: Steam's RGBA goes into a BMP in memory for ImportBufferAsTexture2D.
UObject* ImportAvatar(int image) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!steam::AvatarRGBA(image, w, h, rgba)) return nullptr;
    std::vector<uint8_t> file = RgbaToBmp(rgba.data(), rgba.size(), w, h);
    UObject* cls = FindObject("KismetRenderingLibrary", "Class");
    UObject* lib = cls ? DefaultObject(reinterpret_cast<UClass*>(cls)) : nullptr;
    if (file.empty() || !lib) return nullptr;
    FByteArray buffer{file.data(), int32_t(file.size()), int32_t(file.size())};   // read by the engine, ours
    Call c(lib, "ImportBufferAsTexture2D");
    c.set<UObject*>("WorldContextObject", g.menu).set("Buffer", buffer);
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

// The player's avatar texture, or null while there is none (yet): Steam answers -1 while it fetches an avatar, so a
// miss is asked again a second later, kAvatarTries times.
UObject* AvatarFor(steam::SteamId id, ULONGLONG now) {
    Avatar* a = nullptr;
    for (Avatar& e : g.avatars) if (e.id == id) a = &e;
    if (!a) { g.avatars.push_back(Avatar{id}); a = &g.avatars.back(); }
    if (a->texture && ObjectAt(a->index) == a->texture && ClassOf(a->texture) == a->cls) return a->texture;
    if (a->texture) { a->texture = nullptr; a->tries = 0; }   // collected: make it again
    if (now < a->nextTry || a->tries >= kAvatarTries) return nullptr;
    ++a->tries;
    a->nextTry = now + 1000;
    int image = steam::MediumAvatar(id);
    if (image == 0) { a->tries = kAvatarTries; return nullptr; }   // no avatar set
    if (image < 0) return nullptr;
    a->texture = ImportAvatar(image);
    if (!a->texture) {
        a->tries = kAvatarTries;
        if (!g.avatarFailLogged) { g.avatarFailLogged = true; HSM_LOG("ui: an avatar could not be made into a texture"); }
        return nullptr;
    }
    a->index = ObjectIndex(a->texture);
    a->cls = ClassOf(a->texture);
    return a->texture;
}

// The chat box (in a lobby only): the history's last lines, each sender's name in the colour the line came with.
void DrawChat(bool in) {
    if (!g.chatBox) return;
    g.chatBox.Show(in);
    if (!in) return;
    const std::vector<ChatEntry> history = ChatHistory();
    const size_t first = history.size() > g.chatRows.size() ? history.size() - g.chatRows.size() : 0;
    for (size_t i = 0; i < g.chatRows.size(); ++i) {
        ChatRow& r = g.chatRows[i];
        if (!r.row) continue;
        const bool show = first + i < history.size();
        r.row.Show(show);
        if (!show) continue;
        const ChatEntry& e = history[first + i];
        r.name.Set(e.name);
        r.name.Color(style::OnParchment(e.ink));
        r.text.Set(e.text);
    }
    if (ChatLinesAdded() != g.chatSeen) { g.chatSeen = ChatLinesAdded(); g.chatScroll.ScrollToEnd(); }
}

// The MODS tab's input: the session's list (Session::ModsView), and the rule: the host's own (HostRule, its INI choice),
// a member's from the lobby data.
LobbyModsLocal ModsLocal(Session& session, game::Lobby& lobby) {
    LobbyModsLocal l = session.ModsView();
    l.host = lobby.IsHost();
    l.rulesOn = game::modrules::Enabled();
    if (l.host) {
        l.rule = game::modrules::HostRule(lobby);
        l.chosen = game::modrules::Chosen();
        l.classPicked = session.ClassName() != kPlainClass;   // #3396: the Mods allowed hint says when it is off
    } else {
        l.rule = lobby.RoomRule(&l.hostHasRule);
        // A host with mod_rules=0 announces Anything with HS Enhancer allowed (modrules::LobbyTick) and sends no SessionMods.
        // Until a list comes, that rule means it checks nothing: say so rather than wait. A host that chose the same rule
        // sends the list on admission, and from then on the tab shows it like any other.
        const ModRule open = [] { ModRule r = NoRule(); r.blockHse = false; return r; }();
        if (l.rule == open && !l.haveList) l.hostHasRule = false;
    }
    return l;
}

Rgba ModToneColor(ModTone t) {
    switch (t) {
        case ModTone::Ok: return style::kMenuOk;
        case ModTone::Warn: return style::kMenuWarn;
        case ModTone::Bad: return style::kMenuAccentInk;
        case ModTone::Dim: break;
    }
    return style::kMenuDim;
}

void DrawMods(const LobbyModsScene& s) {
    for (size_t i = 0; i < kModRuleRows; ++i) {
        StepperView& v = g.modRules[i];
        if (!v.box) continue;
        const ModRuleView& r = s.rules[i];
        v.label.Set(r.label);
        v.label.Color(r.enabled ? style::kMenuText : style::kMenuDim);
        v.value.Set(r.value);
        v.hint.Set(r.hint);
        v.hint.Show(!r.hint.empty());
        for (Flat* f : {&v.prev, &v.next}) {
            Dimmed(f->card, r.enabled);
            if (f->card) f->card.button.Show(!v.plate.card);       // Mods allowed and Classes are dropdowns: their plates, not the arrows
        }
    }
    g.modSummary.Show(false);                                  // the "In force" line repeated the rows (PR3); the widget stays
    g.modStatus.Set(s.status);
    g.modStatus.Show(!s.status.empty());
    static const std::string kDot = " \xC2\xB7 ";
    for (size_t i = 0; i < g.modItems.size(); ++i) {
        ModItemView& v = g.modItems[i];
        if (!v.box) continue;
        const bool show = i < s.items.size();
        v.box.Show(show);
        if (!show) continue;
        const ModRowView& r = s.items[i];
        v.name.Set(r.name);
        v.name.Color(r.refused ? style::kMenuAccentInk : style::kMenuText);
        v.detail.Set(r.kind + kDot + r.holders);
        v.note.Set(r.note);
        v.note.Color(r.refused ? style::kMenuAccentInk : style::kMenuWarn);
        v.note.Show(!r.note.empty());
        v.verdict.Set(r.verdict);
        v.verdict.Color(ModToneColor(r.tone));
    }
    const size_t more = s.items.size() > g.modItems.size() ? s.items.size() - g.modItems.size() : 0;
    g.modMore.Set(more ? "+" + std::to_string(more) + " more (the host's log lists them all)" : std::string());
    g.modMore.Show(more > 0);
    g.modEmpty.Set(s.empty);
    g.modEmpty.Show(!s.empty.empty());
    g.modTruncated.Set(s.truncated);
    g.modTruncated.Show(!s.truncated.empty());
    if (g.modPendingTitle) g.modPendingTitle.Show(!s.pending.empty());
    for (size_t i = 0; i < g.modPending.size(); ++i) {
        Text& t = g.modPending[i];
        if (!t) continue;
        const bool show = i < s.pending.size();
        t.Show(show);
        if (show) t.Set(s.pending[i]);
    }
    g.modPrivacy.Set(s.privacy);
    g.modsScene = s;
}

// The classes may never be opened, so they can't be what moves a pick that can't fight. Runs with the page open, and in a lobby at the
// menu: an invite joins without the page.
void FixClassPick(Session& session) {
    const std::string picked = session.ClassName();
    const std::optional<std::string> moved = ClassFallback(picked, session.HasWolverine(), session.ClassesOff());
    if (!moved) return;
    HSM_LOG("classes: %s can't fight with the mods that are on; moved to %s", picked.c_str(), moved->c_str());
    session.SetClassName(*moved);
}

void UpdatePanel(Session& session) {
    game::Lobby& lobby = session.lobby();
    LobbyLocal local = Local(lobby, session.JoinedBuilds());
    local.filter = g.filter;
    local.regionPick = session.RegionPick(); local.regionGuess = session.RegionGuessed();
    local.filter.mine = EffectiveRegion(local.regionPick, local.regionGuess);   // "my region" in the browser
    if (!session.Nickname().empty()) local.myName = session.Nickname();
    if (session.HideNames()) for (LobbyPlayer& p : local.players) p.steamName.clear();   // no Steam name beside "Player 2"
    // The CLASSES tab and YOUR CLASS follow the Classes row (On or Unrestricted, with any Mods allowed preset).
    local.myClass = LobbyClassName(lobby.ClassesModeHere(), session.ClassesOff() ? std::string(kHseClassName) : session.ClassLabel(session.ClassName()));
    local.classBudget = ClassBudgetApplies(lobby.ClassesModeHere());
    const LobbyScene scene = BuildLobbyScene(local);
    // Each shown row joins the listing it came from (the filters and the sort reorder them).
    const auto& list = lobby.listings();
    for (size_t i = 0; i < g.rowLobbies.size(); ++i) {
        const bool row = lobby.state() == game::Lobby::State::Idle && i < scene.browser.size() && scene.browser[i].source < list.size();
        g.rowLobbies[i] = row ? list[scene.browser[i].source].id : 0;
    }
    const auto& mem = lobby.members();
    for (size_t i = 0; i < g.memberIds.size(); ++i) g.memberIds[i] = lobby.InLobby() && i < mem.size() ? mem[i].id : 0;
    Draw(scene);
    if (lobby.InLobby()) DrawMods(BuildLobbyMods(ModsLocal(session, lobby)));
    else g.modsScene = LobbyModsScene{};
    DrawChat(lobby.InLobby());
    std::string notice = update::Notice();
    if (const std::string build = game::GameBuildNotice(); !build.empty()) notice += (notice.empty() ? "" : "\n") + build;   // session/hsm_game_build.h
    if (const std::string install = update::InstallStatus(); !install.empty()) notice += (notice.empty() ? "" : "\n") + install;
    if (const std::string proxy = ue4ss::Notice(); !proxy.empty()) notice += (notice.empty() ? "" : "\n") + proxy;
    g.updateNote.Set(notice);
    if (g.updateRow) {
        g.updateRow.Show(!notice.empty());
        if (g.updateNow.card) g.updateNow.card.button.Show(update::CanInstall());
    } else {
        g.updateNote.Show(!notice.empty());
    }
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g.members.size(); ++i) {
        UObject* texture = g.memberIds[i] ? AvatarFor(g.memberIds[i], now) : nullptr;
        if (texture) g.members[i].avatar.Texture(texture);
        g.members[i].avatar.Visibility(texture ? Vis::HitTestInvisible : Vis::Hidden);
    }
}

void UpdateOpenLabel(Session& session) {
    game::Lobby& lobby = session.lobby();
    std::string label = "MULTIPLAYER";
    if (lobby.InLobby()) label = std::string(lobby.IsHost() ? "LOBBY (HOST) " : "LOBBY ") + std::to_string(lobby.PlayerCount()) + "/" + std::to_string(lobby.MaxPlayers());
    if (std::string crypts = session.CryptsStatus(); !crypts.empty()) label += " - " + crypts;   // Crypts: waiting in the menu
    g.openButton.label.Set(g.openButton.plain ? skin::Title(label) : label);   // the game's ribbons are titled, not shouted
}

// The lobby chat box: SEND, or Enter while the box has the keyboard (or lost it to that Enter this very frame, since the
// box commits on Enter), sends what is typed through the session (commands included). While the box has the keyboard
// the game's hotkeys stay off (ui::Typing).
void HandleChat(Session& session, bool in) {
    const bool enter = mp::KeyDown(VK_RETURN);
    const bool enterEdge = enter && !g.enterDown;
    g.enterDown = enter;
    if (!g.chatBox || !in) { g.chatFocused = false; SetLobbyTyping(false); return; }
    const bool focused = g.chatText.Focused();
    const bool send = Clicked(g.chatSend.card) || (enterEdge && (focused || g.chatFocused));
    g.chatFocused = focused;
    SetLobbyTyping(focused);
    if (!send) return;
    const std::string text = g.chatText.Value();
    if (text.empty()) return;
    g.chatText.Clear();
    g.chatText.Focus();                         // the next line can be typed straight away
    session.LobbyChatLine(text);
    g.nextText = 0;
}

void CloseDamageEdit() {
    DamageView& d = g.damage;
    d.editing = false; d.hadFocus = false; d.focused = false;
    d.edit.Show(false); d.slider.Show(true); d.number.card.button.Show(true);
    d.shown = -1;                                    // put the slider back where the value is
}

// The Damage row's slider and number, every frame. The config is written when the handle is let go (or a click on the
// bar moved it), not while it is dragged, so a drag does not flood the lobby data; the number follows the handle live.
void HandleDamage(game::Lobby& lobby, const LobbyScene& s, bool& acted) {
    DamageView& d = g.damage;
    if (!d.slider) return;
    const bool enter = mp::KeyDown(VK_RETURN), enterEdge = enter && !d.enterDown;
    const bool esc = mp::KeyDown(VK_ESCAPE), escEdge = esc && !d.escDown;
    d.enterDown = enter; d.escDown = esc;
    const bool numberClicked = Clicked(d.number.card);
    const SettingRow* row = nullptr;
    for (const SettingRow& r : s.settings) if (r.id == LobbyRow::Damage) row = &r;
    const bool live = row && row->enabled && RulesShown(SectionOf(LobbyRow::Damage));   // the host's row; a client's is shown, not changed
    const DamageRange r = game::VanillaDamageRange();
    MatchConfig c = lobby.config();
    d.slider.Range(float(r.minPct), float(DamageSliderMax(r)), float(r.stepPct));
    d.slider.Colors(style::kMenuDim, live ? style::kMenuAccentInk : style::kMenuDim);
    d.slider.Enabled(live);
    Dimmed(d.number.card, live);          // the number can say why a click does nothing; the slider (disabled) cannot
    if (numberClicked && !live && row && !row->enabled) Explain(DisabledWhy(lobby.IsHost(), std::string()));
    if (d.editing) {
        const bool focused = d.edit.Focused();
        if (focused) d.hadFocus = true;
        const bool apply = enterEdge && (focused || d.focused);   // the box commits on Enter, so it may have lost the keyboard this frame
        const bool away = d.hadFocus && !focused && !d.focused && !apply;
        d.focused = focused;
        if (focused) SetLobbyTyping(true);
        if (!live || escEdge || away) { CloseDamageEdit(); return; }
        if (!apply) return;
        const std::optional<uint16_t> typed = ParseDamageInput(d.edit.Value());
        if (!typed) { d.edit.Set(""); d.edit.Hint("0 to 1000%"); d.edit.Focus(); return; }
        c.damagePct = DamageFromInput(*typed, r);
        lobby.SetConfig(c);
        HSM_LOG("ui: player damage typed: %s", DamageLabel(c.damagePct, r).c_str());
        acted = true;
        CloseDamageEdit();
        return;
    }
    if (live && numberClicked) {
        d.editing = true; d.hadFocus = false; d.focused = false;
        d.slider.Show(false); d.number.card.button.Show(false); d.edit.Show(true);
        d.edit.Hint("");
        d.edit.Set(std::to_string(DamagePct(c.damagePct, r)));
        d.edit.Focus();
        acted = true;
        return;
    }
    const float at = d.slider.Value();
    if (live && std::isfinite(at) && d.slider.Dragging()) {
        d.dragging = true;
        d.number.label.Set(std::to_string(DamagePct(DamageFromSlider(at, r), r)) + "%");
        return;
    }
    if (live && std::isfinite(at) && d.shown >= 0 && (d.dragging || std::fabs(at - d.shown) > 0.5f)) {
        const uint16_t v = DamageFromSlider(at, r);
        if (v != c.damagePct) { c.damagePct = v; lobby.SetConfig(c); acted = true; HSM_LOG("ui: player damage slid to %s", DamageLabel(v, r).c_str()); }
    }
    d.dragging = false;
    const float want = DamageSliderPos(c.damagePct, r);
    if (d.shown < 0 || !std::isfinite(at) || std::fabs(at - want) > 0.01f) d.slider.Set(want);
    d.shown = want;
    d.number.label.Set(std::to_string(DamagePct(c.damagePct, r)) + "%");
}

void CloseGatherEdit() {
    GatherView& d = g.gather;
    d.editing = false; d.hadFocus = false; d.focused = false;
    d.edit.Show(false); d.number.card.button.Show(true);
}

// The Gather time row's value, every frame: the time on its button; a click opens the text box, Enter sets what is typed
// (match/hsm_crypts.h ParseGatherInput). The row's arrows step as before (StepGather).
void HandleGather(game::Lobby& lobby, const LobbyScene& s, bool& acted) {
    GatherView& d = g.gather;
    if (!d.number.card) return;
    const bool enter = mp::KeyDown(VK_RETURN), enterEdge = enter && !d.enterDown;
    const bool esc = mp::KeyDown(VK_ESCAPE), escEdge = esc && !d.escDown;
    d.enterDown = enter; d.escDown = esc;
    const SettingRow* row = nullptr;
    for (const SettingRow& r : s.settings) if (r.id == LobbyRow::Gather) row = &r;
    const bool live = row && row->enabled && RulesShown(SectionOf(LobbyRow::Gather));   // the host's row; a client's is shown, not changed
    Dimmed(d.number.card, live);          // dimmed, not disabled: a click on it says why when it is the host's alone
    if (d.editing) {
        const bool focused = d.edit.Focused();
        if (focused) d.hadFocus = true;
        const bool apply = enterEdge && (focused || d.focused);   // the box commits on Enter, so it may have lost the keyboard this frame
        const bool away = d.hadFocus && !focused && !d.focused && !apply;
        d.focused = focused;
        if (focused) SetLobbyTyping(true);
        if (!live || escEdge || away) { CloseGatherEdit(); return; }
        if (!apply) return;
        const std::optional<uint16_t> typed = ParseGatherInput(d.edit.Value());
        if (!typed) { d.edit.Set(""); d.edit.Hint("2 to 60 min"); d.edit.Focus(); return; }
        MatchConfig c = lobby.config();
        c.gatherS = *typed;
        lobby.SetConfig(c);
        HSM_LOG("ui: gather time typed: %u min", unsigned(*typed / 60));
        acted = true;
        CloseGatherEdit();
        return;
    }
    const bool numberClicked = Clicked(d.number.card);
    if (numberClicked && !live && row && !row->enabled) Explain(DisabledWhy(lobby.IsHost(), std::string()));
    if (live && numberClicked) {
        d.editing = true; d.hadFocus = false; d.focused = false;
        d.number.card.button.Show(false); d.edit.Show(true);
        d.edit.Hint("");
        d.edit.Set(std::to_string(lobby.config().gatherS / 60));
        d.edit.Focus();
        acted = true;
        return;
    }
    if (row) d.number.label.Set(row->value);
}

// The Dead weight and Weapon weight sliders, every frame, as the Damage one: the config is written when the handle is
// let go, the number follows it live.
void HandleWeights(game::Lobby& lobby, const LobbyScene& s, bool& acted) {
    for (WeightView& w : g.weights) {
        if (!w.slider) continue;
        const SettingRow* row = nullptr;
        for (const SettingRow& r : s.settings) if (r.id == w.id) row = &r;
        const bool live = row && row->enabled && RulesShown(SectionOf(w.id));   // the host's row; a client's is shown, not changed
        MatchConfig c = lobby.config();
        uint8_t& pct = *WeightField(c, w.id);
        w.slider.Range(0.f, float(kWeightMaxPct), float(kWeightStepPct));
        w.slider.Colors(style::kMenuDim, live ? style::kMenuAccentInk : style::kMenuDim);
        w.slider.Enabled(live);
        w.number.Color(live ? style::kMenuText : style::kMenuDim);
        const float at = w.slider.Value();
        if (live && std::isfinite(at) && w.slider.Dragging()) {
            w.dragging = true;
            w.number.Set(WeightLabel(WeightFromSlider(at)));
            continue;
        }
        if (live && std::isfinite(at) && w.shown >= 0 && (w.dragging || std::fabs(at - w.shown) > 0.5f)) {
            const uint8_t v = WeightFromSlider(at);
            if (v != pct) {
                pct = v; lobby.SetConfig(c); acted = true;
                HSM_LOG("ui: %s slid to %u%%", w.id == LobbyRow::DeadWeight ? "dead weight" : "weapon weight", unsigned(v));
            }
        }
        w.dragging = false;
        const float want = float(pct);
        if (w.shown < 0 || !std::isfinite(at) || std::fabs(at - want) > 0.01f) w.slider.Set(want);
        w.shown = want;
        w.number.Set(WeightLabel(pct));
    }
}

// The host's password row: SET (or Enter in the box) locks the lobby with what is typed, CLEAR opens it. Nobody is kicked.
// While the box has the keyboard the game's hotkeys stay off, as in the chat box.
void HandlePassword(Session& session, bool host) {
    const bool enter = mp::KeyDown(VK_RETURN);
    const bool enterEdge = enter && !g.pwHostEnterDown;
    g.pwHostEnterDown = enter;
    if (!g.pwHostBox || !host) { g.pwHostFocused = false; return; }
    const bool focused = g.pwHostText.Focused();
    const bool set = Clicked(g.pwSet.card) || (enterEdge && (focused || g.pwHostFocused));
    const bool clear = Clicked(g.pwClear.card);
    g.pwHostFocused = focused;
    if (focused) SetLobbyTyping(true);          // after HandleChat's own call: either box keeps the hotkeys off
    Paint(g.pwSet.card); Paint(g.pwClear.card);
    if (set) {
        const std::string typed = CleanPassword(g.pwHostText.Value());
        if (typed.empty()) { g.pwHostNote.Set("Type a password first, or press CLEAR to open the lobby."); g.pwHostNote.Show(true); return; }
        g.pwHostNote.Show(false);
        g.pwHostText.Clear();
        session.SetLobbyPassword(typed);
        g.nextText = 0;
    } else if (clear) {
        g.pwHostNote.Show(false);
        g.pwHostText.Clear();
        session.SetLobbyPassword("");
        g.nextText = 0;
    }
}

// A name row's polls: SET (or Enter in the box) saves what is typed, CLEAR saves an empty name. `current` is the saved name, which
// the box shows until the player types over it; `idle` is the line under the box. `save` answers "" when it took the text, else why
// not (a name with a slur in it). While the box has the keyboard the game's hotkeys stay off, as in the chat box.
template <class Save>
void HandleNameRow(NameRow& r, bool shown, const std::string& current, const std::string& idle, Save save) {
    const bool enter = mp::KeyDown(VK_RETURN);
    const bool enterEdge = enter && !r.enterDown;
    r.enterDown = enter;
    if (!r.box || !shown) { r.focused = false; return; }
    const bool focused = r.text.Focused();
    const bool set = Clicked(r.set.card) || (enterEdge && (focused || r.focused));
    const bool clear = Clicked(r.clear.card);
    r.focused = focused;
    if (focused) SetLobbyTyping(true);          // after HandleChat's own call: either box keeps the hotkeys off
    Paint(r.set.card); Paint(r.clear.card);
    if (set || clear) {
        r.refused = save(clear ? std::string() : r.text.Value());
        if (clear && r.refused.empty()) r.text.Clear();
        g.nextText = 0;
    }
    if (!focused && r.filled != current) { r.filled = current; r.text.Set(current); }
    const std::string& said = r.refused.empty() ? idle : r.refused;
    r.note.Set(said);
    r.note.Show(!said.empty());
    r.note.Color(r.refused.empty() ? style::kMenuDim : style::kMenuWarn);
}

// START: the game's red button, brighter under the mouse (without the art: the accent face, a little toward its ink).
void PaintStart() {
    if (!g.start) return;
    const bool hover = g.scene.start && (g.start.button.Hovered() || g.start.button.Focused());   // dimmed: no glow
    g.start.glow += ((hover ? 1.f : 0.f) - g.start.glow) * 0.18f;
    if (skin::Loaded(skin::Art::CardRed)) {
        skin::Paint(g.start.frame, skin::Art::CardRed, mp::Mix(style::kStartTint, {1, 1, 1, 1}, g.start.glow));
        g.start.face.Color(kClear);
        return;
    }
    g.start.frame.Texture(Look{});
    g.start.frame.Color(mp::Mix(style::kMenuAccent, style::kMenuAccentInk, g.start.glow));
    g.start.face.Color(mp::Mix(style::kMenuAccent, style::kMenuAccentInk, g.start.glow * kStartGlow));
}

void ClosePage() {
    g.panelOpen = false; g.panel.Show(false); g.notice.Show(false); g.voice.StopBinding(); g.drops.Close("page");
}

// Every frame while the page is open: each button is polled (a click is a press and a release seen in turn), the
// cards are painted for the mouse, and a click acts on what the last drawn scene offered.
void HandleClicks(Session& session) {
    game::Lobby& lobby = session.lobby();
    const bool openClicked = Clicked(g.openButton);
    Paint(g.openButton.card);
    auto openPage = [&] {
        g.panelOpen = true; g.panel.Show(true); g.nextText = 0; game::modmaps::Rescan();
        g.classShown = false;
        if (g.tab == kClassesTab) g.tab = kModeTab;   // MULTIPLAYER never opens on the classes
        g.settingsShown = false;
        if (lobby.state() == game::Lobby::State::Idle) {   // the browser is filled on arrival, as far as its region chip looks (Any region: the whole world)
            lobby.SetListDistance(DistanceFor(g.filter));
            lobby.RefreshList();
        }
    };
    if (g.askOpen) {                            // the one-time question: nothing else on the menu until it is answered
        const bool esc = mp::KeyDown(VK_ESCAPE), escEdge = esc && !g.escDown && !game::fetchlink::KeysHeld();
        g.escDown = esc;
        const bool yes = Clicked(g.askYes.card), no = Clicked(g.askNo.card) || escEdge;
        Paint(g.askYes.card); Paint(g.askNo.card);
        if (!yes && !no) return;
        g.askOpen = false; g.ask.Show(false);
        HSM_LOG("ui: help-improve question answered: %s", yes ? "yes" : "no");
        session.SetHelpImprove(yes);
        openPage();
        return;
    }
    // The password question: a locked lobby was clicked (or invited to, or the password was wrong). Nothing else on the menu
    // until it is answered; Enter joins, Esc drops the join.
    if (!g.pwOpen && g.pw && lobby.PasswordAsk()) {
        g.pwOpen = true; g.pw.Show(true);
        g.pwText.Clear(); g.pwText.Focus();
        g.pwNote.Set(lobby.PasswordNote());
        g.pwEscDown = mp::KeyDown(VK_ESCAPE);
        g.pwEnterDown = mp::KeyDown(VK_RETURN);
        g.pwFocused = true;
        if (!g.panelOpen) openPage();                  // an invite arrived on the bare main menu
    }
    if (g.pwOpen) {
        const bool esc = mp::KeyDown(VK_ESCAPE), escEdge = esc && !g.pwEscDown && !game::fetchlink::KeysHeld();
        const bool enter = mp::KeyDown(VK_RETURN), enterEdge = enter && !g.pwEnterDown;
        g.pwEscDown = esc; g.pwEnterDown = enter;
        const bool focused = g.pwText.Focused();
        const bool ok = Clicked(g.pwOk.card) || (enterEdge && (focused || g.pwFocused));   // the box commits on Enter, so it may have lost the keyboard this frame
        const bool cancel = Clicked(g.pwCancel.card) || escEdge || !lobby.PasswordAsk();
        g.pwFocused = focused;
        Paint(g.pwOk.card); Paint(g.pwCancel.card);
        SetLobbyTyping(focused);
        if (!ok && !cancel) return;
        const steam::SteamId id = lobby.PasswordAsk();
        const std::string typed = CleanPassword(g.pwText.Value());
        if (ok && !cancel && typed.empty()) { g.pwNote.Set("Type the password first."); g.pwText.Focus(); return; }
        g.pwOpen = false; g.pw.Show(false); g.pwText.Clear(); SetLobbyTyping(false);
        lobby.CancelPasswordAsk();
        if (ok && !cancel && id) { HSM_LOG("ui: joining a locked lobby with a password"); lobby.Join(id, typed); }
        g.nextText = 0;
        return;
    }
    // The switch-off question: a lobby's rule refuses mods this machine can turn off for a restart (gameplay/lobby.cpp
    // SwitchAsk), at a join or when the host changed the rule under us. HandleWarn takes the answer.
    if (!g.warnOpen && g.warn) if (const game::Lobby::SwitchAsk& ask = lobby.SwitchAskPending(); ask.lobby && ask.back) {
        // The other way: this lobby takes the mods this game has off for another one (session/hsm_mod_switch.h PlanBack).
        g.warnKind = State::WarnKind::Switch;
        g.warnTitle.Set("YOUR MODS CAN COME BACK");
        std::string body = "Host allows: " + ask.ruleText + ". These are off for another lobby, and this one takes them:";
        for (const std::string& line : BackSummary(ask.backPlan, 6)) body += "\n   " + line;
        if (ask.backPlan.lockHse) body += "\nHS Enhancer stays locked: this lobby blocks it.";
        g.warnBody.Set(body);
        g.warnMods.Set("The game restarts with them on and comes back to this lobby. NOT NOW keeps you here as you are.");
        g.warnOn.label.Set("RESTART AND REJOIN");
        g.warnCancel.label.Set("NOT NOW");
        g.warnOpen = true; g.warn.Show(true);
        HSM_LOG("ui: asking about a restart with %s for this lobby", BackWords(ask.backPlan).c_str());
        if (!g.panelOpen) openPage();
        return;
    }
    if (!g.warnOpen && g.warn) if (const game::Lobby::SwitchAsk& ask = lobby.SwitchAskPending(); ask.lobby) {
        g.warnKind = State::WarnKind::Switch;
        g.warnTitle.Set(ask.rejoin ? "THIS LOBBY'S MODS CHANGED" : ask.name.empty() ? "JOINING THE LOBBY" : "JOINING " + ask.name);
        std::string body = "Host allows: " + ask.ruleText + ". These turn off until the game closes:";
        for (const std::string& line : SwitchSummary(ask.plan, 6)) body += "\n   " + line;
        g.warnBody.Set(body);
        g.warnMods.Set(ask.rejoin ? "The game restarts and comes back to this lobby. LEAVE LOBBY keeps everything on; the host removes players who no longer fit after a minute. Nothing is deleted."
                                  : "The game restarts and joins the lobby on its own. Your mods are back the next time you start the game. Nothing is deleted.");
        g.warnOn.label.Set(ask.rejoin ? "RESTART AND REJOIN" : "RESTART AND JOIN");
        g.warnCancel.label.Set(ask.rejoin ? "LEAVE LOBBY" : "CANCEL");
        g.warnOpen = true; g.warn.Show(true);
        HSM_LOG("ui: asking about a restart with %s for %s", SwitchWords(ask.plan).c_str(), ask.rejoin ? "this lobby's new rule" : "the lobby");
        if (!g.panelOpen) openPage();                  // an invite arrived on the bare main menu
        return;
    }
    if (openClicked && !g.panelOpen && !session.HelpImproveAsked() && g.ask) {
        g.askOpen = true; g.ask.Show(true);
        g.escDown = mp::KeyDown(VK_ESCAPE);
        HSM_LOG("ui: first MULTIPLAYER click: asking about help improve the mod (off until answered)");
        return;
    }
    if (openClicked) { if (g.panelOpen) ClosePage(); else openPage(); }
    if (!g.panelOpen) { SetLobbyTyping(false); g.reportTyping = false; return; }
    const LobbyScene& s = g.scene;
    bool acted = false;
    // A click on a dimmed control says why (`why`: empty, nothing to say); the clicked-and-allowed ones act.
    auto act = [&](bool clicked, bool allowed, const std::string& why = std::string()) {
        if (clicked && !allowed) Explain(why);
        bool go = clicked && allowed; acted = acted || go; return go;
    };
    const bool host = lobby.IsHost();
    auto why = [&](const std::string& own) { return DisabledWhy(host, own); };   // a member: "Only the host changes this."; the host: the control's own reason

    // The footer.
    // The bottom-left button: BACK TO MAIN MENU closes the page; BACK TO LOBBY (LIST) closes SETTINGS or the class page, over either.
    if (act(Clicked(g.close.card), !FooterFor(s.in, ViewOf(s)).back.empty())) {
        if (ViewOf(s) == PageView::List) { ClosePage(); return; }
        g.settingsShown = false; g.classShown = false; g.voice.StopBinding();
    }
    if (act(Clicked(g.settingsOpen.card), !g.settingsShown)) { g.settingsShown = true; g.classShown = false; }   // a second click does not close it: BACK does
    if (act(Clicked(g.classOpen.card), true)) {
        g.settingsShown = false; g.voice.StopBinding();
        if (YourClassGoes(s.in, ClassesTabShown(s)) == ClassGoes::Tab) { g.tab = kClassesTab; g.classShown = false; }   // the lobby's CLASSES tab
        else g.classShown = true;                                                                                         // no tab: the class page
    }
    if (act(Clicked(g.classChange.card), s.in && !s.classLine.empty())) g.tab = kClassesTab;
    g.reportOpen.card.button.Show(report::Enabled());
    if (act(Clicked(g.reportOpen.card), report::Enabled())) {
        g.reportOpenNow = !g.reportOpenNow;
        g.reportBox.Show(g.reportOpenNow);
        g.reportNote.Set("");
        if (g.reportOpenNow) g.reportText.Focus();
    }
    g.reportOpen.card.selected = g.reportOpenNow;
    g.reportOpen.label.Color(InkOn(g.reportOpen.card, style::kMenuText));
    if (g.reportOpenNow && act(Clicked(g.reportCancel.card), true)) { g.reportOpenNow = false; g.reportBox.Show(false); g.reportText.Clear(); }
    if (g.reportOpenNow && act(Clicked(g.reportSend.card), true)) {
        std::string text = g.reportText.Value();
        if (text.empty()) {
            g.reportNote.Set("Type what went wrong first.");
        } else {
            g.reportNote.Set(session.SubmitReport(text));
            g.reportText.Clear();
            g.reportOpenNow = false; g.reportBox.Show(false);
        }
    }
    g.reportTyping = g.reportOpenNow && g.reportText && g.reportText.Focused();
    if (const uint32_t seq = session.ReportStatusSeq(); seq != g.reportSeen) {
        g.reportSeen = seq;
        g.reportNote.Set(s.in ? std::string() : session.ReportStatus());
    }

    // SETTINGS: HELP IMPROVE THE MOD (on: the picked card and its ink, as Mark me ready) and voice.
    const bool help = session.HelpImprove();
    g.helpToggle.label.Set(help ? "HELP IMPROVE THE MOD: ON" : "HELP IMPROVE THE MOD: OFF");
    g.helpToggle.label.Color(help ? style::kMenuOnPicked : style::kMenuText);
    g.helpToggle.card.selected = help;
    if (act(Clicked(g.updateNow.card), update::CanInstall())) update::StartInstall();
    if (act(Clicked(g.helpToggle.card), g.settingsShown)) session.SetHelpImprove(!help);
    const bool hide = session.HideNames();
    g.hideToggle.label.Set(hide ? "HIDE PLAYER NAMES: ON" : "HIDE PLAYER NAMES: OFF");
    g.hideToggle.label.Color(hide ? style::kMenuOnPicked : style::kMenuText);
    g.hideToggle.card.selected = hide;
    if (act(Clicked(g.hideToggle.card), g.settingsShown)) session.SetHideNames(!hide);
    if (act(Clicked(g.myRegion.card), g.settingsShown)) session.SetRegionPick(NextRegion(session.RegionPick()));
    if (g.myRegionPlate.card) {                     // MY REGION: Auto and the six regions in a list
        const uint32_t owner = dropdown_owner::kMyRegion;
        if (act(Clicked(g.myRegionPlate.card), g.settingsShown))
            g.drops.Open(owner, "MY REGION", g.myRegionPlate.card.button, MyRegionChoices(session.RegionGuessed()), MyRegionChoiceAt(session.RegionPick()));
        size_t pick = 0;
        if (g.drops.TakePick(owner, pick) && g.settingsShown) session.SetRegionPick(MyRegionPicked(pick));
        PaintPlate(g.myRegionPlate, RegionPickLabel(session.RegionPick(), session.RegionGuessed()), g.drops.IsOpen(owner), true);
    }
    if (g.lookToggle.card || g.lookPlate.card) {
        auto setLook = [&](style::MenuLook look) {
            g.look = look;
            g.lookSaved = SaveUiStyle(g.look);
            HSM_LOG("ui: menu look set to %s for the next start%s", UiStyleName(g.look), g.lookSaved ? "" : " (HSMMod.ini could not be written)");
        };
        if (g.lookToggle.card && act(Clicked(g.lookToggle.card), g.settingsShown)) setLook(style::NextMenuLook(g.look));
        if (g.lookPlate.card) {                     // MENU LOOK: the three looks in a list
            const uint32_t owner = dropdown_owner::kMenuLook;
            if (act(Clicked(g.lookPlate.card), g.settingsShown))
                g.drops.Open(owner, "MENU LOOK", g.lookPlate.card.button, style::MenuLookChoices(), style::MenuLookChoiceAt(g.look));
            size_t pick = 0;
            if (g.drops.TakePick(owner, pick) && g.settingsShown) setLook(style::MenuLookPicked(pick));
            PaintPlate(g.lookPlate, style::MenuLookName(g.look), g.drops.IsOpen(owner), true);
        }
        g.lookToggle.label.Set(g.look == style::MenuLook::Classic ? "MENU LOOK: CLASSIC"
                               : g.look == style::MenuLook::Game ? "MENU LOOK: GAME ART" : "MENU LOOK: DARK");
        const bool pending = g.look != style::CurrentMenuLook();
        const std::string lookSays = !g.lookSaved ? "The menu look could not be saved, so it will not stick."
                                   : pending ? "Restart the game to see it." : "";   // no note until a look is picked
        g.lookNote.Set(lookSays);
        g.lookNote.Show(!lookSays.empty());
        g.lookNote.Color(!g.lookSaved || pending ? style::kMenuWarn : style::kMenuDim);
    }
    // MODS: the checkboxes for this machine's lobbies, while that tab is what's on screen.
    if (s.in && g.tab == kModsTab && !g.settingsShown) {
        const bool unrestricted = session.ClassesUnrestricted();
        // The Classes row steps onto Unrestricted through here. A guest sees the host's choice, and can't change it.
        const bool canSet = session.CanSetUnrestricted();
        g.classUnrestricted.label.Set(std::string(unrestricted ? "[X]" : "[  ]") + " UNRESTRICTED");
        g.classUnrestricted.label.Color(canSet ? style::kMenuText : style::kMenuDim);
        const bool asked = std::exchange(g.unrestrictedAsked, false);
        if (act(asked, canSet)) {
            if (unrestricted) session.SetClassesUnrestricted(false);
            else if (g.warn) {   // it says what this is first. HandleWarn takes the answer
                g.warnKind = State::WarnKind::Unrestricted;
                g.warnOn.label.Set("TURN IT ON"); g.warnCancel.label.Set("CANCEL");
                g.warnTitle.Set(skin::Title("UNRESTRICTED CLASSES"));
                g.warnBody.Set(kUnrestrictedWarning);
                g.warnMods.Set("HS Enhancer stays blocked unless the rule above allows it. The Half Wolverine options still need Half Wolverine.");
                g.warnOpen = true; g.warn.Show(true);
            }
        }
        Paint(g.classUnrestricted.card);
    }
    if (ClassesOpen(s) && g.className) {
        const std::string picked = session.ClassName();
        const bool unrestricted = session.ClassesUnrestricted(), wolverine = session.HasWolverine();
        g.classFrame++;
        // The mods that are on decide what can fight, so it opens on those.
        if ((g.classTab == kClassTabHacker && !unrestricted) || !ClassTabUsable(g.classTab, wolverine)) g.classTab = ClassHomeTab(wolverine);
        const bool classesOff = session.ClassesOff();
        // The dropdown: a mod's entry is greyed out until the mod is on, and the hacker entry stays hidden outside Unrestricted.
        g.classTabButton.label.Set(std::string(ClassTabs()[size_t(g.classTab)].title) + (g.classTabOpen ? "  ^" : "  v"));
        if (act(Clicked(g.classTabButton.card), true)) g.classTabOpen = !g.classTabOpen;
        Paint(g.classTabButton.card);
        if (g.classTabList) g.classTabList.Show(g.classTabOpen);
        for (size_t i = 0; i < g.classTabs.size() && i < ClassTabs().size(); ++i) {
            Flat& f = g.classTabs[i];
            const ClassTab& t = ClassTabs()[i];
            const bool shown = !t.unrestrictedOnly || unrestricted, grey = !ClassTabUsable(int(i), wolverine);
            f.card.button.Show(shown);
            if (f.label) {
                f.label.Color(grey ? style::kMenuDim : int(i) == g.classTab ? style::kMenuOnPicked : style::kMenuText);
                f.label.Set(std::string(t.title) + (!grey ? "" : wolverine ? "  (Half Wolverine is on)" : "  (needs Half Wolverine)"));
            }
            f.card.selected = int(i) == g.classTab;
            if (g.classTabOpen && shown && Clicked(f.card)) {
                if (grey) g.classTabNote.Set(ClassTabRefusal(int(i), wolverine));
                else { g.classTab = int(i); g.classTabOpen = false; g.classTabNote.Set(""); acted = true; }
            }
            Paint(f.card);
        }
        const std::vector<std::string> tabPicks = ClassTabPicks(g.classTab);
        if (g.classWhere) g.classWhere.Set(unrestricted ? "UNRESTRICTED: no class balance. Expect glitches, and possibly crashes, for everyone in the lobby."
                                                        : "Used in lobbies with Classes on.");
        if (g.classNew.card) {
            g.classNew.card.button.Show(g.classTab == kClassTabCustom);
            if (act(Clicked(g.classNew.card), g.classTab == kClassTabCustom)) {
                if (session.NewClass()) { g.classNameBox.Clear(); g.classNameBox.Focus(); g.classNameFor = session.ClassName(); g.classNameNote = "Name your class, then change its stats."; }
                else g.classTabNote.Set("All your class slots are used. Delete one first.");
            }
            Paint(g.classNew.card);
        }
        g.className.Set(classesOff ? std::string(kHseClassName) : session.ClassLabel(picked));
        // The player's own classes can be named. Enter saves too, as in the password box, which may already have let go of the keyboard.
        const bool nameable = CustomClassSlot(picked) >= 0;
        if (g.classNameRow) {
            g.classNameRow.Show(nameable);
            // Two clicks, because it can't be undone.
            if (g.classDelete.card) {
                if (act(Clicked(g.classDelete.card), nameable)) {
                    if (g.classDeleteArmed) { session.DeleteClass(); g.classDeleteArmed = false; }
                    else g.classDeleteArmed = true;
                }
                if (g.classNameFor != picked) g.classDeleteArmed = false;
                g.classDelete.label.Set(g.classDeleteArmed ? "SURE?" : "DELETE");
                Paint(g.classDelete.card);
            }
            if (g.classNameFor != picked) { g.classNameFor = picked; g.classNameNote.clear(); g.classNameBox.Set(session.ClassOwnName()); }
            const bool focused = g.classNameBox.Focused();
            const bool enter = mp::KeyDown(VK_RETURN), enterEdge = enter && !g.classNameEnterDown;
            g.classNameEnterDown = enter;
            const bool save = Clicked(g.classNameSave.card) || (enterEdge && (focused || g.classNameFocused));
            g.classNameFocused = focused;
            Paint(g.classNameSave.card);
            if (act(save, nameable)) {
                const std::string typed = g.classNameBox.Value();
                g.classNameNote = session.NameClass(typed);
                if (!g.classNameNote.empty()) g.classNameBox.Focus();   // left as typed, to fix; nothing was saved
            }
        }
        g.classNote.Set(g.classNameNote.empty() ? session.ClassNote() : g.classNameNote);
        for (int i = 0; i < int(g.classPicks.size()); ++i) {
            Flat& f = g.classPicks[size_t(i)];
            const std::string name = ClassPickName(i);
            const bool inTab = std::find(tabPicks.begin(), tabPicks.end(), name) != tabPicks.end() && (CustomClassSlot(name) < 0 || session.ClassSlotUsed(name));
            f.card.button.Show(inTab);
            if (!inTab) continue;
            const bool pickable = !classesOff && ClassPickable(name, wolverine);
            if (f.label) { f.label.Set(session.ClassLabel(name)); f.label.Color(name == picked ? style::kMenuOnPicked : pickable ? style::kMenuText : style::kMenuDim); }
            f.card.selected = name == picked;
            if (act(Clicked(f.card), pickable)) session.SetClassName(name);
            Paint(f.card);
        }
        ClassSheet sheet = session.ClassValue();
        const ClassBars bars = ClassBarsFor(sheet);
        if (g.classBudget) {
            const ClassBudgetView budget = ClassBudget(sheet, unrestricted, session.ClassPowerLimit());
            g.classBudget.Set(budget.text);
            g.classBudget.Color(budget.over ? style::kMenuAccentInk : style::kMenuText);
        }
        const bool glitch = unrestricted;
        g.classAttack.Percent(glitch ? GlitchedBar(bars.attack, g.classFrame) : bars.attack);
        g.classSpeed.Percent(glitch ? GlitchedBar(bars.speed, g.classFrame + 5) : bars.speed);
        g.classTough.Percent(glitch ? GlitchedBar(bars.toughness, g.classFrame + 11) : bars.toughness);
        for (Bar* bar : {&g.classAttack, &g.classSpeed, &g.classTough}) bar->FillColor(glitch ? (g.classFrame / 3 % 7 == 0 ? style::kMenuDim : style::kBloodInk) : style::kMenuAccentInk);
        // The player's own classes can be changed here. The built-ins are the same for everyone, so theirs are only shown.
        const bool own = CustomClassSlot(picked) >= 0 && !classesOff;
        for (size_t i = 0; i < g.classFields.size(); ++i) {
            ClassRow& v = g.classFields[i];
            if (!v.box) continue;
            const bool rowShown = ClassFieldShown(int(i), g.classTab);
            v.box.Show(rowShown);
            if (!rowShown) { if (g.classFieldSel == int(i)) g.classFieldSel = 7; continue; }
            const ClassFieldView f = ClassField(sheet, int(i));
            v.value.Set(f.value);
            v.name.card.selected = int(i) == g.classFieldSel;
            if (act(Clicked(v.name.card), true)) g.classFieldSel = int(i);
            int dir = Clicked(v.prev.card) ? -1 : 0;
            if (Clicked(v.next.card)) dir = 1;
            if (dir != 0) g.classFieldSel = int(i);
            const bool locked = ClassFieldLocked(sheet, int(i), wolverine);
            if (v.name.label) v.name.label.Color(locked ? style::kMenuDim : InkOn(v.name.card, style::kMenuText));
            if (act(dir != 0, own && !locked)) { ClassFieldStep(sheet, int(i), dir, unrestricted); session.SetCustomClass(sheet); }
            v.prev.card.button.Show(own); v.next.card.button.Show(own);
            Paint(v.name.card); Paint(v.prev.card); Paint(v.next.card);
        }
        g.classInfoName.Set(ClassField(sheet, g.classFieldSel).label);
        g.classInfo.Set(ClassFieldInfo(g.classFieldSel));
    }
    if (g.classOpen.card) Paint(g.classOpen.card);
    if (g.classChange.card) Paint(g.classChange.card);
    if (g.settingsShown) g.voice.Tick(session);
    HandleChat(session, s.in && !g.settingsShown);
    HandlePassword(session, s.hostPassword && !g.settingsShown);
    HandleNameRow(g.lobbyName, s.hostPassword && !g.settingsShown, session.LobbyName(),
                  "",                               // no note: the box says what to type
                  [&](const std::string& typed) { return session.SetLobbyName(typed); });
    HandleNameRow(g.nickname, g.settingsShown, session.Nickname(),
                  "The host still sees your Steam name.",
                  [&](const std::string& typed) { return session.SetNickname(typed); });
    HandleDamage(lobby, s, acted);               // after the two boxes: its own text box keeps the hotkeys off too
    HandleGather(lobby, s, acted);               // the same
    HandleWeights(lobby, s, acted);
    if (ClassesOpen(s) && g.classNameBox && g.classNameBox.Focused()) SetLobbyTyping(true);   // and the class name's, after all of them

    // Not in a lobby: hosting, the browser and its filters.
    if (act(Clicked(g.hostFriends.card), s.idle)) lobby.Host(steam::LobbyType::FriendsOnly);
    if (act(Clicked(g.hostPublic.card), s.idle)) lobby.Host(steam::LobbyType::Public);
    if (act(Clicked(g.refresh.card), s.idle)) lobby.RefreshList();
    auto setFilter = [&](const BrowserFilter& next) {
        const LobbyDistance was = DistanceFor(g.filter);
        g.filter = next;
        if (DistanceFor(g.filter) != was) { lobby.SetListDistance(DistanceFor(g.filter)); lobby.RefreshList(); }   // Steam looks farther (or not): ask again
    };
    for (size_t i = 0; i < g.chips.size(); ++i) {
        const bool listed = g.drops && i < s.filters.size() && s.filters[i].choice;      // the region and the mode open a list; the others are toggles
        const uint32_t owner = dropdown_owner::kFilterChip + uint32_t(i);
        if (!act(Clicked(g.chips[i].card), s.idle && i < s.filters.size())) {
            size_t pick = 0;
            if (listed && g.drops.TakePick(owner, pick) && s.idle) setFilter(WithFilterChoice(g.filter, i, pick));
            continue;
        }
        if (listed) g.drops.Open(owner, i == kRegionChip ? "region filter" : "mode filter", g.chips[i].card.button, FilterChoices(g.filter, i), FilterChoiceAt(g.filter, i));
        else setFilter(ToggleFilter(g.filter, i));
    }
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (act(Clicked(g.rows[i].card), i < s.browser.size() && s.browser[i].joinable && g.rowLobbies[i])) {
            if (s.browser[i].locked) lobby.AskPassword(g.rowLobbies[i]);   // the question opens next frame; JOIN there joins
            else lobby.Join(g.rowLobbies[i]);
        }

    // In a lobby: the players' buttons, the tabs, and what each tab offers.
    if (act(Clicked(g.invite.card), s.in)) lobby.Invite();
    if (act(Clicked(g.readyUp.card), s.ready)) lobby.SetReady(!s.readyOn);
    if (act(Clicked(g.balance.card), s.balance)) lobby.ClearOverrides();
    if (act(Clicked(g.shuffle.card), s.shuffle)) lobby.ShuffleTeamsNow();
    if (act(Clicked(g.leave.card), s.in)) lobby.Leave();
    for (size_t i = 0; i < kTabCount; ++i) if (act(Clicked(g.tabs[i].card), s.in && TabShown(i, ClassesTabShown(s)))) { g.tab = i; g.classShown = false; }
    for (size_t sct = 0; sct < kSectionCount; ++sct)   // RULES: the sub-tabs
        if (act(Clicked(g.subTabs[sct].card), s.in && g.tab == kRulesTab && !g.settingsShown && !g.classShown)) g.rulesTab = LobbySection(sct);

    MatchConfig c = lobby.config();
    int teams = PicksTeams(c.format) ? TeamCount(c.format, c.teams) : 0;
    for (size_t i = 0; i < g.members.size(); ++i) {
        if (!act(Clicked(g.members[i].card), i < s.roster.size() && s.roster[i].movable && g.memberIds[i] && teams)) continue;
        for (const game::LobbyMember& m : lobby.members())
            if (m.id == g.memberIds[i]) lobby.SetOverride(m.id, NextTeamPick(int8_t(m.hostOverride >= 0 ? m.hostOverride : m.team), teams));
    }
    for (size_t i = 0; i < g.modes.size(); ++i) {
        if (!act(Clicked(g.modes[i].card), g.tab == kModeTab && !g.settingsShown && !g.classShown && i < s.modes.size() && s.modes[i].enabled && s.modes[i].shown, why(i < s.modes.size() ? s.modes[i].hint : std::string()))) continue;
        if (s.modes[i].progression) { lobby.SetProgression(true); continue; }
        if (lobby.Progression()) lobby.SetProgression(false);
        MatchConfig next = SelectMode(c, s.modes[i].format, s.modes[i].brawl, s.modes[i].crypts, s.modes[i].blossfechten, s.modes[i].graveRobbers,
                                      s.modes[i].coopCrypts);
        if (next.format != c.format || next.gear != c.gear || next.scenario != c.scenario || next.graveRobbers != c.graveRobbers || next.coopCrypts != c.coopCrypts) {
            lobby.SetConfig(next); lobby.ClearOverrides(); c = next;   // the Crypts modes play on the Crypts map by themselves (EffectiveMap)
        }
    }
    for (size_t i = 0; i < g.steppers.size(); ++i) {
        StepperView& v = g.steppers[i];
        if (!v.box) continue;
        int dir = Clicked(v.prev.card) ? -1 : 0;
        if (Clicked(v.next.card)) dir = 1;
        const SettingRow* row = RowFor(s, i);
        if (v.plate.card && row) {                  // a dropdown row: the plate opens its list, and a pick sets that option (to a member it is a plain value)
            const uint32_t owner = dropdown_owner::kRulesRow + uint32_t(row->id);
            const bool allowed = row->enabled && RulesShown(SectionOf(row->id));
            PaintPlate(v.plate, row->value, g.drops.IsOpen(owner), row->enabled);
            if (act(Clicked(v.plate.card), allowed, why(std::string()))) g.drops.Open(owner, row->label, v.plate.card.button, row->choices, row->choice);
            size_t pick = 0;
            if (!g.drops.TakePick(owner, pick) || !allowed) continue;
            if (row->id == LobbyRow::MyTeam) lobby.SetTeamPick(lobby_detail::MyTeamPick(pick));
            else if (row->id == LobbyRow::FightMap) {   // the lobby's own pick, not a config field
                const std::vector<size_t> maps = lobby_detail::FightMaps(c.format);
                if (pick < maps.size()) lobby.SetFightPick(MapAt(maps[pick]).path);
            } else { c = lobby_detail::WithChoice(c, row->id, pick); lobby.SetConfig(c); }
            HSM_LOG("lobby: %s picked from the list: %s", row->label.c_str(), pick < row->choices.size() ? row->choices[pick].label.c_str() : "?");
            acted = true;
            continue;
        }
        if (!act(dir != 0, row && row->enabled && RulesShown(SectionOf(row->id)), why(std::string()))) continue;
        if (row->id == LobbyRow::Players) {         // the lobby's own limit, not a MatchConfig field
            lobby.SetMaxPlayers(StepPlayerLimit(lobby.MaxPlayers(), lobby.PlayerCount(), dir));
        } else if (row->id == LobbyRow::MyTeam) {
            int mine = kNoTeam;
            for (const game::LobbyMember& m : lobby.members()) if (m.id == steam::MySteamId()) mine = m.team;
            lobby.SetTeamPick(StepTeamPick(int8_t(mine), teams, dir));
        } else {
            c = StepSetting(c, row->id, dir, game::VanillaDamageRange());
            lobby.SetConfig(c);
        }
    }
    {   // the Weapons row's chips: Any clears the ticks, a type toggles (match/hsm_match.h ToggleWeaponType)
        const SettingRow* w = RowFor(s, size_t(LobbyRow::Weapons));
        for (size_t k = 0; k < g.weaponChips.size(); ++k) {
            if (!act(Clicked(g.weaponChips[k].card), w && w->enabled && RulesShown(SectionOf(LobbyRow::Weapons)) && k < w->chips.size(), why(std::string()))) continue;
            c.weapons = ToggleWeaponType(c.weapons, uint8_t(k));
            lobby.SetConfig(c);
            HSM_LOG("lobby: weapons row: %s", WeaponMaskName(c.weapons).c_str());
        }
    }
    {   // the tier chips, under Gear and under the Crypts card's Starting tier: a click sets the tier (match/hsm_lobby_scene.h WithTier)
        struct TierRow { LobbyRow id; std::array<Flat, kTierChipCount>* chips; };
        for (const TierRow& t : {TierRow{LobbyRow::Gear, &g.gearTierChips}, TierRow{LobbyRow::Tier, &g.startTierChips}}) {
            const SettingRow* row = RowFor(s, size_t(t.id));
            for (size_t k = 0; k < t.chips->size(); ++k) {
                if (!act(Clicked((*t.chips)[k].card), row && row->enabled && RulesShown(SectionOf(t.id)) && k < row->tierChips.size(), why(std::string()))) continue;
                c = WithTier(c, k);
                lobby.SetConfig(c);
                HSM_LOG("lobby: tier chip: %s", TierName(c.tier));
            }
        }
    }
    for (size_t i = 0; i < kModRuleRows; ++i) {   // MODS: the host's rule steppers; written to HSMMod.ini [mod]
        StepperView& v = g.modRules[i];
        if (!v.box) continue;
        int dir = Clicked(v.prev.card) ? -1 : 0;
        if (Clicked(v.next.card)) dir = 1;
        const ModRuleView& row = g.modsScene.rules[i];
        if (v.plate.card) {                         // Mods allowed and Classes: the plate opens the list, a pick sets one
            const uint32_t owner = dropdown_owner::kModRule + uint32_t(i);
            const bool allowed = g.tab == kModsTab && s.in && row.enabled && host;
            PaintPlate(v.plate, row.value, g.drops.IsOpen(owner), row.enabled);
            if (act(Clicked(v.plate.card), allowed, why(std::string()))) g.drops.Open(owner, row.label, v.plate.card.button, row.choices, row.choice);
            size_t pick = 0;
            if (!g.drops.TakePick(owner, pick) || !allowed || pick == row.choice) continue;
            const ModChoice now = game::modrules::Chosen();
            if (row.id == ModRuleRow::Classes) {
                if (ClassesPickAsksFirst(now, pick) && g.warn) g.unrestrictedAsked = true;   // Unrestricted says what it is first; HandleWarn takes the answer
                else game::modrules::SetChoice(WithClassesMode(now, pick));
            } else game::modrules::SetChoice(WithModPreset(now, pick));
            continue;
        }
        if (!act(dir != 0, g.tab == kModsTab && s.in && row.enabled && host, why(std::string()))) continue;
        const ModChoice next = StepModRule(game::modrules::Chosen(), row.id, dir);
        if (row.id == ModRuleRow::Classes && next.classes == ClassesMode::Unrestricted && g.warn) { g.unrestrictedAsked = true; continue; }   // it says what this is first
        if (row.id != ModRuleRow::Hse) { game::modrules::SetChoice(next); continue; }
        // Allowing HS Enhancer where classes hold is the true unrestricted: the warning says so first, HandleWarn takes the answer.
        if (!next.blockHse && ClassesOn(next.classes) && g.warn && session.CanSetUnrestricted()) {
            g.warnKind = State::WarnKind::Hse;
            g.warnOn.label.Set("TURN IT ON"); g.warnCancel.label.Set("CANCEL");
            g.warnTitle.Set(skin::Title("ALLOW HS ENHANCER"));
            g.warnBody.Set("The true unrestricted. Players with HS Enhancer can use all of it in the lobbies you host: any value, and spawning things. Classes are off for them, and no limits hold them. "
                           "Everyone else plays Unrestricted Classes. Expect crashes, and only do this with people you trust.");
            g.warnMods.Set("This also turns on Unrestricted. Everyone who joins is told, and the K card shows what each player reports running.");
            g.warnOpen = true; g.warn.Show(true);
        } else session.SetAllowHse(!next.blockHse);
    }
    for (size_t i = 0; i < g.maps.size(); ++i)
        if (act(Clicked(g.maps[i].card), g.tab == kMapTab && !g.settingsShown && !g.classShown && i < s.maps.size() && s.maps[i].enabled, why(i < s.maps.size() ? s.maps[i].reason : std::string())))
        {
            lobby.SetMapPick(s.maps[i].modded ? s.maps[i].path : std::string(MapAt(s.maps[i].index).path));
            // The Crypts map enters the Crypts modes; another map while in them leaves (match/hsm_lobby_scene.h MapPicked)
            const MatchConfig next = MapPicked(c, !s.maps[i].modded && MapAt(s.maps[i].index).crypts);
            if (next.format != c.format || next.gear != c.gear || next.graveRobbers != c.graveRobbers || next.coopCrypts != c.coopCrypts) { lobby.SetConfig(next); lobby.ClearOverrides(); c = next; }
        }
    if (act(Clicked(g.start), s.start, s.startWhy)) {
        bool progression = false;                        // the Progression card is not the last card any more: look for it
        for (const ModeCard& mc : s.modes) progression = progression || (mc.progression && mc.selected);
        if (progression) session.StartProgression();
        else {
            const MapCard* custom = nullptr;                 // a modded map is the pick: it is open by its package path
            for (const MapCard& mc : s.maps) if (mc.modded && mc.selected) custom = &mc;
            size_t m = EffectiveMap(c.format, c.gear, lobby.MapPick(), IsBlossfechten(c));
            if (custom) session.StartOnMap(custom->path);
            else if (m < MapCount()) session.StartOnMap(MapAt(m).path);
        }
    }

    // Paint after the polls (a card's Hovered() is the one its Clicked() just saw).
    for (Flat& f : g.weaponChips) Paint(f.card);
    for (Flat* f : {&g.refresh, &g.invite, &g.readyUp, &g.balance, &g.shuffle, &g.leave, &g.close, &g.settingsOpen, &g.reportOpen,
                    &g.reportSend, &g.reportCancel, &g.chatSend, &g.helpToggle, &g.lookToggle, &g.updateNow, &g.hideToggle, &g.myRegion, &g.myRegionPlate, &g.lookPlate}) Paint(f->card);
    for (Flat& f : g.subTabs) Paint(f.card);
    for (Flat& f : g.gearTierChips) Paint(f.card);
    for (Flat& f : g.startTierChips) Paint(f.card);
    for (Flat& f : g.chips) Paint(f.card);
    for (HostView* h : {&g.hostFriends, &g.hostPublic}) Paint(h->card);
    for (BrowserView& v : g.rows) Paint(v.card);
    for (MemberView& v : g.members) Paint(v.card);
    for (ModeView& v : g.modes) Paint(v.card);
    for (StepperView& v : g.steppers) { Paint(v.prev.card); Paint(v.next.card); Paint(v.plate.card); }
    for (StepperView& v : g.modRules) { Paint(v.prev.card); Paint(v.next.card); Paint(v.plate.card); }
    Paint(g.damage.number.card);
    Paint(g.gather.number.card);
    for (MapView& v : g.maps) Paint(v.card);
    PaintTabs();
    PaintStart();
    if (acted) g.nextText = 0;                  // redraw now, not at the next quarter second
}
}  // namespace

void Tick(Session& session, UObject* worldContext) {
    if (!session.steamMode()) return;
    game::modmaps::Tick();                       // the modded-map scan (worker results, asset registry); cheap when idle
    game::modrules::Tick();                      // the mod report (UE4SS mods, proxies, Enhancer) for the mod rules
    if (worldContext != g.worldContext) { Reset(); g.worldContext = worldContext; }
    ULONGLONG now = GetTickCount64();
    const bool alive = StillAlive(g.menu, g.menuIndex, g.menuClass);
    if (!alive && g.menu) { UObject* wc = g.worldContext; Reset(); g.worldContext = wc; }
    if (!alive || !InViewport(g.menu)) {
        // The settings screens remove the menu and their Back button makes a new one (UI_GameSettings.cpp:123, 345),
        // while the removed one lives on until garbage collection: follow whichever menu is on screen (PickMenu).
        if (now < g.nextScan) return;
        g.nextScan = now + 500;
        std::vector<MenuCandidate> live;
        for (const char* name : {"UI_Startup_Menu_C", "UI_MainMenuSNF_C"})
            for (UObject* o : FindInstances(name)) live.push_back({o, InViewport(o)});
        UObject* pick = static_cast<UObject*>(const_cast<void*>(PickMenu(g.menu, false, live)));
        if (!pick) return;
        if (pick != g.menu) {
            UObject* wc = g.worldContext; Reset(); g.worldContext = wc;
            g.menu = pick; g.menuIndex = ObjectIndex(pick); g.menuClass = ClassOf(pick);
            HSM_LOG("ui: main menu %s is on screen; the Multiplayer button follows it", ObjectName(pick).c_str());
        }
    }
    if (!g.built) Build();
    NoticesMenuTick(g.menu, g.panelOpen);
    if (!g.openButton) return;
    // Turned away by a host: the page opens on its own with the reason in large type, so it is not missed.
    if (std::string why = session.TakeNotice(); !why.empty() && g.notice) {
        g.notice.Set(why); g.notice.Show(true);
        g.panelOpen = true; g.panel.Show(true); g.nextText = 0;
    }
    // Back from a match with the lobby kept (the host's BACK TO LOBBY): the page opens, on the lobby.
    if (session.TakeLobbyPage()) { g.panelOpen = true; g.panel.Show(true); g.nextText = 0; }
    g.drops.Tick(DropContext());                 // an open list takes its clicks and Esc before the page reads its own
    if (!HandleWarn(session)) HandleClicks(session);
    if (now >= g.nextText) {
        g.nextText = now + 250;
        if (g.panelOpen || session.lobby().InLobby()) FixClassPick(session);
        UpdateOpenLabel(session);
        if (g.panelOpen) UpdatePanel(session);
    }
}

bool MenuTyping() { return g.panelOpen && g.reportTyping; }

// Fight levels: no main menu belongs on screen. A client that joined with the multiplayer page open kept the page over
// the match (test 2026-10-27), so close our page and take any main menu still in the viewport off it.
bool PanelOpen() { return g.panelOpen && g.menu && StillAlive(g.menu, g.menuIndex, g.menuClass) && InViewport(g.menu); }

void LeaveMenus() {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;
    SetLobbyTyping(false);
    g.reportTyping = false;
    if (g.panelOpen && StillAlive(g.menu, g.menuIndex, g.menuClass)) { g.panelOpen = false; g.panel.Show(false); g.drops.Close("page"); }
    for (const char* name : {"UI_Startup_Menu_C", "UI_MainMenuSNF_C"})
        for (UObject* o : FindInstances(name))
            if (InViewport(o)) {
                Call(o, "RemoveFromParent").invoke();
                HSM_LOG("ui: %s was still on screen in a fight level; removed", ObjectName(o).c_str());
            }
}

}  // namespace hsm::ui

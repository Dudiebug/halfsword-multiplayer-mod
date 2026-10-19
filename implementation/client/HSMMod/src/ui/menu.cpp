#include "ui/menu.h"
#include "util/hsm_names.h"
#include "gameplay/session.h"
#include "log.h"
#include "match/hsm_hud_text.h"
#include "match/hsm_map_list.h"
#include "match/hsm_maps.h"
#include "match/hsm_menu_pick.h"
#include <windows.h>
#include <string>
#include <vector>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

struct Vec2 { double x = 0, y = 0; };
struct Anchors { Vec2 min, max; };
struct Margin { float left = 0, top = 0, right = 0, bottom = 0; };
struct LinearColor { float r = 1, g = 1, b = 1, a = 1; };
struct ChildSize { float value = 1; uint8_t rule = 0; uint8_t pad[3] = {}; };   // FSlateChildSize; rule 0 Automatic, 1 Fill
static_assert(sizeof(ChildSize) == 8, "FSlateChildSize");
enum HAlign : uint8_t { kHAlignFill = 0, kHAlignLeft = 1 };
enum Visibility : uint8_t { kVisible = 0, kCollapsed = 1, kHidden = 2, kSelfHitTestInvisible = 4 };

// A clickable text button built from UMG widgets.
struct Button {
    UObject* button = nullptr; UObject* label = nullptr; bool wasPressed = false;
    explicit operator bool() const { return button != nullptr; }
};

struct State {
    UObject* worldContext = nullptr;
    UClass* menuClass = nullptr;
    UObject* menu = nullptr;
    int32_t menuIndex = -1;
    ULONGLONG nextScan = 0, nextText = 0;
    bool built = false, panelOpen = false;
    Button openButton;
    UObject* panel = nullptr;                // the full-screen multiplayer page (a Border over the whole menu)
    UObject *title = nullptr, *status = nullptr, *idleHint = nullptr, *playersTitle = nullptr;
    UObject *matchColumn = nullptr, *mapColumn = nullptr, *hostOnly = nullptr, *mapHint = nullptr;
    Button hostFriends, hostPublic, invite, leave, find, close;
    Button mode, limit, timeLimit, friendlyFire, myTeam, balance;   // match settings (host edits; members see them)
    Button gear, tier, start;
    std::vector<Button> mapButtons;                                // one per catalog map (kMaps order)
    std::vector<Button> memberRows;                                // host clicks a member to move them to the next team
    std::vector<steam::SteamId> memberIds;
    std::vector<Button> rows;               // lobby browser entries
    std::vector<uint64_t> rowLobbies;
    std::vector<uint8_t> fontBytes;          // copied from the menu's own text so ours matches
} g;

UClass* Cls(const char* name) { return FindClass(name); }
UObject* WidgetTree(UObject* userWidget) { UObject** t = Field<UObject*>(userWidget, "WidgetTree"); return t ? *t : nullptr; }

bool StillAlive(UObject* o, int32_t index, UClass* cls) { return o && ObjectAt(index) == o && ClassOf(o) == cls; }

UObject* Spawn(const char* cls) { return SpawnObject(Cls(cls), WidgetTree(g.menu)); }

void SetText(UObject* textBlock, const std::string& s) {
    if (!textBlock) return;
    FText t = MakeText(s);
    Call(textBlock, "SetText").set("InText", t).invoke();
}

void StyleText(UObject* textBlock) {
    if (!textBlock || g.fontBytes.empty()) return;
    Call c(textBlock, "SetFont");
    UFunction* fn = c.function();
    if (!fn) return;
    FProperty* p = FindProperty(fn, "InFontInfo");
    if (p && size_t(PropertySize(p)) == g.fontBytes.size()) {
        std::vector<uint8_t> buf(size_t(ParamsSize(fn)) + 16, 0);
        std::memcpy(buf.data() + PropertyOffset(p), g.fontBytes.data(), g.fontBytes.size());   // engine copies (add-refs) it
        ProcessEvent(textBlock, fn, buf.data());
    }
}

void SetVisible(UObject* w, bool visible) { if (w) Call(w, "SetVisibility").set<uint8_t>("InVisibility", visible ? kVisible : kCollapsed).invoke(); }
// Hidden but still taking its space (the page's columns stay where they are when a lobby is joined or left).
void SetShown(UObject* w, bool shown) { if (w) Call(w, "SetVisibility").set<uint8_t>("InVisibility", shown ? kVisible : kHidden).invoke(); }

Button MakeButton(const std::string& text, UObject* styleFrom) {
    Button b;
    b.button = Spawn("Button");
    b.label = Spawn("TextBlock");
    if (!b.button || !b.label) { HSM_LOG("ui: widget spawn failed"); return Button{}; }
    if (styleFrom) if (void* style = FieldPtr(styleFrom, "WidgetStyle")) {
        Call c(b.button, "SetStyle");
        if (FProperty* p = c.function() ? FindProperty(c.function(), "InStyle") : nullptr) {
            std::vector<uint8_t> buf(size_t(ParamsSize(c.function())) + 16, 0);
            std::memcpy(buf.data() + PropertyOffset(p), style, size_t(PropertySize(p)));
            ProcessEvent(b.button, c.function(), buf.data());
        }
    }
    Call(b.button, "SetContent").set<UObject*>("Content", b.label).invoke();
    StyleText(b.label);
    SetText(b.label, text);
    return b;
}

bool Clicked(Button& b) {
    if (!b) return false;
    Call p(b.button, "IsPressed"); p.invoke();
    bool pressed = p.get<bool>("ReturnValue");
    Call h(b.button, "IsHovered"); h.invoke();
    bool hovered = h.get<bool>("ReturnValue");
    bool clicked = b.wasPressed && !pressed && hovered;
    b.wasPressed = pressed;
    return clicked;
}

UObject* AddToVerticalBox(UObject* box, UObject* child, float padTop) {
    Call c(box, "AddChildToVerticalBox"); c.set<UObject*>("Content", child).invoke();
    UObject* slot = c.get<UObject*>("ReturnValue");
    if (slot) { Margin m{0, padTop, 0, 0}; Call(slot, "SetPadding").set("InPadding", m).invoke(); }
    return slot;
}

// A vertical box child that takes the space left over (the page body, the map list).
void FillVerticalBox(UObject* box, UObject* child, float padTop) {
    if (UObject* slot = AddToVerticalBox(box, child, padTop)) Call(slot, "SetSize").set("InSize", ChildSize{1, 1}).invoke();
}

// fill > 0: a share of the row; 0: the child's own width.
void AddToHorizontalBox(UObject* box, UObject* child, float fill, float padRight) {
    Call c(box, "AddChildToHorizontalBox"); c.set<UObject*>("Content", child).invoke();
    UObject* slot = c.get<UObject*>("ReturnValue");
    if (!slot) return;
    Call(slot, "SetSize").set("InSize", fill > 0 ? ChildSize{fill, 1} : ChildSize{1, 0}).invoke();
    Margin m{0, 0, padRight, 0};
    Call(slot, "SetPadding").set("InPadding", m).invoke();
}

UObject* Label(UObject* box, const std::string& text, float padTop) {
    UObject* t = Spawn("TextBlock");
    if (!t) return nullptr;
    StyleText(t);
    SetText(t, text);
    AddToVerticalBox(box, t, padTop);
    return t;
}

void AddButton(UObject* box, Button& b, float padTop) { if (b) AddToVerticalBox(box, b.button, padTop); }

// Insert the Multiplayer button right after `anchor`, in whatever container the menu uses.
bool InjectOpenButton(UObject* anchor, UObject* style) {
    UObject* freeMode = anchor;
    Call gp(freeMode, "GetParent"); gp.invoke();
    UObject* parent = gp.get<UObject*>("ReturnValue");
    if (!parent) { HSM_LOG("ui: menu anchor button has no parent panel"); return false; }
    g.openButton = MakeButton("MULTIPLAYER", style);
    if (!g.openButton) return false;
    std::string pcls = ObjectName(reinterpret_cast<UObject*>(ClassOf(parent)));
    if (pcls == "CanvasPanel") {
        Call add(parent, "AddChildToCanvas"); add.set<UObject*>("Content", g.openButton.button).invoke();
        UObject* slot = add.get<UObject*>("ReturnValue");
        UObject** freeSlotPtr = Field<UObject*>(freeMode, "Slot");
        if (slot && freeSlotPtr && *freeSlotPtr) {
            UObject* fs = *freeSlotPtr;
            Call pos(fs, "GetPosition"); pos.invoke(); Vec2 p = pos.get<Vec2>("ReturnValue");
            Call size(fs, "GetSize"); size.invoke(); Vec2 s = size.get<Vec2>("ReturnValue");
            Call anc(fs, "GetAnchors"); anc.invoke(); Anchors a = anc.get<Anchors>("ReturnValue");
            Call al(fs, "GetAlignment"); al.invoke(); Vec2 alignment = al.get<Vec2>("ReturnValue");
            Call(slot, "SetAnchors").set("InAnchors", a).invoke();
            Call(slot, "SetAlignment").set("InAlignment", alignment).invoke();
            Call(slot, "SetSize").set("InSize", s).invoke();
            Call(slot, "SetPosition").set("InPosition", Vec2{p.x, p.y + s.y * 1.15}).invoke();
        }
    } else {
        // UPanelWidget::InsertChildAt is C++-only in this engine build (tools/check_reflection.py); AddChild is the
        // reflected way in, so the button goes to the end of the box rather than reshuffling the stock layout.
        Call add(parent, "AddChild"); add.set<UObject*>("Content", g.openButton.button).invoke();
        if (!add.get<UObject*>("ReturnValue")) { HSM_LOG("ui: %s refused the Multiplayer button", pcls.c_str()); return false; }
    }
    HSM_LOG("ui: Multiplayer button added to %s", pcls.c_str());
    return true;
}

// The multiplayer page: a Border over the whole menu (opaque, so the stock buttons underneath can neither be seen nor
// clicked) with a title row, three columns (lobby and players | match settings | map list and Start) and Back.
bool BuildPanel(UObject* freeMode) {   // freeMode: style source for the page's buttons (may be null)
    UObject* tree = WidgetTree(g.menu);
    UObject** rootPtr = tree ? Field<UObject*>(tree, "RootWidget") : nullptr;
    UObject* root = rootPtr ? *rootPtr : nullptr;
    if (!root || !SameNameAscii(ObjectName(reinterpret_cast<UObject*>(ClassOf(root))), "CanvasPanel")) { HSM_LOG("ui: menu root is not a CanvasPanel; panel disabled"); return false; }
    g.panel = Spawn("Border");
    UObject* page = Spawn("VerticalBox");
    UObject* header = Spawn("HorizontalBox");
    UObject* body = Spawn("HorizontalBox");
    UObject* lobbyColumn = Spawn("VerticalBox");
    g.matchColumn = Spawn("VerticalBox");
    g.mapColumn = Spawn("VerticalBox");
    UObject* mapList = Spawn("ScrollBox");
    if (!g.panel || !page || !header || !body || !lobbyColumn || !g.matchColumn || !g.mapColumn || !mapList) {
        HSM_LOG("ui: multiplayer page widgets could not be made; panel disabled");
        g.panel = nullptr;
        return false;
    }
    Call add(root, "AddChildToCanvas"); add.set<UObject*>("Content", g.panel).invoke();
    if (UObject* slot = add.get<UObject*>("ReturnValue")) {
        Call(slot, "SetAnchors").set("InAnchors", Anchors{{0, 0}, {1, 1}}).invoke();
        Call(slot, "SetOffsets").set("InOffset", Margin{0, 0, 0, 0}).invoke();
        Call(slot, "SetZOrder").set<int32_t>("InZOrder", 100).invoke();
    }
    Call(g.panel, "SetBrushColor").set("InBrushColor", LinearColor{0.02f, 0.017f, 0.014f, 0.97f}).invoke();
    Call(g.panel, "SetPadding").set("InPadding", Margin{72, 48, 72, 40}).invoke();
    Call(g.panel, "SetContent").set<UObject*>("Content", page).invoke();

    AddToVerticalBox(page, header, 0);
    g.title = Spawn("TextBlock"); g.status = Spawn("TextBlock");
    for (UObject* t : {g.title, g.status}) if (t) StyleText(t);
    SetText(g.title, "MULTIPLAYER");
    if (g.title) AddToHorizontalBox(header, g.title, 0, 48);
    if (g.status) AddToHorizontalBox(header, g.status, 1, 0);
    FillVerticalBox(page, body, 28);
    AddToHorizontalBox(body, lobbyColumn, 1, 40);
    AddToHorizontalBox(body, g.matchColumn, 1, 40);
    AddToHorizontalBox(body, g.mapColumn, 1, 0);

    // Lobby: host or find one; once in, invite, leave and the players (the host clicks one to move them to another team).
    Label(lobbyColumn, "LOBBY", 0);
    g.hostFriends = MakeButton("Host lobby (friends)", freeMode);
    g.hostPublic = MakeButton("Host lobby (public)", freeMode);
    g.find = MakeButton("Find public lobbies", freeMode);
    g.invite = MakeButton("Invite friends", freeMode);
    g.leave = MakeButton("Leave lobby", freeMode);
    for (Button* b : {&g.hostFriends, &g.hostPublic, &g.find}) AddButton(lobbyColumn, *b, 6);
    for (int i = 0; i < 6; ++i) { Button r = MakeButton("", freeMode); if (r) { AddToVerticalBox(lobbyColumn, r.button, 2); SetVisible(r.button, false); g.rows.push_back(r); } }
    g.rowLobbies.assign(g.rows.size(), 0);
    g.idleHint = Label(lobbyColumn, "Host a lobby or join one to choose the mode and the map.", 16);
    for (Button* b : {&g.invite, &g.leave}) AddButton(lobbyColumn, *b, 6);
    g.playersTitle = Label(lobbyColumn, "PLAYERS", 24);
    for (int i = 0; i < game::kMaxLobbyMembers; ++i) { Button r = MakeButton("", freeMode); if (r) { AddToVerticalBox(lobbyColumn, r.button, 2); SetVisible(r.button, false); g.memberRows.push_back(r); } }
    g.memberIds.assign(g.memberRows.size(), 0);

    // Match settings: every click cycles one value (host only; members see them).
    Label(g.matchColumn, "MATCH SETTINGS", 0);
    g.mode = MakeButton("", freeMode); g.limit = MakeButton("", freeMode); g.timeLimit = MakeButton("", freeMode);
    g.friendlyFire = MakeButton("", freeMode); g.gear = MakeButton("", freeMode); g.tier = MakeButton("", freeMode);
    g.myTeam = MakeButton("", freeMode); g.balance = MakeButton("Auto-balance teams", freeMode);
    for (Button* b : {&g.mode, &g.limit, &g.timeLimit, &g.friendlyFire, &g.gear, &g.tier, &g.myTeam, &g.balance}) AddButton(g.matchColumn, *b, 6);
    g.hostOnly = Label(g.matchColumn, "Only the host can change these.", 16);

    // Map: every map the mode allows, the one a match would start on marked; Start below.
    Label(g.mapColumn, "MAP", 0);
    FillVerticalBox(g.mapColumn, mapList, 6);
    for (size_t i = 0; i < MapCount(); ++i) {
        Button r = MakeButton(MapAt(i).display, freeMode);   // kept even if empty: g.mapButtons[i] is kMaps[i]
        if (r) {
            Call c(mapList, "AddChild"); c.set<UObject*>("Content", r.button).invoke();
            if (UObject* slot = c.get<UObject*>("ReturnValue")) Call(slot, "SetPadding").set("InPadding", Margin{0, 2, 0, 2}).invoke();
        }
        g.mapButtons.push_back(r);
    }
    g.mapHint = Label(g.mapColumn, "The host picks the map.", 8);
    g.start = MakeButton("START MATCH", freeMode);
    AddButton(g.mapColumn, g.start, 12);

    g.close = MakeButton("Back", freeMode);
    if (g.close) if (UObject* slot = AddToVerticalBox(page, g.close.button, 20))
        Call(slot, "SetHorizontalAlignment").set<uint8_t>("InHorizontalAlignment", kHAlignLeft).invoke();
    SetVisible(g.panel, false);
    return true;
}

void Reset() { g = State{}; }

bool InViewport(UObject* widget) {
    Call v(widget, "IsInViewport"); v.invoke();
    return v.get<bool>("ReturnValue");
}

// UI_Startup_Menu (this build's main menu): image ribbons with the label baked in, so our buttons keep the default
// style and go below Quit (Button_3; Button_4 is Free Mode, decomp UI/UI_Startup_Menu.cpp LatentResume_L44).
// UI_MainMenuSNF (older builds): text buttons; ours copy ButtonFreeMode's style and TextFreeMode's font.
void Build() {
    bool startup = SameNameAscii(ObjectName(reinterpret_cast<UObject*>(g.menuClass)), "UI_Startup_Menu_C");
    const char* anchorName = startup ? "Button_3" : "ButtonFreeMode";
    UObject** anchorPtr = Field<UObject*>(g.menu, anchorName);
    g.built = true;
    if (!anchorPtr || !*anchorPtr) { HSM_LOG("ui: %s not found on the main menu", anchorName); return; }
    UObject* anchor = *anchorPtr;
    UObject* style = startup ? nullptr : anchor;
    if (!startup) if (UObject** tf = Field<UObject*>(g.menu, "TextFreeMode")) if (*tf) if (void* font = FieldPtr(*tf, "Font")) {
        FProperty* p = FindProperty(ClassOf(*tf), "Font");
        g.fontBytes.assign(static_cast<uint8_t*>(font), static_cast<uint8_t*>(font) + PropertySize(p));
    }
    if (!InjectOpenButton(anchor, style)) return;
    BuildPanel(style);
}

void UpdatePanel(Session& session) {
    game::Lobby& lobby = session.lobby();
    bool in = lobby.InLobby(), idle = lobby.state() == game::Lobby::State::Idle, host = in && lobby.IsHost();
    SetText(g.status, lobby.status());
    MatchConfig c = lobby.config();
    bool teams = TeamCount(c.format) > 0 && !IsBotFormat(c.format);
    SetShown(g.matchColumn, in);
    SetShown(g.mapColumn, in);
    SetVisible(g.idleHint, idle);
    SetVisible(g.playersTitle, in);
    SetVisible(g.hostOnly, in && !host);
    SetVisible(g.mapHint, in && !host);
    SetVisible(g.limit.button, in && !IsBotFormat(c.format));          // survival and zombies are endless
    SetVisible(g.gear.button, in && !IsBotFormat(c.format));
    SetVisible(g.tier.button, in && (IsBotFormat(c.format) || c.gear != Gear::Own));
    SetVisible(g.start.button, host);
    SetVisible(g.myTeam.button, in && teams);
    SetVisible(g.balance.button, in && teams && host);
    if (in) {
        SetText(g.mode.label, "Mode: " + FormatName(c.format));
        SetText(g.limit.label, std::string(c.format == Format::Duel ? "Rounds to win: " : "Score limit: ") + std::to_string(c.scoreLimit));
        SetText(g.timeLimit.label, "Time limit: " + TimeLabel(c.timeLimitS));
        SetText(g.friendlyFire.label, std::string("Friendly fire: ") + (c.friendlyFire ? "ON" : "OFF"));
        SetText(g.gear.label, std::string("Gear: ") + (c.gear == Gear::Own ? "own loadouts" : c.gear == Gear::RandomTier ? "random by tier" : "everyone identical"));
        SetText(g.tier.label, std::string(IsBotFormat(c.format) ? "Starting bot tier: " : "Gear tier: ") + std::to_string(int(c.tier)));
        int mine = -1;
        for (const game::LobbyMember& m : lobby.members()) if (m.id == steam::MySteamId()) mine = m.team;
        SetText(g.myTeam.label, "My team: " + TeamLabel(mine));
        const std::vector<MapRow> rows = MapRows(c.format, lobby.MapPick());
        for (size_t i = 0; i < g.mapButtons.size() && i < rows.size(); ++i) {
            SetVisible(g.mapButtons[i].button, rows[i].shown);
            SetText(g.mapButtons[i].label, rows[i].label);
            Call(g.mapButtons[i].button, "SetBackgroundColor")
                .set("InBackgroundColor", rows[i].selected ? LinearColor{1.0f, 0.72f, 0.3f, 1} : LinearColor{}).invoke();
        }
    }
    const auto& mem = lobby.members();
    for (size_t i = 0; i < g.memberRows.size(); ++i) {
        bool show = in && i < mem.size();
        SetVisible(g.memberRows[i].button, show);
        g.memberIds[i] = show ? mem[i].id : 0;
        if (!show) continue;
        const game::LobbyMember& m = mem[i];
        int team = m.hostOverride >= 0 ? m.hostOverride : m.team;
        SetText(g.memberRows[i].label, (m.host ? "[host] " : "") + m.name + (teams ? "  - " + TeamLabel(team) + (m.hostOverride >= 0 ? " (moved)" : "") : ""));
    }
    SetVisible(g.hostFriends.button, idle); SetVisible(g.hostPublic.button, idle); SetVisible(g.find.button, idle);
    SetVisible(g.invite.button, in); SetVisible(g.leave.button, in);
    const auto& list = lobby.listings();
    for (size_t i = 0; i < g.rows.size(); ++i) {
        bool show = idle && i < list.size();
        SetVisible(g.rows[i].button, show);
        g.rowLobbies[i] = show ? list[i].id : 0;
        if (show) SetText(g.rows[i].label, "Join: " + list[i].name + "  " + std::to_string(list[i].players) + "/" + std::to_string(game::kMaxLobbyMembers) +
                                           (list[i].map.empty() ? "" : "  " + list[i].map) + (list[i].compatible ? "" : "  (" + list[i].reason + ")"));
    }
}

void UpdateOpenLabel(Session& session) {
    game::Lobby& lobby = session.lobby();
    std::string label = "MULTIPLAYER";
    if (lobby.InLobby()) label = std::string(lobby.IsHost() ? "LOBBY (HOST) " : "LOBBY ") + std::to_string(lobby.members().size()) + "/" + std::to_string(game::kMaxLobbyMembers);
    SetText(g.openButton.label, label);
}

void HandleClicks(Session& session) {
    game::Lobby& lobby = session.lobby();
    if (Clicked(g.openButton)) { g.panelOpen = !g.panelOpen; SetVisible(g.panel, g.panelOpen); if (g.panelOpen) g.nextText = 0; }
    if (!g.panelOpen) return;
    if (Clicked(g.hostFriends)) lobby.Host(steam::LobbyType::FriendsOnly);
    if (Clicked(g.hostPublic)) lobby.Host(steam::LobbyType::Public);
    if (Clicked(g.invite)) lobby.Invite();
    if (Clicked(g.find)) lobby.RefreshList();
    if (Clicked(g.leave)) lobby.Leave();
    if (Clicked(g.close)) { g.panelOpen = false; SetVisible(g.panel, false); return; }
    for (size_t i = 0; i < g.rows.size(); ++i) if (g.rowLobbies[i] && Clicked(g.rows[i])) lobby.Join(g.rowLobbies[i]);
    if (!lobby.InLobby()) return;
    MatchConfig c = lobby.config();
    bool host = lobby.IsHost();
    int teams = IsBotFormat(c.format) ? 0 : TeamCount(c.format);
    if (Clicked(g.mode) && host) { c.format = Format((uint8_t(c.format) + 1) % kFormatCount); c.scoreLimit = DefaultScoreLimit(c.format); lobby.SetConfig(c); lobby.ClearOverrides(); }
    if (Clicked(g.limit) && host) { c.scoreLimit = uint16_t(NextOf({3, 5, 10, 15, 20, 30, 50}, c.scoreLimit)); lobby.SetConfig(c); }
    if (Clicked(g.timeLimit) && host) { c.timeLimitS = uint16_t(NextOf({0, 300, 600, 900, 1200}, c.timeLimitS)); lobby.SetConfig(c); }
    if (Clicked(g.friendlyFire) && host) { c.friendlyFire = !c.friendlyFire; lobby.SetConfig(c); }
    if (Clicked(g.gear) && host) { c.gear = Gear((uint8_t(c.gear) + 1) % kGearCount); lobby.SetConfig(c); }
    if (Clicked(g.tier) && host) { c.tier = uint8_t((c.tier + 1) % 8); lobby.SetConfig(c); }
    for (size_t i = 0; i < g.mapButtons.size(); ++i)
        if (Clicked(g.mapButtons[i]) && host && MapAllowsFormat(MapAt(i), c.format)) { lobby.SetMapPick(MapAt(i).path); g.nextText = 0; }
    if (Clicked(g.start) && host) {
        size_t m = EffectiveMap(c.format, lobby.MapPick());
        if (m < MapCount()) session.StartOnMap(MapAt(m).path);
    }
    if (Clicked(g.myTeam)) {
        int mine = -1;
        for (const game::LobbyMember& m : lobby.members()) if (m.id == steam::MySteamId()) mine = m.team;
        lobby.SetTeamPick(NextTeamPick(int8_t(mine), teams));
    }
    if (Clicked(g.balance) && host) lobby.ClearOverrides();
    for (size_t i = 0; i < g.memberRows.size(); ++i) {
        if (!g.memberIds[i] || !Clicked(g.memberRows[i]) || !host || teams == 0) continue;
        for (const game::LobbyMember& m : lobby.members())
            if (m.id == g.memberIds[i]) lobby.SetOverride(m.id, NextTeamPick(int8_t(m.hostOverride >= 0 ? m.hostOverride : m.team), teams));
    }
}
}  // namespace

void Tick(Session& session, UObject* worldContext) {
    if (!session.steamMode()) return;
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
    if (!g.openButton) return;
    HandleClicks(session);
    if (now >= g.nextText) { g.nextText = now + 250; UpdateOpenLabel(session); if (g.panelOpen) UpdatePanel(session); }
}

}  // namespace hsm::ui

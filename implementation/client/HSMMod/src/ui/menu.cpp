#include "ui/menu.h"
#include "gameplay/session.h"
#include "log.h"
#include "match/hsm_hud_text.h"
#include "match/hsm_maps.h"
#include <windows.h>
#include <string>
#include <vector>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

struct Vec2 { double x = 0, y = 0; };
struct Anchors { Vec2 min, max; };
struct Margin { float left = 0, top = 0, right = 0, bottom = 0; };
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
    UObject* panel = nullptr;
    UObject *title = nullptr, *status = nullptr, *members = nullptr;
    Button hostFriends, hostPublic, invite, leave, find, close;
    Button mode, limit, timeLimit, friendlyFire, myTeam, balance;   // match settings (host edits; members see them)
    Button gear, tier, map, start;
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

bool BuildPanel(UObject* freeMode) {   // freeMode: style source for the panel's buttons (may be null)
    UObject* tree = WidgetTree(g.menu);
    UObject** rootPtr = tree ? Field<UObject*>(tree, "RootWidget") : nullptr;
    UObject* root = rootPtr ? *rootPtr : nullptr;
    if (!root || ObjectName(reinterpret_cast<UObject*>(ClassOf(root))) != "CanvasPanel") { HSM_LOG("ui: menu root is not a CanvasPanel; panel disabled"); return false; }
    g.panel = Spawn("VerticalBox");
    if (!g.panel) return false;
    Call add(root, "AddChildToCanvas"); add.set<UObject*>("Content", g.panel).invoke();
    if (UObject* slot = add.get<UObject*>("ReturnValue")) {
        Call(slot, "SetAnchors").set("InAnchors", Anchors{{0.62, 0.15}, {0.62, 0.15}}).invoke();
        Call(slot, "SetAutoSize").set<bool>("InbAutoSize", true).invoke();
        Call(slot, "SetZOrder").set<int32_t>("InZOrder", 100).invoke();
    }
    g.title = Spawn("TextBlock"); g.status = Spawn("TextBlock"); g.members = Spawn("TextBlock");
    for (UObject* t : {g.title, g.status, g.members}) { StyleText(t); AddToVerticalBox(g.panel, t, 6); }
    SetText(g.title, "MULTIPLAYER");
    g.hostFriends = MakeButton("Host lobby (friends)", freeMode);
    g.hostPublic = MakeButton("Host lobby (public)", freeMode);
    g.invite = MakeButton("Invite friends", freeMode);
    g.find = MakeButton("Find public lobbies", freeMode);
    g.leave = MakeButton("Leave lobby", freeMode);
    g.close = MakeButton("Close", freeMode);
    for (Button* b : {&g.hostFriends, &g.hostPublic, &g.invite, &g.find, &g.leave, &g.close}) if (*b) AddToVerticalBox(g.panel, b->button, 4);
    g.mode = MakeButton("", freeMode); g.limit = MakeButton("", freeMode); g.timeLimit = MakeButton("", freeMode);
    g.friendlyFire = MakeButton("", freeMode); g.myTeam = MakeButton("", freeMode); g.balance = MakeButton("Auto-balance teams", freeMode);
    g.gear = MakeButton("", freeMode); g.tier = MakeButton("", freeMode); g.map = MakeButton("", freeMode); g.start = MakeButton("Start match", freeMode);
    for (Button* b : {&g.mode, &g.limit, &g.timeLimit, &g.friendlyFire, &g.gear, &g.tier, &g.map, &g.start, &g.myTeam, &g.balance})
        if (*b) { AddToVerticalBox(g.panel, b->button, 2); SetVisible(b->button, false); }
    for (int i = 0; i < game::kMaxLobbyMembers; ++i) { Button r = MakeButton("", freeMode); if (r) { AddToVerticalBox(g.panel, r.button, 1); SetVisible(r.button, false); g.memberRows.push_back(r); } }
    g.memberIds.assign(g.memberRows.size(), 0);
    for (int i = 0; i < 6; ++i) { Button r = MakeButton("", freeMode); if (r) { AddToVerticalBox(g.panel, r.button, 2); SetVisible(r.button, false); g.rows.push_back(r); } }
    g.rowLobbies.assign(g.rows.size(), 0);
    SetVisible(g.panel, false);
    return true;
}

void Reset() { g = State{}; }

// UI_Startup_Menu (this build's main menu): image ribbons with the label baked in, so our buttons keep the default
// style and go below Quit (Button_3; Button_4 is Free Mode, decomp UI/UI_Startup_Menu.cpp LatentResume_L44).
// UI_MainMenuSNF (older builds): text buttons; ours copy ButtonFreeMode's style and TextFreeMode's font.
void Build() {
    bool startup = ObjectName(reinterpret_cast<UObject*>(g.menuClass)) == "UI_Startup_Menu_C";
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
    bool in = lobby.InLobby(), idle = lobby.state() == game::Lobby::State::Idle;
    SetText(g.status, lobby.status());
    SetText(g.members, "");
    MatchConfig c = lobby.config();
    bool teams = TeamCount(c.format) > 0 && !IsBotFormat(c.format);
    for (Button* b : {&g.mode, &g.timeLimit, &g.friendlyFire, &g.map}) SetVisible(b->button, in);
    SetVisible(g.limit.button, in && !IsBotFormat(c.format));          // survival and zombies are endless
    SetVisible(g.gear.button, in && !IsBotFormat(c.format));
    SetVisible(g.tier.button, in && (IsBotFormat(c.format) || c.gear != Gear::Own));
    SetVisible(g.start.button, in && lobby.IsHost());
    SetVisible(g.myTeam.button, in && teams);
    SetVisible(g.balance.button, in && teams && lobby.IsHost());
    if (in) {
        SetText(g.mode.label, "Mode: " + FormatName(c.format));
        SetText(g.limit.label, std::string(c.format == Format::Duel ? "Rounds to win: " : "Score limit: ") + std::to_string(c.scoreLimit));
        SetText(g.timeLimit.label, "Time limit: " + TimeLabel(c.timeLimitS));
        SetText(g.friendlyFire.label, std::string("Friendly fire: ") + (c.friendlyFire ? "ON" : "OFF"));
        SetText(g.gear.label, std::string("Gear: ") + (c.gear == Gear::Own ? "own loadouts" : c.gear == Gear::RandomTier ? "random by tier" : "everyone identical"));
        SetText(g.tier.label, std::string(IsBotFormat(c.format) ? "Starting bot tier: " : "Gear tier: ") + std::to_string(int(c.tier)));
        const MapInfo* picked = FindMap(lobby.MapPick());
        SetText(g.map.label, std::string("Map: ") + (picked && MapAllowsFormat(*picked, c.format) ? picked->display : "choose"));
        int mine = -1;
        for (const game::LobbyMember& m : lobby.members()) if (m.id == steam::MySteamId()) mine = m.team;
        SetText(g.myTeam.label, "My team: " + TeamLabel(mine));
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
        if (show) SetText(g.rows[i].label, list[i].name + "  " + std::to_string(list[i].players) + "/" + std::to_string(game::kMaxLobbyMembers) +
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
    if (Clicked(g.openButton)) { g.panelOpen = !g.panelOpen; SetVisible(g.panel, g.panelOpen); if (g.panelOpen) g.nextText = 0; HSM_LOG("ui: multiplayer panel open=%d", int(g.panelOpen)); }
    if (!g.panelOpen) return;
    if (Clicked(g.hostFriends)) lobby.Host(steam::LobbyType::FriendsOnly);
    if (Clicked(g.hostPublic)) lobby.Host(steam::LobbyType::Public);
    if (Clicked(g.invite)) lobby.Invite();
    if (Clicked(g.find)) lobby.RefreshList();
    if (Clicked(g.leave)) lobby.Leave();
    if (Clicked(g.close)) { g.panelOpen = false; SetVisible(g.panel, false); }
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
    if (Clicked(g.map) && host) {
        const MapInfo* cur = FindMap(lobby.MapPick());
        size_t at = cur ? size_t(cur - &MapAt(0)) : MapCount() - 1;
        size_t next = NextMap(at, c.format);
        if (next < MapCount()) lobby.SetMapPick(MapAt(next).path);
    }
    if (Clicked(g.start) && host) {
        const MapInfo* m = FindMap(lobby.MapPick());
        if (!m || !MapAllowsFormat(*m, c.format)) { size_t n = NextMap(MapCount() - 1, c.format); m = n < MapCount() ? &MapAt(n) : nullptr; }
        if (m) session.StartOnMap(m->path);
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
    if (!StillAlive(g.menu, g.menuIndex, g.menuClass)) {
        if (g.menu) { UObject* wc = g.worldContext; Reset(); g.worldContext = wc; }
        if (now < g.nextScan) return;
        g.nextScan = now + 500;
        for (const char* name : {"UI_Startup_Menu_C", "UI_MainMenuSNF_C"}) {
            std::vector<UObject*> live = FindInstances(name);
            if (live.empty()) continue;
            g.menu = live[0]; g.menuIndex = ObjectIndex(live[0]); g.menuClass = ClassOf(live[0]);
            break;
        }
        if (!g.menu) return;
    }
    Call inView(g.menu, "IsInViewport"); inView.invoke();
    if (!inView.get<bool>("ReturnValue")) return;
    if (!g.built) Build();
    if (!g.openButton) return;
    HandleClicks(session);
    if (now >= g.nextText) { g.nextText = now + 250; UpdateOpenLabel(session); if (g.panelOpen) UpdatePanel(session); }
}

}  // namespace hsm::ui

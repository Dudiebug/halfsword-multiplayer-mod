// The multiplayer page's navigation (hsm_lobby_nav.h): the tab order MODE, MAP, RULES, MODS, CLASSES, when CLASSES shows and where
// YOUR CLASS goes, the footer's bottom-left button in each view, the MY REGION text, and the RULES sub-tabs each mode uses.
#include "match/hsm_lobby_nav.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static LobbyPlayer Player(const char* name, bool host, bool me) { LobbyPlayer p; p.name = name; p.host = host; p.me = me; return p; }
static LobbyLocal InLobby(bool host, Format f = Format::FreeForAll, Gear g = Gear::Own) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.2.0"; l.myName = host ? "Ann" : "Bob";
    l.config.format = f; l.config.gear = g; l.config.scoreLimit = DefaultScoreLimit(f);
    l.players = {Player("Ann", true, host), Player("Bob", false, !host)};
    return l;
}
using Secs = std::vector<LobbySection>;
static Secs TabsOf(const LobbyLocal& l) { return RulesTabs(BuildLobbyScene(l)); }

static void TestTabOrder() {
    // MODE, MAP, RULES, MODS, CLASSES: the order the page draws them and the order the click loop numbers them.
    CHECK(kModeTab == 0 && kMapTab == 1 && kRulesTab == 2 && kModsTab == 3 && kClassesTab == 4 && kTabCount == 5);
    CHECK(std::string(TabName(kModeTab)) == "MODE" && std::string(TabName(kMapTab)) == "MAP" && std::string(TabName(kRulesTab)) == "RULES");
    CHECK(std::string(TabName(kModsTab)) == "MODS" && std::string(TabName(kClassesTab)) == "CLASSES" && std::string(TabName(kTabCount)).empty());
}

static void TestClassesTab() {
    // CLASSES shows only in a lobby that uses classes: our class is named only then (the page sets it only in a classes lobby).
    LobbyLocal l = InLobby(false);
    CHECK(!ClassesTabShown(BuildLobbyScene(l)));                                   // vanilla: no class
    l.myClass = "Knight";
    CHECK(ClassesTabShown(BuildLobbyScene(l)));
    LobbyLocal idle; idle.phase = LobbyPhase::Idle; idle.version = "0.2.0"; idle.myClass = "Knight";
    CHECK(!ClassesTabShown(BuildLobbyScene(idle)));                                // not in a lobby: the class page, not a tab
    LobbyScene apart = BuildLobbyScene(idle);
    apart.classLine = "Knight";                                                    // even a scene that names a class, outside a lobby
    CHECK(!apart.in && !ClassesTabShown(apart));
    // Every other tab always shows; CLASSES follows the flag.
    for (size_t t = 0; t < kTabCount; ++t) CHECK(TabShown(t, true));
    for (size_t t = 0; t < kTabCount; ++t) CHECK(TabShown(t, false) == (t != kClassesTab));
    CHECK(!TabShown(kTabCount, true) && !TabShown(99, true));
    // A tab that went away falls back to MODE; the others stay.
    CHECK(ClampTab(kClassesTab, true) == kClassesTab && ClampTab(kClassesTab, false) == kModeTab);
    for (size_t t = 0; t < kClassesTab; ++t) CHECK(ClampTab(t, false) == t && ClampTab(t, true) == t);
    CHECK(ClampTab(99, true) == kModeTab);
}

static void TestClassesTabFollowsRow() {
    // The CLASSES tab and the YOUR CLASS line follow the Classes row, with every Mods allowed preset: On and Unrestricted show them, Off does not.
    const ClassesMode rows[3] = {ClassesMode::Off, ClassesMode::On, ClassesMode::Unrestricted};
    for (uint8_t p = 0; p <= kMaxModPreset; ++p)
        for (const ClassesMode row : rows) {
            ModRule rule = NoRule(); rule.preset = ModPreset(p); rule.classes = row;
            LobbyLocal l = InLobby(false);
            l.myClass = LobbyClassName(rule.classes, "Knight");
            const LobbyScene s = BuildLobbyScene(l);
            CHECK(ClassesTabShown(s) == (row != ClassesMode::Off));
            CHECK((s.classLine == "YOUR CLASS: Knight") == (row != ClassesMode::Off));
            CHECK(YourClassGoes(s.in, ClassesTabShown(s)) == (row == ClassesMode::Off ? ClassGoes::Page : ClassGoes::Tab));
            CHECK(ClassBudgetApplies(row) == (row == ClassesMode::On));
        }
    // The same through an older host's lobby keys: its Classes (5) and Unrestricted Classes (6) show the tab, its presets 0..4 do not.
    for (const char* rule : {"0", "1", "2", "3", "4", ""}) CHECK(LobbyClassName(LobbyClassesMode(rule, ""), "Knight").empty());
    CHECK(LobbyClassName(LobbyClassesMode("5", ""), "Knight") == "Knight" && LobbyClassName(LobbyClassesMode("6", ""), "Knight") == "Knight");
    // The tab goes when the host turns the row off while a player sits on it.
    CHECK(ClampTab(kClassesTab, ClassesTabShown(BuildLobbyScene([] { LobbyLocal l = InLobby(false); l.myClass = LobbyClassName(ClassesMode::Off, "Knight"); return l; }()))) == kModeTab);
}

static void TestYourClass() {
    // In a lobby with the tab: the tab. In a lobby without it, or outside one: the class page.
    CHECK(YourClassGoes(true, true) == ClassGoes::Tab);
    CHECK(YourClassGoes(true, false) == ClassGoes::Page);
    CHECK(YourClassGoes(false, true) == ClassGoes::Page && YourClassGoes(false, false) == ClassGoes::Page);
}

static void TestFooter() {
    // The lobby itself: LEAVE LOBBY and INVITE FRIENDS, and no back button (the lobby has no menu to go back to).
    Footer f = FooterFor(true, PageView::Lobby);
    CHECK(f.leave && f.invite && f.back.empty());
    // The lobby list: BACK TO MAIN MENU, no lobby buttons.
    f = FooterFor(false, PageView::List);
    CHECK(!f.leave && !f.invite && f.back == "BACK TO MAIN MENU");
    // Settings and Your class open over either: the bottom-left button goes back to where they came from, and leaving is not offered.
    for (PageView v : {PageView::Settings, PageView::Class}) {
        f = FooterFor(true, v);
        CHECK(!f.leave && !f.invite && f.back == "BACK TO LOBBY");
        f = FooterFor(false, v);
        CHECK(!f.leave && !f.invite && f.back == "BACK TO LOBBY LIST");
    }
    // LEAVE LOBBY never shows outside a lobby, whatever the view.
    for (PageView v : {PageView::List, PageView::Lobby, PageView::Settings, PageView::Class}) CHECK(!FooterFor(false, v).leave && !FooterFor(false, v).invite);
}

static void TestMyRegion() {
    CHECK(MyRegionText("Auto (NA East)") == "MY REGION: AUTO (NA EAST)");
    CHECK(MyRegionText("Auto (unknown)") == "MY REGION: AUTO (UNKNOWN)");
    CHECK(MyRegionText("Europe") == "MY REGION: EUROPE");
    // From the scene: the picker's button text rides in the scene as it did on the header.
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0"; l.regionPick = Region::Auto; l.regionGuess = Region::NaEast;
    CHECK(MyRegionText(BuildLobbyScene(l).regionButton) == "MY REGION: AUTO (NA EAST)");
    l.regionPick = Region::Oceania;
    CHECK(MyRegionText(BuildLobbyScene(l).regionButton) == "MY REGION: OCEANIA");
}

static void TestRulesTabs() {
    using S = LobbySection;
    // One sub-tab per section the mode uses, in section order.
    CHECK(TabsOf(InLobby(true)) == (Secs{S::Match, S::Combat, S::Gear, S::World}));                                 // Carnage: no teams
    CHECK(TabsOf(InLobby(true, Format::Duel)) == (Secs{S::Match, S::Combat, S::Gear, S::World}));
    CHECK(TabsOf(InLobby(true, Format::TwoTeams)) == (Secs{S::Match, S::Combat, S::Teams, S::Gear, S::World}));      // all five
    CHECK(TabsOf(InLobby(true, Format::TeamDuel)) == (Secs{S::Match, S::Combat, S::Teams, S::Gear, S::World}));
    CHECK(TabsOf(InLobby(true, Format::PlayersVsBots)) == (Secs{S::Match, S::Combat, S::Teams, S::Gear, S::World}));  // friendly fire is a TEAMS row
    CHECK(TabsOf(InLobby(true, Format::Riot)) == (Secs{S::Match, S::Combat, S::Teams, S::Gear, S::World}));         // Strip is a GEAR row
    // Brawl: bare fists, so the Gear row goes but Strip stays; Tag has the same rows.
    CHECK(TabsOf(InLobby(true, Format::FreeForAll, Gear::Fists)) == (Secs{S::Match, S::Combat, S::Gear, S::World}));
    // Progression: the player limit and the damage rows.
    LobbyLocal prog = InLobby(true); prog.progression = true;
    CHECK(TabsOf(prog) == (Secs{S::Match, S::Combat}));
    // Not in a lobby: nothing to set.
    LobbyLocal idle; idle.phase = LobbyPhase::Idle; idle.version = "0.2.0";
    CHECK(TabsOf(idle).empty());
    // Every sub-tab holds a row of its own section, and no row is left without a sub-tab.
    for (int i = 0; i < kFormatCount; ++i) {
        const LobbyScene s = BuildLobbyScene(InLobby(true, Format(i)));
        const Secs tabs = RulesTabs(s);
        for (const SettingRow& r : s.settings) {
            bool found = false;
            for (LobbySection t : tabs) found = found || t == SectionOf(r.id);
            CHECK(found);
        }
        for (LobbySection t : tabs) {
            bool any = false;
            for (const SettingRow& r : s.settings) any = any || SectionOf(r.id) == t;
            CHECK(any);
        }
        for (size_t k = 1; k < tabs.size(); ++k) CHECK(tabs[k - 1] < tabs[k]);
    }
    // The sub-tab asked for if the mode has it, otherwise the first one (a switch of mode that drops TEAMS).
    const Secs duel = TabsOf(InLobby(true, Format::Duel));
    CHECK(ClampRulesTab(duel, S::Gear) == S::Gear && ClampRulesTab(duel, S::Teams) == S::Match && ClampRulesTab(duel, S::World) == S::World);
    CHECK(ClampRulesTab(Secs{}, S::Gear) == S::Match);
}

int main() {
    TestTabOrder();
    TestClassesTab();
    TestClassesTabFollowsRow();
    TestYourClass();
    TestFooter();
    TestMyRegion();
    TestRulesTabs();
    if (g_fail) { std::printf("%d lobby nav checks failed\n", g_fail); return 1; }
    std::printf("all lobby nav tests passed\n");
    return 0;
}

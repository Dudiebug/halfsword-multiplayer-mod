// The multiplayer page's navigation as pure rules: the tabs and which of them show, the RULES sub-tabs a lobby uses, the
// footer's buttons and what the bottom-left one says, where YOUR CLASS goes, and the text of the MY REGION button. ui/menu.cpp
// only draws what these say. Game-independent and unit-tested (test_hsm_lobby_nav.cpp).
#pragma once
#include "match/hsm_lobby_scene.h"
#include "session/hsm_mod_rules.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

// The tabs, in the order the page shows them. CLASSES shows only in a lobby that uses classes (the Classes setting is anything
// other than Off: any Mods allowed preset): outside it, and while a host has it off, YOUR CLASS opens the class page instead.
inline constexpr size_t kModeTab = 0, kMapTab = 1, kRulesTab = 2, kModsTab = 3, kClassesTab = 4;
inline constexpr size_t kTabCount = 5;
inline const char* TabName(size_t tab) {
    switch (tab) {
        case kModeTab: return "MODE";
        case kMapTab: return "MAP";
        case kRulesTab: return "RULES";
        case kModsTab: return "MODS";
        case kClassesTab: return "CLASSES";
        default: return "";
    }
}
// Whether the lobby of this scene uses classes: the scene names our class (match/hsm_lobby_scene.h classLine) only then.
inline bool ClassesTabShown(const LobbyScene& s) { return s.in && !s.classLine.empty(); }
// The class the page names in a lobby: only where the Classes row is On or Unrestricted, whatever the Mods allowed preset is. Empty
// (so no CLASSES tab and no YOUR CLASS line) when the row is Off.
inline std::string LobbyClassName(ClassesMode row, const std::string& name) { return ClassesOn(row) ? name : std::string(); }
// The class budget (points to spend) applies in On and not in Unrestricted, which has none.
inline bool ClassBudgetApplies(ClassesMode row) { return row == ClassesMode::On; }
inline bool TabShown(size_t tab, bool classesTab) { return tab < kTabCount && (tab != kClassesTab || classesTab); }
// The tab to draw: a CLASSES tab that went away (the host turned Classes off) falls back to the first tab.
inline size_t ClampTab(size_t tab, bool classesTab) { return TabShown(tab, classesTab) ? tab : kModeTab; }

// What the page shows apart from the tabs of a lobby: the lobby list (not in a lobby), the lobby, the Settings view and the
// Your class page (the class page opens over a lobby too, when the CLASSES tab is hidden).
enum class PageView : uint8_t { List, Lobby, Settings, Class };
struct Footer {
    std::string back;                   // the bottom-left button, saying where it goes; empty: not shown (the lobby has LEAVE LOBBY there)
    bool leave = false;                 // LEAVE LOBBY: on the lobby itself only, so Settings never leaves a lobby by mistake
    bool invite = false;                // INVITE FRIENDS: on the lobby itself too
};
// YOUR CLASS, SETTINGS and REPORT A BUG show in every view; these are the buttons that change.
inline Footer FooterFor(bool inLobby, PageView view) {
    Footer f;
    const bool apart = view == PageView::Settings || view == PageView::Class;   // a view opened from the page: its button goes back
    f.leave = inLobby && !apart;
    f.invite = inLobby && !apart;
    if (apart) f.back = inLobby ? "BACK TO LOBBY" : "BACK TO LOBBY LIST";
    else if (!inLobby) f.back = "BACK TO MAIN MENU";
    return f;
}
// Where YOUR CLASS goes: to the CLASSES tab of a lobby that has it, otherwise to the class page.
enum class ClassGoes : uint8_t { Tab, Page };
inline ClassGoes YourClassGoes(bool inLobby, bool classesTab) { return inLobby && classesTab ? ClassGoes::Tab : ClassGoes::Page; }

// The MY REGION button in SETTINGS, from the scene's regionButton ("Auto (NA East)"): "MY REGION: AUTO (NA EAST)".
inline std::string MyRegionText(const std::string& regionButton) { return "MY REGION: " + Upper(regionButton); }

// The MY REGION list in SETTINGS: Auto (with what the guess found, "Auto (Europe)") and the six regions, in the order the button
// stepped them. Option i is Region(i).
inline std::vector<DropOption> MyRegionChoices(Region guessed) {
    std::vector<DropOption> v;
    for (size_t i = 0; i < kRegionCount; ++i) v.push_back({RegionPickLabel(Region(i), guessed), true, ""});
    return v;
}
inline size_t MyRegionChoiceAt(Region pick) { return size_t(pick) < kRegionCount ? size_t(pick) : 0; }
inline Region MyRegionPicked(size_t i) { return i < kRegionCount ? Region(i) : Region::Auto; }

// The page a dropdown was opened on, as one number: it changes with the page being open, the view, being in a lobby, the tab, the
// RULES sub-tab and a window over the page, so an open list closes when any of them does (Dropdown::Context).
inline uint32_t PageContextKey(bool pageOpen, PageView view, bool inLobby, size_t tab, LobbySection rules, bool modal) {
    return (pageOpen ? 1u : 0u) | (uint32_t(view) << 1) | (inLobby ? 1u << 3 : 0u) | (uint32_t(tab & 7) << 4) | (uint32_t(rules) << 7) | (modal ? 1u << 10 : 0u);
}

// The RULES tab's sub-tabs for a scene: each section that holds a row, in section order. A mode that has no gear rows has no
// GEAR sub-tab, and Progression has MATCH (the player limit) and COMBAT alone.
inline std::vector<LobbySection> RulesTabs(const LobbyScene& s) {
    bool used[kSectionCount] = {};
    for (const SettingRow& r : s.settings) used[size_t(SectionOf(r.id))] = true;
    std::vector<LobbySection> v;
    for (size_t i = 0; i < kSectionCount; ++i) if (used[i]) v.push_back(LobbySection(i));
    return v;
}
// The sub-tab to draw: the one asked for if the mode has rows in it, otherwise the first.
inline LobbySection ClampRulesTab(const std::vector<LobbySection>& tabs, LobbySection want) {
    for (LobbySection t : tabs) if (t == want) return want;
    return tabs.empty() ? LobbySection::Match : tabs.front();
}

}  // namespace hsm

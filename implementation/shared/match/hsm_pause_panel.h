// The mod's pause panel in a multiplayer match (mod_pause=1): which buttons it has, in which order, which need a second
// click, and when a QUIT TO DESKTOP may close the game. Game-independent and unit-tested; ui/pause_panel.cpp draws it and
// gameplay/session.cpp carries the actions out. Esc ownership is in match/hsm_input_mode.h.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace hsm {

// The nine buttons, top to bottom. They take over everything the game's own pause menu did, so GAME MENU is gone:
//   RESUME            closes the panel
//   MULTIPLAYER MENU  the players (mute, volume, kick, votekick), voice and network page
//   LOADOUT           the game's own inventory screen, as a trial: only with loadout_trial=1 (LoadoutVisitEnds)
//   SETTINGS          the game's own settings screen; its Back brings the panel again
//   MOD SETTINGS      HELP IMPROVE THE MOD, MENU LOOK and the voice settings
//   REPORT A BUG      closes the panel and opens the chat with "/bug " typed (only when bug reports are on)
//   LEAVE MATCH       leaves the session, back to the main menu            (second click confirms)
//   QUIT TO MAIN MENU what the game's button does after leaving the session (second click confirms)
//   QUIT TO DESKTOP   leaves the session, then closes the game              (second click confirms)
enum class PauseButton : uint8_t { Resume, Multiplayer, Loadout, Settings, ModSettings, ReportBug, LeaveMatch, QuitMainMenu, QuitDesktop };
constexpr size_t kPauseButtonCount = 9;

// LOADOUT is a trial behind loadout_trial=1 (default 0). The game's inventory screen (UI_Tavern_Inventory_C) was made for
// the Tavern: it looks for the Tavern's camera and dress-up figure, and its exit makes the Tavern's hub menu. The trial
// opens it over a match to see what works in an arena; session.cpp's OpenLoadoutTrial() does it and logs every step.

inline const char* PauseButtonLabel(PauseButton b) {
    switch (b) {
        case PauseButton::Resume: return "RESUME";
        case PauseButton::Multiplayer: return "MULTIPLAYER MENU";
        case PauseButton::Loadout: return "LOADOUT";
        case PauseButton::Settings: return "SETTINGS";
        case PauseButton::ModSettings: return "MOD SETTINGS";
        case PauseButton::ReportBug: return "REPORT A BUG";
        case PauseButton::LeaveMatch: return "LEAVE MATCH";
        case PauseButton::QuitMainMenu: return "QUIT TO MAIN MENU";
        default: return "QUIT TO DESKTOP";
    }
}
// What the label reads after the first click; nullptr: the button acts at once.
inline const char* PauseButtonConfirm(PauseButton b) {
    switch (b) {
        case PauseButton::LeaveMatch: return "CLICK AGAIN TO LEAVE";
        case PauseButton::QuitMainMenu:
        case PauseButton::QuitDesktop: return "CLICK AGAIN TO QUIT";
        default: return nullptr;
    }
}
// Shown on the main list. reportOn: bug reports are on and the player is in a session fight. loadoutTrial: loadout_trial=1.
inline bool PauseButtonShown(PauseButton b, bool reportOn, bool loadoutTrial) {
    if (b == PauseButton::Loadout) return loadoutTrial;
    if (b == PauseButton::ReportBug) return reportOn;
    return true;
}

// The LOADOUT trial: the game's inventory screen is up over the match and the panel waits out of sight. What ends the
// visit: Esc (the screen has no Esc of its own, so the mod takes it down: nobody is left stuck behind a screen that does
// not work here), or the screen being gone (its own exit ran) once kLoadoutVisitGraceMs have passed since it opened (a
// widget is not in the viewport in the very frame it is made).
enum class LoadoutVisitEnd : uint8_t { None, Esc, ScreenClosed };
constexpr uint32_t kLoadoutVisitGraceMs = 500;
inline LoadoutVisitEnd LoadoutVisitEnds(bool screenUp, bool escPressed, uint32_t sinceOpenMs) {
    if (escPressed) return LoadoutVisitEnd::Esc;
    if (!screenUp && sinceOpenMs >= kLoadoutVisitGraceMs) return LoadoutVisitEnd::ScreenClosed;
    return LoadoutVisitEnd::None;
}
inline const char* LoadoutVisitEndText(LoadoutVisitEnd e) {
    switch (e) {
        case LoadoutVisitEnd::Esc: return "Esc";
        case LoadoutVisitEnd::ScreenClosed: return "the screen's own exit";
        default: return "still open";
    }
}
inline std::array<PauseButton, kPauseButtonCount> PauseButtonOrder() {
    return {PauseButton::Resume, PauseButton::Multiplayer, PauseButton::Loadout, PauseButton::Settings, PauseButton::ModSettings,
            PauseButton::ReportBug, PauseButton::LeaveMatch, PauseButton::QuitMainMenu, PauseButton::QuitDesktop};
}

// The second click. The first click on a button with a confirm label arms it; a second click on the same button within
// kPauseConfirmMs does it. A click on any other button, or the wait running out, disarms it.
constexpr uint32_t kPauseConfirmMs = 4000;
class PauseConfirm {
public:
    // True: do it now. False: armed (or nothing to confirm, in which case the caller acts on its own).
    bool Click(PauseButton b, uint32_t nowMs) {
        if (!PauseButtonConfirm(b)) { armed_ = false; return false; }
        if (Armed(b, nowMs)) { armed_ = false; return true; }
        armed_ = true;
        which_ = b;
        at_ = nowMs;
        return false;
    }
    bool Armed(PauseButton b, uint32_t nowMs) const { return armed_ && which_ == b && uint32_t(nowMs - at_) < kPauseConfirmMs; }
    void Clear() { armed_ = false; }
private:
    bool armed_ = false;
    PauseButton which_ = PauseButton::Resume;
    uint32_t at_ = 0;
};

// QUIT TO DESKTOP leaves the session first. The player's own save is put back once the game is in a menu level again (the
// save guard stops blocking), so the game closes then, or after kQuitDesktopMaxMs at the latest (the next start puts a
// backup back by itself).
constexpr uint32_t kQuitDesktopMaxMs = 8000;
inline bool QuitDesktopDue(bool saveGuardBlocking, uint32_t waitedMs) { return !saveGuardBlocking || waitedMs >= kQuitDesktopMaxMs; }

}  // namespace hsm

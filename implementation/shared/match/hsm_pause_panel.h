// The mod's pause panel in a multiplayer match: which buttons it has, in which order, what each says, which need a second
// click, what the line under the panel says, and when a QUIT TO DESKTOP may close the game. Game-independent and
// unit-tested; ui/pause_panel.cpp draws it and gameplay/session.cpp carries the actions out. Esc ownership is in
// match/hsm_input_mode.h.
#pragma once
#include "match/hsm_hud_style.h"
#include "match/hsm_notice_text.h"
#include "session/hsm_bench.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace hsm {

// The ten buttons, top to bottom. They take over everything the game's own pause menu did, so GAME MENU is gone:
//   RESUME             closes the panel
//   PLAYERS            the players (mute, volume, kick, votekick) and network page
//   LOADOUT            the game's own inventory screen, as a trial: only with loadout_trial=1 (LoadoutVisitEnds)
//   CHANGE LOADOUT     sends the loadout the player edited to the host (session/hsm_loadout_change.h); dimmed, with the rule's reason
//   GAME SETTINGS      the game's own settings screen; its Back brings the panel again
//   MOD SETTINGS       HELP IMPROVE THE MOD, MENU LOOK and the voice settings
//   REPORT A BUG       closes the panel and opens the chat with "/bug " typed; dimmed, with a reason, when bug reports are off
//   BACK TO LOBBY      the host ends the match for everyone and the lobby page comes back; a guest leaves the match and
//                      waits in the lobby for the next one (session/hsm_bench.h)                   (second click confirms)
//   LEAVE TO MAIN MENU leaves the session, back to the main menu                                   (second click confirms)
//   QUIT TO DESKTOP    leaves the session, then closes the game                                    (second click confirms)
enum class PauseButton : uint8_t { Resume, Players, Loadout, ChangeLoadout, GameSettings, ModSettings, ReportBug, BackToLobby, LeaveToMainMenu, QuitDesktop };
constexpr size_t kPauseButtonCount = 10;

// The title and the line under it. The mod keeps the world running in every session match, so the menu never says PAUSED.
inline constexpr const char* kPauseTitle = "MENU";
inline constexpr const char* kPauseTitleNote = "The fight goes on behind this menu. You can still be hit.";
// The sub-pages (PLAYERS, MOD SETTINGS) lead back with the name of where they go.
inline constexpr const char* kPauseBackLabel = "BACK TO MENU";
// The line under the panel when nothing else has anything to say.
inline constexpr const char* kPauseDefaultHint = "Esc closes this.";

// LOADOUT is a trial behind loadout_trial=1 (default 0). The game's inventory screen (UI_Tavern_Inventory_C) was made for
// the Tavern: it looks for the Tavern's camera and dress-up figure, and its exit makes the Tavern's hub menu. The trial
// opens it over a match to see what works in an arena; session.cpp's OpenLoadoutTrial() does it and logs every step.

inline const char* PauseButtonLabel(PauseButton b) {
    switch (b) {
        case PauseButton::Resume: return "RESUME";
        case PauseButton::Players: return "PLAYERS";
        case PauseButton::Loadout: return "LOADOUT";
        case PauseButton::ChangeLoadout: return "CHANGE LOADOUT";
        case PauseButton::GameSettings: return "GAME SETTINGS";
        case PauseButton::ModSettings: return "MOD SETTINGS";
        case PauseButton::ReportBug: return "REPORT A BUG";
        case PauseButton::BackToLobby: return "BACK TO LOBBY";
        case PauseButton::LeaveToMainMenu: return "LEAVE TO MAIN MENU";
        default: return "QUIT TO DESKTOP";
    }
}
// What the label reads after the first click; nullptr: the button acts at once. `host`: this machine hosts the lobby (only
// BACK TO LOBBY reads differently: the host's click ends the match for everyone, a guest's leaves it alone).
inline const char* PauseButtonConfirm(PauseButton b, bool host = true) {
    switch (b) {
        case PauseButton::BackToLobby: return host ? "CLICK AGAIN TO END THE MATCH" : "CLICK AGAIN TO GO BACK";
        case PauseButton::LeaveToMainMenu: return "CLICK AGAIN TO LEAVE";
        case PauseButton::QuitDesktop: return "CLICK AGAIN TO QUIT";
        default: return nullptr;
    }
}
// What the second click does, for the line under the panel while the button waits for it.
inline const char* PauseButtonLeaves(PauseButton b, bool host = true) {
    switch (b) {
        case PauseButton::BackToLobby: return host ? "end the match for everyone" : "go back to the lobby";
        case PauseButton::LeaveToMainMenu: return "leave";
        case PauseButton::QuitDesktop: return "close the game";
        default: return "";
    }
}
// The space above a button in the list: the leaving buttons stand apart from the rest.
inline float PauseButtonGapBefore(PauseButton b) {
    return b == PauseButton::Resume ? 0.f : b == PauseButton::BackToLobby ? 22.f : 8.f;
}
inline std::array<PauseButton, kPauseButtonCount> PauseButtonOrder() {
    return {PauseButton::Resume, PauseButton::Players, PauseButton::Loadout, PauseButton::ChangeLoadout, PauseButton::GameSettings,
            PauseButton::ModSettings, PauseButton::ReportBug, PauseButton::BackToLobby, PauseButton::LeaveToMainMenu, PauseButton::QuitDesktop};
}

// What the main list shows now. The switches: bug reports are on (REPORT A BUG works), loadout_trial=1 (LOADOUT shows), the
// BACK TO LOBBY switches (session/hsm_bench.h BackToLobbyBlock): this machine hosts the lobby (`host`; else it is a guest),
// the Steam lobby is kept (`lobby`), a Crypts run is on (`cryptsRun`: a gather, Apart or Together, goes on to its battle, so
// no lobby comes of a click) and a Progression co-op fight is on (`coop`: dims a guest's button), and the CHANGE LOADOUT view
// (session.h LoadoutView): shown in a match, whether the rule lets it work now, and the rule's reason when it does not.
struct PauseRowIn {
    bool reportOn = false, loadoutTrial = false, host = false, cryptsRun = false, lobby = false, coop = false;
    bool changeShown = false, changeAvailable = false;
    std::string changeWhy;
};
struct PauseRow {
    bool shown = true;
    bool enabled = true;           // false: dimmed; a click does nothing but say `why`
    std::string hint;              // the line under the panel while the button is hovered; empty: nothing of its own
    notices::Msg why;              // what a click on a dimmed button says (text empty: nothing)
};
// BACK TO LOBBY, hovered and working: the host's click ends the match for everyone; a guest's leaves this match alone.
inline constexpr const char* kBackToLobbyHostHint = "Ends the match for everyone. Nobody leaves the lobby.";
inline constexpr const char* kBackToLobbyGuestHint = "Leaves this match. You wait in the lobby for the host's next one.";
inline PauseRow PauseRowOf(PauseButton b, const PauseRowIn& in) {
    PauseRow r;
    switch (b) {
        case PauseButton::Players: r.hint = "Mute, kick or vote, and the network."; break;
        case PauseButton::Loadout:
            r.shown = in.loadoutTrial;
            r.hint = "A trial: it may not work here.";
            break;
        case PauseButton::ChangeLoadout:
            r.shown = in.changeShown;
            r.enabled = in.changeAvailable;
            if (r.enabled) r.hint = "Sends the loadout you edited to the host.";
            else { r.hint = notices::Sentence(in.changeWhy); r.why = notices::LoadoutWhy(in.changeWhy); }
            break;
        case PauseButton::ReportBug:
            r.enabled = in.reportOn;
            if (!r.enabled) { r.hint = "Bug reports are off on this PC."; r.why = notices::Make(notices::Id::ReportsOff); }
            break;
        case PauseButton::BackToLobby: {
            const BackBlock block = BackToLobbyBlock(in.host, in.lobby, in.cryptsRun, in.coop);
            r.enabled = block == BackBlock::None;
            if (r.enabled) r.hint = in.host ? kBackToLobbyHostHint : kBackToLobbyGuestHint;
            else {
                const notices::Id why = block == BackBlock::NoLobby ? notices::Id::LobbyNone
                                      : block == BackBlock::Crypts ? notices::Id::LobbyNotInCrypts : notices::Id::LobbyNotInProgression;
                r.hint = notices::Of(why).text;
                r.why = notices::Make(why);
            }
            break;
        }
        default: break;
    }
    return r;
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
    bool Armed(PauseButton b, uint32_t nowMs) const { return Waiting(nowMs) && which_ == b; }
    // Some button waits for its second click, which one, and for how long yet.
    bool Waiting(uint32_t nowMs) const { return armed_ && uint32_t(nowMs - at_) < kPauseConfirmMs; }
    PauseButton Which() const { return which_; }
    uint32_t LeftMs(uint32_t nowMs) const { return Waiting(nowMs) ? kPauseConfirmMs - uint32_t(nowMs - at_) : 0; }
    void Clear() { armed_ = false; }
private:
    bool armed_ = false;
    PauseButton which_ = PauseButton::Resume;
    uint32_t at_ = 0;
};

// The line under the panel while a button waits for its second click: what that click does and how long it waits (the
// seconds, rounded up, never under 1).
inline std::string PauseArmedHint(PauseButton b, uint32_t leftMs, bool host = true) {
    const uint32_t secs = leftMs ? (leftMs + 999) / 1000 : 1;
    return "Click again within " + std::to_string(secs) + " s to " + PauseButtonLeaves(b, host) + ". Any other click cancels.";
}
// The line under the panel. The pointer on the waiting button: its countdown. On another button that has a hint: the
// hint. Otherwise the waiting button's countdown stays, and without one, the default.
inline std::string PauseHintLine(const PauseConfirm& c, uint32_t nowMs, bool hovering, PauseButton hover, const std::string& hoverHint, bool host = true) {
    if (hovering && c.Armed(hover, nowMs)) return PauseArmedHint(hover, c.LeftMs(nowMs), host);
    if (hovering && !hoverHint.empty()) return hoverHint;
    if (c.Waiting(nowMs)) return PauseArmedHint(c.Which(), c.LeftMs(nowMs), host);
    return kPauseDefaultHint;
}

// MENU LOOK on the MOD SETTINGS page: the look picked, and "(after restart)" until the running game uses it.
inline std::string PauseLookLabel(style::MenuLook look, bool pending) {
    const std::string s = look == style::MenuLook::Classic ? "MENU LOOK: CLASSIC" : look == style::MenuLook::Game ? "MENU LOOK: GAME ART" : "MENU LOOK: DARK";
    return pending ? s + " (after restart)" : s;
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

// QUIT TO DESKTOP leaves the session first. The player's own save is put back once the game is in a menu level again (the
// save guard stops blocking), so the game closes then, or after kQuitDesktopMaxMs at the latest (the next start puts a
// backup back by itself).
constexpr uint32_t kQuitDesktopMaxMs = 8000;
inline bool QuitDesktopDue(bool saveGuardBlocking, uint32_t waitedMs) { return !saveGuardBlocking || waitedMs >= kQuitDesktopMaxMs; }

}  // namespace hsm

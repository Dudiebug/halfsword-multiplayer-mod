// The mod's pause panel (mod_pause=1): in a multiplayer session match Esc opens this instead of the game's pause menu. It is
// built into the game's UI_HUD widget tree, as the chat and the build card are, and shows the buttons of
// match/hsm_pause_panel.h: RESUME, MULTIPLAYER MENU, SETTINGS, MOD SETTINGS, REPORT A BUG, LEAVE MATCH, QUIT TO MAIN MENU,
// QUIT TO DESKTOP (and LOADOUT once it is switched on). MULTIPLAYER MENU and MOD SETTINGS are pages of the panel itself;
// every other button is reported to the session (gameplay/session.cpp ModPauseTick), which carries it out. Esc ownership
// is match/hsm_input_mode.h. Game thread only.
#pragma once
#include "match/hsm_pause_panel.h"
#include <optional>

namespace hsm {
class Session;
namespace ui {

struct PausePanelWant {
    bool match = false;      // a session match with mod_pause=1: keep the panel built on the HUD, ready for Esc
    bool open = false;       // the player has the panel open
    bool hidden = false;     // open, but one of the game's screens (SETTINGS) is in front: the panel waits out of sight
    bool reportOn = false;   // bug reports are on and this is a session fight: REPORT A BUG, HELP IMPROVE THE MOD
};

// Once per frame. Builds the panel on the UI_HUD that is on screen (again when the game swaps the HUD), shows or hides it,
// runs its pages and returns the button the player chose this frame (once per click, after the second click where the
// button needs one). The pages (MULTIPLAYER MENU, MOD SETTINGS) are handled inside and report nothing.
std::optional<PauseButton> PausePanelTick(Session& session, const PausePanelWant& want);

// The panel is built on the UI_HUD it found. onScreen: and that UI_HUD is in the viewport right now (one engine call; asked when
// Esc is pressed, since only then may the game's own Esc be taken from it).
bool PausePanelReady(bool onScreen = false);

}  // namespace ui
}  // namespace hsm

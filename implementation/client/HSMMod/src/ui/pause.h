// A REPORT A BUG button on the game's own pause menu (UI_Pause_Eng), under its last button, built as real UMG widgets in
// the menu's widget tree (same kit as the main menu's MULTIPLAYER entry). A click is reported to the caller, which closes
// the menu and opens the chat with "/bug " typed. Game thread only.
#pragma once
#include "ue/engine.h"

namespace hsm::ui {

// Per call (the pause menu's own Tick calls it too, as the world may be paused then and the session tick stops): `menu`
// is the pause menu widget on screen (nullptr: none), `show` whether the button belongs there. The button is added once
// per menu instance; the structure of the menu is logged once per run. True once per click.
bool PauseReportTick(ue::UObject* menu, bool show);

}  // namespace hsm::ui

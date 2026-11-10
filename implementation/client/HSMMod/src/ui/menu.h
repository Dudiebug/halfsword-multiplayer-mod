// In-game Multiplayer entry: a button injected on the main menu (UI_Startup_Menu, or UI_MainMenuSNF in older builds)
// that opens a full-screen multiplayer page built from real UMG widgets at runtime (lobby, match settings, map list).
// Game thread only.
#pragma once
#include "ue/engine.h"

namespace hsm {
class Session;
namespace ui {
void Tick(Session& session, ue::UObject* worldContext);
void LeaveMenus();   // fight levels: close the multiplayer page, take a leftover main menu off the screen
bool PanelOpen();    // the multiplayer page is on screen (its main menu is alive and in the viewport)
// The page's bug report box has the keyboard. Not part of ui::Typing (the game's hotkeys stay as they were there); it
// goes on the Fetch link page as typing, as the other boxes do.
bool MenuTyping();
}
}  // namespace hsm

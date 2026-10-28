// The game's own pause menu (UI_Pause_Eng) in a session. In a multiplayer match, Esc opens the multiplayer Esc menu
// instead (ui/mp_widgets.h: players with mute, volume, kick and votekick, voice, network, RESUME, GAME MENU, REPORT A
// BUG, LEAVE SESSION), a panel built over the stock menu in its own widget tree; GAME MENU shows the stock menu, which
// gets a MULTIPLAYER MENU button under its last button to come back. Without a session (or when the panel cannot be
// built) the stock menu gets REPORT A BUG there instead. Game thread only.
#pragma once
#include "ue/engine.h"
#include <cstdint>

namespace hsm {
class Session;
namespace ui {

enum class PauseAction : uint8_t { None, Resume, ReportBug, Leave };

// Per call: `menu` is the pause menu widget on screen (nullptr: none); `session` the session when this is a multiplayer
// match (nullptr: the stock menu stays); `reportButton` whether REPORT A BUG belongs there. Widgets are added once per
// menu instance; the structure of the menu is logged once per run. Returns what the player chose, once per click.
PauseAction PauseMenuTick(ue::UObject* menu, Session* session, bool reportButton);

}  // namespace ui
}  // namespace hsm

// In-game text chat overlay: recent lines bottom-left and a typing line, added as real UMG text into the game's own
// UI_HUD widget (same mechanism as hud.cpp). T opens typing while the game window is foreground; Enter sends,
// Esc cancels, Backspace deletes. Game thread only.
#pragma once
#include "ui/hud.h"
#include <optional>
#include <string>

namespace hsm::ui {

// Draws the overlay and polls keys. Takes the same model HudTick takes; typing and the overlay only run while
// model.visible is true (the match HUD condition). Call once per frame alongside HudTick.
void ChatTick(const HudModel& model);

// The next line the local player sent (sanitized, non-empty), or nothing. Returns at most one line per call.
std::optional<std::string> TakeOutgoing();

// Adds a line to the overlay (name is shown as "name: text"). Keeps the last 8 lines, each for 10 s.
void ShowLine(const std::string& name, const std::string& text);

// True while the player is composing a line.
bool Typing();

}  // namespace hsm::ui

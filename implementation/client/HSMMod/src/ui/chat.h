// In-game text chat overlay: recent lines bottom-left in a smoke panel and an input row, added as real UMG widgets into
// the game's own UI_HUD widget (same mechanism as hud.cpp); what shows, and how faded, is the shared ChatScene's
// decision (MATCH_ROUNDS_UI_CHAT_PLAN 3.3). The chat key (Enter by default, HSMMod.ini chat_key) opens typing while
// the game window is foreground; Enter sends, Esc cancels, Backspace deletes. While a line is typed the keyboard is the
// chat's (hsm::ChatSession; the capture callback takes it from the game and gives it back). Game thread only.
#pragma once
#include "ui/hud.h"
#include "hsm_chat.h"
#include "match/hsm_chat_scene.h"
#include <functional>
#include <optional>
#include <string>

namespace hsm::ui {

// The chat key; capture(on) takes the keyboard from the game (true) or gives it back (Adapter::CaptureInput); paused()
// tells whether the game is paused (no chat then: asked only at the open key and while typing). Before the first ChatTick.
void ChatSetup(ChatKey key, std::function<void(bool on)> capture, std::function<bool()> paused);

// Draws the overlay and polls keys. Takes the same model HudTick takes; typing and the overlay only run while
// model.visible is true (the match HUD condition). Call once per frame alongside HudTick.
void ChatTick(const HudScene& model);

// The next line the local player sent (sanitized, non-empty), or nothing. Returns at most one line per call.
std::optional<std::string> TakeOutgoing();

// Adds a line to the overlay, the name in `ink` (ChatNameInk). Keeps the last 8 lines; each shows for 10 s, and all
// of them while typing.
void ShowLine(const std::string& name, Rgba ink, const std::string& text);

// True while the player is composing a line.
bool Typing();

// Drops a line being typed and gives the keyboard back, as when the HUD hides (`why` goes in the log line): the level
// changed, the match ended, the session is gone. Nothing when not typing.
void ChatDrop(const char* why);

}  // namespace hsm::ui

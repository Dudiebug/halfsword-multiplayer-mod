// Which input mode the local player's controller gets, as the pause menu, the chat and the respawn fixes interact.
// Game-independent and unit-tested; the client's gameplay layer applies the answer.
//
// The stock pause menu needs UI input (a visible cursor, clicks) to be usable. Our "restore game input" calls
// (a respawn, the fix after a level load, the chat closing) are game-only with the cursor hidden, so while the menu is
// open they must not run; the menu's own mode is set once when it opens and the game's is restored once when it closes.
#pragma once
#include <cstdint>

namespace hsm {

enum class InputMode : uint8_t {
    Leave,        // change nothing
    GameOnly,     // game-only input, cursor hidden (Adapter::RestoreGameInput)
    GameAndUI,    // game and UI input, cursor shown (Adapter::ShowPauseCursor)
};

struct InputModeState {
    bool typing = false;            // the chat holds the keyboard: its UI-only mode stays
    bool menuOpen = false;          // the stock pause menu is on screen now
    bool wasMenuOpen = false;       // ... as of the previous decision
    bool respawnHappened = false;   // a new body was just possessed
    bool delayedFixDue = false;     // the 1.5 s / 4.5 s after-load input fix is due
};

inline InputMode ChooseInputMode(const InputModeState& s) {
    if (s.typing) return InputMode::Leave;
    if (s.menuOpen) return s.wasMenuOpen ? InputMode::Leave : InputMode::GameAndUI;
    if (s.wasMenuOpen || s.respawnHappened || s.delayedFixDue) return InputMode::GameOnly;
    return InputMode::Leave;
}

}  // namespace hsm

// Which input mode the local player's controller gets, as the pause menu, the chat and the respawn fixes interact.
// Game-independent and unit-tested; the client's gameplay layer applies the answer.
//
// The stock pause menu needs UI input (a visible cursor, clicks) to be usable. Our "restore game input" calls
// (a respawn, the fix after a level load, the chat closing) are game-only with the cursor hidden, so while the menu is
// open they must not run; the menu's own mode is set once when it opens and the game's is restored once when it closes.
#pragma once
#include <cstdint>
#include <string_view>

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
    bool fightLevel = true;         // the loaded level is a fight: the menus and lobby set their own input mode
};

inline InputMode ChooseInputMode(const InputModeState& s) {
    if (s.typing) return InputMode::Leave;
    if (!s.fightLevel) return InputMode::Leave;   // a stock Quit to menu loads the menu level with the pause menu just gone: not ours to restore
    if (s.menuOpen) return s.wasMenuOpen ? InputMode::Leave : InputMode::GameAndUI;
    if (s.wasMenuOpen || s.respawnHappened || s.delayedFixDue) return InputMode::GameOnly;
    return InputMode::Leave;
}

// The stock pause menu (UI_Pause_Eng) and the settings screens it opens: its SETTINGS button adds UI_DisplaySettings2
// and removes itself, whose tabs open UI_GameSettings, UI_AudioSettings, UI_CameraSettings, ... (decomp
// UI_Pause_Eng.cpp LatentResume_Lf). While a settings screen is up the menu counts as open, or closing it would hand
// the mouse back to the game under the settings (HSMBot 2752, 2774). Matched by Blueprint class name.
enum class PauseScreen : uint8_t { None, Menu, Settings };
inline PauseScreen PauseScreenOf(std::string_view cls) {
    if (cls.find("UI_Pause") != std::string_view::npos) return PauseScreen::Menu;
    if (cls.rfind("UI_", 0) != 0) return PauseScreen::None;
    for (std::string_view s : {"Settings", "UI_Controls", "UI_KeyBinds"})
        if (cls.find(s) != std::string_view::npos) return PauseScreen::Settings;
    return PauseScreen::None;
}

}  // namespace hsm

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

// Who owns the Esc key in a multiplayer session match (mod_pause=1). The game's own Esc pause toggle is blocked there, so
// the key is the mod's: it opens the mod's pause panel, unless something else on screen already uses Esc. The caller fills
// the context (the busy flags OR'd with their value one tick earlier, so the press that closes the chat or a screen does
// not also open the panel) and acts on the answer. Outside a session match, or with mod_pause=0, nothing changes: the
// game's own Esc applies and the mod does not log.
struct EscContext {
    bool sessionMatch = false;     // joined a session and on a match level (the game's own pause toggle is blocked there)
    bool modPause = true;          // mod_pause=1
    bool chatOpen = false;         // a chat line is being typed: Esc cancels it
    bool textFocused = false;      // some other text box has the keyboard (report box, lobby password): Esc leaves it
    bool fetchMenu = false;        // Fetch's own menu is up and uses Esc
    bool lobbyPage = false;        // the mod's multiplayer lobby page is open: Esc closes that
    bool settingsScreen = false;   // one of the game's settings screens is up (opened from the panel): Esc goes back
    bool panelOpen = false;        // the mod's pause panel is open
};
enum class EscOwner : uint8_t {
    Game,        // not ours: the game's own Esc handling applies
    Chat,        // the chat closes itself
    TextBox,     // a text box lets go of the keyboard
    Fetch,       // Fetch's menu closes itself
    Lobby,       // the multiplayer page closes itself
    Settings,    // the game's settings screen goes back (to the pause class, which the Construct hook swallows)
    ClosePanel,  // the pause panel closes: resume
    OpenPanel,   // the pause panel opens
};
inline EscOwner EscOwnerOf(const EscContext& c) {
    if (!c.sessionMatch || !c.modPause) return EscOwner::Game;
    if (c.chatOpen) return EscOwner::Chat;
    if (c.textFocused) return EscOwner::TextBox;
    if (c.fetchMenu) return EscOwner::Fetch;
    if (c.lobbyPage) return EscOwner::Lobby;
    if (c.settingsScreen) return EscOwner::Settings;
    if (c.panelOpen) return EscOwner::ClosePanel;
    return EscOwner::OpenPanel;
}
// The log line for one Esc press, "pause: Esc -> <this>". Game has no line: the mod does nothing then.
inline const char* EscOwnerText(EscOwner o) {
    switch (o) {
        case EscOwner::Chat: return "chat (it closes)";
        case EscOwner::TextBox: return "text box (it lets go)";
        case EscOwner::Fetch: return "Fetch's menu (it closes)";
        case EscOwner::Lobby: return "multiplayer page (it closes)";
        case EscOwner::Settings: return "game settings screen (it goes back)";
        case EscOwner::ClosePanel: return "mod panel closes";
        case EscOwner::OpenPanel: return "mod panel";
        default: return "";
    }
}

// One Esc press, seen two ways: the key going down (read from the OS, so it works with the game's own Esc blocked) and the
// game's blocked pause event (which also comes from P and the gamepad's Start). The key and the event of one press land a
// frame or two apart; kEscGapMs merges them. A held key counts once, and a press right after another (for example the one
// that came back from the game's settings screen) is not taken for a second.
constexpr uint32_t kEscGapMs = 250;
class EscPress {
public:
    bool Step(bool keyDown, bool pauseEvent, uint32_t nowMs) {
        const bool edge = keyDown && !keyWas_;
        keyWas_ = keyDown;
        if (!edge && !pauseEvent) return false;
        if (started_ && uint32_t(nowMs - last_) < kEscGapMs) return false;
        started_ = true;
        last_ = nowMs;
        return true;
    }
private:
    bool keyWas_ = false, started_ = false;
    uint32_t last_ = 0;
};

}  // namespace hsm

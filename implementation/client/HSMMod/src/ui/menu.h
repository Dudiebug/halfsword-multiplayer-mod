// In-game Multiplayer entry: a button injected on the main menu (UI_Startup_Menu, or UI_MainMenuSNF in older builds)
// that opens a full-screen multiplayer page built from real UMG widgets at runtime (lobby, match settings, map list).
// Game thread only.
#pragma once
#include "ue/engine.h"

namespace hsm {
class Session;
namespace ui {
void Tick(Session& session, ue::UObject* worldContext);
}
}  // namespace hsm

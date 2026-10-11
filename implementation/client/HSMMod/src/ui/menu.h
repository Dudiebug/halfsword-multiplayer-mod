// In-game Multiplayer entry: a button injected next to Free Mode on the main menu (UI_MainMenuSNF) and a
// lobby panel built from real UMG widgets at runtime. Game thread only.
#pragma once
#include "ue/engine.h"

namespace hsm {
class Session;
namespace ui {
void Tick(Session& session, ue::UObject* worldContext);
}
}  // namespace hsm

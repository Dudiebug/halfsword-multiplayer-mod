// Which main-menu widget instance the Multiplayer button belongs on (ui/menu.cpp).
//
// The game's settings screens remove the main menu, and their Back button creates a new one (UI_GameSettings.cpp:123,
// 345). The removed instance stays alive until garbage collection, so the button follows whichever instance is in the
// viewport: the held one while it is shown, else the first shown one found, else none (wait).
#pragma once
#include <vector>

namespace hsm {

struct MenuCandidate { const void* id = nullptr; bool inViewport = false; };

// held: the instance the button is on now (null if none). Returns the instance to use, or null to wait.
inline const void* PickMenu(const void* held, bool heldInViewport, const std::vector<MenuCandidate>& live) {
    if (held && heldInViewport) return held;
    for (const MenuCandidate& c : live) if (c.id && c.inViewport) return c.id;
    return nullptr;
}

}  // namespace hsm

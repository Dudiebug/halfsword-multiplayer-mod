// In-fight match HUD: score line, respawn countdown, kill feed and a Tab scoreboard, added as real UMG text
// into the game's own UI_HUD widget so it looks native. Game thread only.
#pragma once
#include "ue/engine.h"
#include <string>

namespace hsm::ui {

struct HudModel {
    bool visible = false;
    std::string status, respawn, feed, scoreboard;
};

void HudTick(const HudModel& model);

}  // namespace hsm::ui

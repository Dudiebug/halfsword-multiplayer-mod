// In-fight match HUD (MATCH_ROUNDS_UI_CHAT_PLAN 3.2): the score strip, time bar and phase pill, centre banners, the
// dead panel, the kill feed and the Tab scoreboard, built as real UMG widgets inside the game's own UI_HUD so they
// sit in its layout and font. What shows, and how, is the shared HudScene's decision; this only maps it to widgets.
// Game thread only.
#pragma once
#include "match/hsm_hud_scene.h"
#include "match/hsm_nametag.h"
#include <vector>

namespace hsm::ui {

// tags: name tags to draw over other fighters this frame (empty: none); format picks team ink.
void HudTick(const HudScene& scene, const std::vector<NameTagSrc>& tags = {}, Format format = Format::Duel);
bool HudTabHeld();   // the scoreboard key (Tab), while the game window has focus

}  // namespace hsm::ui

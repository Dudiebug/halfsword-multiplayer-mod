// The game's own look for the mod's widgets: which of Half Sword's UI textures and typefaces each kind of panel and
// text takes (its parchment, gilt-framed buttons, scrolls and blackletter). The art is the game's, found by name while
// the engine has it loaded (ui::GameTexture); when it is not, a panel is drawn in the plain colour of the palette
// (shared/match/hsm_hud_style.h) that stands for it, so nothing ever goes missing. Game thread only.
#pragma once
#include "ui/umg.h"
#include "match/hsm_hud_style.h"
#include <cstdint>
#include <string>

namespace hsm::ui::skin {

enum class Art : uint8_t {
    Backdrop,     // behind a full-screen page: the game's tavern painting, darkened
    Scroll,       // a column, a list, the scoreboard: a parchment scroll with its rods
    Sheet,        // a header or footer band, the Esc menu: old paper with burnt edges
    Note,         // a question over the page (the modals): the tavern's question scroll
    Card,         // a button or a pickable card: parchment in a gilt frame
    CardPicked,   // the chosen one: the game's amber pressed button
    CardRed,      // START: the game's red button
    Ribbon,       // a banner across the screen (the HUD's round banners)
    Strip,        // a kill feed line: a torn parchment strip
    Plate,        // a small HUD plate (score, pill, feed, chat input): the button art with a finer frame
    PlatePicked,  // the same in the amber pressed button (our own kill feed lines)
    Count
};

// The panel draws `art` (tinted by `tint`), or its plain colour: in the classic look (style::ClassicMenus, ui_style in
// HSMMod.ini) always, otherwise when the game has not loaded it. Loaded: the art is what is drawn.
void Paint(Panel& p, Art art, Rgba tint = {1, 1, 1, 1});
bool Loaded(Art art);
// The panel's padding: `room` inside the art's frame, rods or burnt edges (as drawn), or `room` alone on a plain fill.
// The frame is measured from the texture, so content never sits on it whatever size the panel is.
void Pad(Panel& p, Art art, Margin room);

enum class Face : uint8_t { Body, Heading };   // the game's rounded gothic (its menus' text), its blackletter (its titles)
void Font(Text& t, Face face);   // the classic look keeps the kit's copied font

// A heading as the look writes it (style::MenuTitle): "HOST A LOBBY" as "Host A Lobby" in the game look, capitals in the
// classic one. Write the text in capitals here: the launcher's generator copies the warning titles from these calls.
std::string Title(const std::string& upper);

// The faded ink of a parchment card's tint: normal, under the mouse (glow 0..1).
Rgba CardTint(float glow);

}  // namespace hsm::ui::skin

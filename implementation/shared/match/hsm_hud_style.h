// HUD palette and the contrast arithmetic behind it (MATCH_ROUNDS_UI_CHAT_PLAN 3.1). Colours are linear
// FLinearColor values. Text is judged by WCAG contrast against the smoke panel laid over a mid-grey scene.
// Game-independent.
#pragma once
#include "match/hsm_match.h"
#include <algorithm>
#include <string>

namespace hsm::style {

inline constexpr Rgba kSmoke{0.02f, 0.017f, 0.014f, 0.82f};      // panel fill
inline constexpr Rgba kBone{0.93f, 0.89f, 0.80f, 1};             // body text
inline constexpr Rgba kBlood{0.70f, 0.05f, 0.04f, 1};            // accent: frames, bars, swatches (not text)
inline constexpr Rgba kBloodInk{0.95f, 0.32f, 0.26f, 1};         // the accent as text
inline constexpr Rgba kIron{0.45f, 0.47f, 0.50f, 1};             // secondary text, frames
inline constexpr Rgba kOk{0.35f, 0.65f, 0.25f, 1};
inline constexpr Rgba kWarn{0.85f, 0.62f, 0.05f, 1};
inline constexpr Rgba kInfectedInk{0.45f, 0.78f, 0.28f, 1};      // Infection: the infected, as text
inline constexpr Rgba kSceneGrey{0.18f, 0.18f, 0.18f, 1};        // the reference scene behind a panel

// The menus (multiplayer page, Esc panel) and the HUD's panels. Three looks, chosen once at start (ui_style in
// HSMMod.ini, UseMenuLook): the classic one (the default, the look before 0.8.0-pre43, "Iron & Blood": opaque dark
// faces, light text), the game's own (faces are its parchment and gilt art, tinted, and these are the plain fills drawn
// when the art is not loaded, and the faces text is judged on; text is dark ink, as on its scrolls) and dark (the game's
// art shaded dark and almost opaque, light text; a menu page sits on a darkened painting). Set before any widget is made; read only on the game thread
// after that.
struct MenuPalette {
    Rgba bg, backdrop, panel, card, cardHover, frame, frameHover, text, dim, accent, accentInk, ok, warn, light;
};
inline constexpr MenuPalette kGameMenus{
    {0.020f, 0.012f, 0.007f, 0.97f},   // bg: behind everything, dark wood
    {0.30f, 0.26f, 0.22f, 1},          // backdrop: the tint over the game's tavern painting behind the page
    {0.62f, 0.55f, 0.43f, 1},          // panel: a column's scroll
    {0.70f, 0.63f, 0.50f, 1},          // card: a button's parchment
    {0.80f, 0.72f, 0.57f, 1},          // cardHover
    {0.16f, 0.09f, 0.035f, 1},         // frame: brown ink, a thin rule, a plain card's frame
    {0.36f, 0.20f, 0.05f, 1},          // frameHover: gilt
    {0.010f, 0.007f, 0.005f, 1},       // text: ink
    {0.085f, 0.060f, 0.040f, 1},       // dim: labels, hints, faded ink
    {0.30f, 0.010f, 0.008f, 1},        // accent: rubric red, START, the selected frame, rules
    {0.30f, 0.008f, 0.005f, 1},        // accentInk: the rubric as text, tabs, section heads
    {0.018f, 0.11f, 0.012f, 1},        // ok
    {0.19f, 0.055f, 0.002f, 1},        // warn
    {0.80f, 0.72f, 0.58f, 1},          // light: text on the dark backdrop and on START's red
};
inline constexpr MenuPalette kDarkMenus{
    {0.010f, 0.009f, 0.008f, 1},          // bg: behind a page, opaque (the game's own menu never shows through)
    {1, 1, 1, 1},                         // backdrop: the tavern painting, shaded dark by the skin
    {0.012f, 0.010f, 0.008f, 0.92f},      // panel: a column (the plain fill when the art is not loaded)
    {0.020f, 0.017f, 0.014f, 0.94f},      // card
    {0.045f, 0.037f, 0.030f, 0.95f},      // cardHover
    {0.0423f, 0.0343f, 0.0273f, 1},       // frame
    {0.30f, 0.20f, 0.08f, 1},             // frameHover: dull gilt
    {0.8148f, 0.7529f, 0.6514f, 1},       // text: bone
    {0.64f, 0.58f, 0.48f, 1},             // dim: labels, hints
    {0.42f, 0.030f, 0.020f, 1},           // accent: START, the selected frame, rules
    {1.00f, 0.47f, 0.37f, 1},             // accentInk: the accent as text, tabs, section heads
    {0.35f, 0.80f, 0.35f, 1},             // ok
    {0.85f, 0.55f, 0.08f, 1},             // warn
    {0.8148f, 0.7529f, 0.6514f, 1},       // light: START's label, as the rest of the text
};
inline constexpr MenuPalette kClassicMenus{
    {0.0040f, 0.0037f, 0.0033f, 0.97f},   // bg: the page behind everything
    {0.0040f, 0.0037f, 0.0033f, 0.97f},   // backdrop: the same page
    {0.0086f, 0.0075f, 0.0065f, 1},       // panel: a column's panel
    {0.0137f, 0.0116f, 0.0097f, 1},       // card: a card or button face
    {0.0232f, 0.0194f, 0.0160f, 1},       // cardHover
    {0.0423f, 0.0343f, 0.0273f, 1},       // frame: a card's 2 px frame
    {0.1470f, 0.1195f, 0.0953f, 1},       // frameHover
    {0.8148f, 0.7529f, 0.6514f, 1},       // text
    {0.3663f, 0.3231f, 0.2623f, 1},       // dim: labels, hints
    {0.3419f, 0.0110f, 0.0110f, 1},       // accent: START, the selected frame, the header rule
    {0.8070f, 0.1441f, 0.1022f, 1},       // accentInk: the accent as text, tabs, section labels, the pill
    {0.1590f, 0.5210f, 0.1714f, 1},       // ok
    {0.7454f, 0.4342f, 0.0648f, 1},       // warn
    {0.8148f, 0.7529f, 0.6514f, 1},       // light: START's label, as the rest of the text
};

enum class MenuLook : uint8_t { Dark, Game, Classic };
inline MenuLook gMenuLook = MenuLook::Classic;
inline Rgba kMenuBg = kClassicMenus.bg, kMenuBackdrop = kClassicMenus.backdrop, kMenuPanel = kClassicMenus.panel,
            kMenuCard = kClassicMenus.card, kMenuCardHover = kClassicMenus.cardHover, kMenuFrame = kClassicMenus.frame,
            kMenuFrameHover = kClassicMenus.frameHover, kMenuText = kClassicMenus.text, kMenuDim = kClassicMenus.dim,
            kMenuAccent = kClassicMenus.accent, kMenuAccentInk = kClassicMenus.accentInk, kMenuOk = kClassicMenus.ok,
            kMenuWarn = kClassicMenus.warn, kMenuLight = kClassicMenus.light;

inline const MenuPalette& PaletteOf(MenuLook look) {
    return look == MenuLook::Classic ? kClassicMenus : look == MenuLook::Game ? kGameMenus : kDarkMenus;
}
inline void UseMenuLook(MenuLook look) {
    const MenuPalette& m = PaletteOf(look);
    gMenuLook = look;
    kMenuBg = m.bg; kMenuBackdrop = m.backdrop; kMenuPanel = m.panel; kMenuCard = m.card; kMenuCardHover = m.cardHover;
    kMenuFrame = m.frame; kMenuFrameHover = m.frameHover; kMenuText = m.text; kMenuDim = m.dim; kMenuAccent = m.accent;
    kMenuAccentInk = m.accentInk; kMenuOk = m.ok; kMenuWarn = m.warn; kMenuLight = m.light;
}
inline MenuLook CurrentMenuLook() { return gMenuLook; }
inline bool ClassicMenus() { return gMenuLook == MenuLook::Classic; }
inline bool DarkMenus() { return gMenuLook == MenuLook::Dark; }
inline bool ParchmentMenus() { return gMenuLook == MenuLook::Game; }   // dark ink on light parchment

// ui_style in HSMMod.ini at start (`value`; "classic" when the line is missing). Classic is the default. The HSMMod.ini
// that 0.8.0-pre43 to pre49 wrote says ui_style=game whether or not the player picked it, so the first start with this
// build reads "game" as classic once and has it written back (`moveToClassic`, with ui_style_moved=1). `moved`: that
// marker is in the file, so a "game" there is the player's own pick and is kept. Anything unknown is classic.
struct StartLook {
    MenuLook look;
    bool known;           // false: the value is none of classic, game or dark
    bool moveToClassic;   // write ui_style=classic and ui_style_moved=1 back
};
inline StartLook ReadStartLook(const std::string& value, bool moved) {
    if (value == "dark") return {MenuLook::Dark, true, false};
    if (value == "game") return moved ? StartLook{MenuLook::Game, true, false} : StartLook{MenuLook::Classic, true, true};
    return {MenuLook::Classic, value == "classic", false};
}
// The SETTINGS button's order, starting at the default: classic, game art, dark, classic.
inline MenuLook NextMenuLook(MenuLook look) {
    return look == MenuLook::Classic ? MenuLook::Game : look == MenuLook::Game ? MenuLook::Dark : MenuLook::Classic;
}

// hud_opacity in HSMMod.ini: the render opacity of the match HUD, the chat and the fighter card, for a less harsh and
// less distracting overlay (HSMBot 2801). kHudOpacityMin..1; below it is kHudOpacityMin, above 1 or not a number is 1.
constexpr float kHudOpacityMin = 0.3f;
inline float SaneHudOpacity(double raw) {
    if (!(raw == raw) || raw > 1.0) return 1.f;
    return raw < kHudOpacityMin ? kHudOpacityMin : float(raw);
}
inline float gHudOpacity = 1.f;
inline void UseHudOpacity(double raw) { gHudOpacity = SaneHudOpacity(raw); }
inline float HudOpacity() { return gHudOpacity; }

// A menu heading as the look writes it. Classic: capitals. The game look: title case ("HOST A LOBBY" as "Host A Lobby"),
// as the game writes its own ("Paused", "Options"): blackletter capitals run together. Only capitals inside a word are
// lowered, so "Help Improve the Mod?" keeps its small "the". The word "HS" (HS Enhancer) is a name and keeps both capitals.
inline std::string MenuTitle(std::string s) {
    const auto letter = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    if (ClassicMenus()) {
        for (char& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        return s;
    }
    bool start = true;                              // the next letter starts a word
    for (size_t i = 0; i < s.size(); ++i) {
        if (!letter(s[i])) { start = s[i] != '\''; continue; }   // a new word after a space or a mark, not inside "PLAYER'S"
        if (start && s.compare(i, 2, "HS") == 0 && (i + 2 == s.size() || !letter(s[i + 2]))) ++i;
        else if (!start && s[i] >= 'A' && s[i] <= 'Z') s[i] = char(s[i] - 'A' + 'a');
        start = false;
    }
    return s;
}

// Relative luminance of a linear colour (alpha ignored).
inline double Luminance(Rgba c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; }

// WCAG contrast ratio, 1..21, either order.
inline double Contrast(Rgba a, Rgba b) {
    double la = Luminance(a), lb = Luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// `top` composited over `under` (straight alpha).
inline Rgba Over(Rgba top, Rgba under) {
    float a = top.a + under.a * (1 - top.a);
    if (a <= 0) return {};
    auto mix = [&](float t, float u) { return (t * top.a + u * under.a * (1 - top.a)) / a; };
    return {mix(top.r, under.r), mix(top.g, under.g), mix(top.b, under.b), a};
}

inline Rgba PanelOverScene() { return Over(kSmoke, kSceneGrey); }

// Contrast of text in colour `c` (its alpha included) on the panel over the reference scene.
inline double TextContrast(Rgba c) { Rgba p = PanelOverScene(); return Contrast(Over(c, p), p); }

inline Rgba Faded(Rgba c, float opacity) { return {c.r, c.g, c.b, c.a * opacity}; }

// A light ink (made for the dark HUD panels) as ink on parchment: the same hue, darkened until it reads on the menu
// faces. Dark colours stay as they are, and so does everything in the dark and classic looks (their faces are dark).
inline Rgba OnParchment(Rgba c) {
    if (!ParchmentMenus()) return c;
    constexpr double kMaxLuminance = 0.06;
    const double l = Luminance(c);
    if (l <= kMaxLuminance) return c;
    const float k = float(kMaxLuminance / l);
    return {c.r * k, c.g * k, c.b * k, c.a};
}

// Team colour for text: TeamColor's tabard tints are too dark to read, so each team has a lighter ink of
// the same hue. No team (free-for-all): bone.
inline Rgba TeamInk(int team) {
    static constexpr Rgba kInk[] = {kBloodInk, {0.40f, 0.55f, 1.0f, 1}, {0.30f, 0.80f, 0.30f, 1}, kWarn};
    return team >= 0 && team < 4 ? kInk[team] : kBone;
}

// The ink of a side in a match of format `f`: Infection's survivors keep bone and its infected are sickly green; the rest are TeamInk.
inline Rgba TeamInkFor(Format f, int team) {
    if (IsInfection(f)) return team == kInfectedTeam ? kInfectedInk : kBone;
    return TeamInk(team);
}

}  // namespace hsm::style

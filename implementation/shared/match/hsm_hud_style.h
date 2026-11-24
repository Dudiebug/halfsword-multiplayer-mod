// HUD palette and the contrast arithmetic behind it (MATCH_ROUNDS_UI_CHAT_PLAN 3.1). Colours are linear
// FLinearColor values. Text is judged by WCAG contrast against the smoke panel laid over a mid-grey scene.
// Game-independent.
#pragma once
#include "match/hsm_dropdown.h"
#include "match/hsm_match.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace hsm::style {

inline constexpr Rgba kSmoke{0.02f, 0.017f, 0.014f, 0.82f};      // panel fill
inline constexpr Rgba kSmokeDense{0.02f, 0.017f, 0.014f, 0.92f};  // the same smoke, denser: the chat's input line
// Text with no panel behind it (the banner sub, the chat hint, the name tags) has the dark outline and, under it, this drop
// shadow, kShadowPx to the right and down: it stays readable on a bright map. One outline for every text that has one.
inline constexpr Rgba kOutlineInk{0, 0, 0, 0.9f};
inline constexpr Rgba kShadowInk{0, 0, 0, 0.75f};
inline constexpr float kShadowPx = 1.5f;
inline constexpr Rgba kBone{0.93f, 0.89f, 0.80f, 1};             // body text
inline constexpr Rgba kBlood{0.70f, 0.05f, 0.04f, 1};            // accent: frames, bars, swatches (not text)
inline constexpr Rgba kBloodInk{0.9647f, 0.3763f, 0.3095f, 1};  // the accent as text (sRGB #fba597; was 0.95 0.32 0.26: 4.1:1 over white)
inline constexpr Rgba kIron{0.4851f, 0.5029f, 0.5395f, 1};       // secondary text, frames (sRGB #b9bcc2; was 0.45 0.47 0.50: 4.3:1 over white)
inline constexpr Rgba kOk{0.35f, 0.65f, 0.25f, 1};
inline constexpr Rgba kWarn{0.85f, 0.62f, 0.05f, 1};
inline constexpr Rgba kInfectedInk{0.45f, 0.78f, 0.28f, 1};      // Infection: the infected, as text
inline constexpr Rgba kSceneGrey{0.18f, 0.18f, 0.18f, 1};        // the reference scene behind a panel
inline constexpr Rgba kSceneWhite{1, 1, 1, 1};                    // the worst case: a bright map (snow, sky) behind a panel

// The game's pressed-button amber: a picked card or plate in the game look (the plain fill that stands for its art), and in
// the dark look the same, shaded. The shading is what the skin multiplies the art's colour by (kDarkKeep*).
inline constexpr Rgba kAmber{0.85f, 0.45f, 0.03f, 1};
inline constexpr float kDarkKeepPicked = 0.55f, kDarkKeepPlate = 0.50f;
constexpr Rgba Keep(Rgba c, float k) { return {c.r * k, c.g * k, c.b * k, c.a}; }
// Your own kill-feed line in the classic look: the smoke, reddened.
inline constexpr Rgba kPlateMineClassic{0.22f, 0.025f, 0.02f, 0.80f};
// What the skin multiplies the art by: a card at rest and under the mouse, and START at rest.
inline constexpr Rgba kCardTint{0.86f, 0.84f, 0.80f, 1}, kCardTintHover{1, 0.97f, 0.88f, 1}, kStartTint{0.85f, 0.85f, 0.85f, 1};

// Disabled controls: the whole control drawn at this opacity, everywhere (buttons, steppers, rows). 0.55 is the lowest of
// the values the page used (0.3, 0.4, 0.45, 0.55) at which the label still reads 3:1 in all three looks.
inline constexpr float kDisabledAlpha = 0.55f;
// WCAG: body text 4.5:1, disabled text 3:1.
inline constexpr double kMinTextContrast = 4.5, kMinDisabledContrast = 3.0;

// The type scale (UMG points), by role. Every size the menus and the HUD draw is one of these (or a HUD display size
// below): hint 14 (notes under a control, legends), body 15 (notes, card text), row 17 (a row's name, a stepper),
// emphasis 20 (buttons, kill feed, chat), section 23 (a rubric), card 28 (START, a board title), modal 36 (a pop-up's
// title), page 44 (the page's title). The match HUD's display sizes stay off the scale: its banners, score and "you died".
namespace type {
inline constexpr float kHint = 14, kBody = 15, kRow = 17, kEmphasis = 20, kSection = 23, kCard = 28, kModal = 36, kPage = 44;
inline constexpr float kBannerSub = 26, kScore = 34, kDeadTitle = 38, kBanner = 80;
inline constexpr float kScale[] = {kHint, kBody, kRow, kEmphasis, kSection, kCard, kModal, kPage};
// The typeface follows the role, not a number sitting in a call: the game's blackletter from a section up, its plain face
// below. (A tab's label is the one exception: emphasis size, blackletter, set where it is made.)
constexpr bool IsHeading(float size) { return size >= kSection; }
}  // namespace type

// The menus (multiplayer page, Esc panel) and the HUD's panels. Three looks, chosen once at start (ui_style in
// HSMMod.ini, UseMenuLook): the classic one (the default, the look before 0.8.0-pre43, "Iron & Blood": opaque dark
// faces, light text), the game's own (faces are its parchment and gilt art, tinted, and these are the plain fills drawn
// when the art is not loaded, and the faces text is judged on; text is dark ink, as on its scrolls) and dark (the game's
// art shaded dark and almost opaque, light text; a menu page sits on a darkened painting). Set before any widget is made; read only on the game thread
// after that.
struct MenuPalette {
    Rgba bg, backdrop, panel, card, cardHover, frame, frameHover, text, dim, accent, accentInk, ok, warn, light;
    // A picked card (a chosen option, an open tab): its face, its frame and the ink on it. Classic keeps the card face
    // inside a red frame, with light text; the game look is the game's amber button with dark ink; dark is that amber
    // shaded, where light text reads 2.5:1, so it takes dark ink too. platePicked: the same for a plate (your own
    // kill-feed line). field: a text box's fill.
    Rgba picked, pickedFrame, onPicked, platePicked, field;
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
    kAmber,                            // picked: the game's pressed button
    kAmber,                            // pickedFrame
    {0.010f, 0.007f, 0.005f, 1},       // onPicked: ink
    kAmber,                            // platePicked
    {0.86f, 0.80f, 0.66f, 1},          // field: a text box, parchment
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
    Keep(kAmber, kDarkKeepPicked),        // picked: the amber button, shaded as the skin shades it
    Keep(kAmber, kDarkKeepPicked),        // pickedFrame
    {0.010f, 0.007f, 0.005f, 1},          // onPicked: dark ink (bone on the shaded amber read 2.5:1)
    Keep(kAmber, kDarkKeepPlate),         // platePicked
    {0.016f, 0.014f, 0.012f, 0.85f},      // field: a text box, dark with the scene showing a little
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
    {0.0137f, 0.0116f, 0.0097f, 1},       // picked: the card face (the frame says picked)
    {0.3419f, 0.0110f, 0.0110f, 1},       // pickedFrame: the accent
    {0.8148f, 0.7529f, 0.6514f, 1},       // onPicked: the text
    kPlateMineClassic,                    // platePicked
    {0.016f, 0.014f, 0.012f, 1},          // field: a text box
};

enum class MenuLook : uint8_t { Dark, Game, Classic };
inline MenuLook gMenuLook = MenuLook::Classic;
inline Rgba kMenuBg = kClassicMenus.bg, kMenuBackdrop = kClassicMenus.backdrop, kMenuPanel = kClassicMenus.panel,
            kMenuCard = kClassicMenus.card, kMenuCardHover = kClassicMenus.cardHover, kMenuFrame = kClassicMenus.frame,
            kMenuFrameHover = kClassicMenus.frameHover, kMenuText = kClassicMenus.text, kMenuDim = kClassicMenus.dim,
            kMenuAccent = kClassicMenus.accent, kMenuAccentInk = kClassicMenus.accentInk, kMenuOk = kClassicMenus.ok,
            kMenuWarn = kClassicMenus.warn, kMenuLight = kClassicMenus.light, kMenuPicked = kClassicMenus.picked,
            kMenuPickedFrame = kClassicMenus.pickedFrame, kMenuOnPicked = kClassicMenus.onPicked,
            kMenuPlatePicked = kClassicMenus.platePicked, kMenuField = kClassicMenus.field;

inline const MenuPalette& PaletteOf(MenuLook look) {
    return look == MenuLook::Classic ? kClassicMenus : look == MenuLook::Game ? kGameMenus : kDarkMenus;
}
inline void UseMenuLook(MenuLook look) {
    const MenuPalette& m = PaletteOf(look);
    gMenuLook = look;
    kMenuBg = m.bg; kMenuBackdrop = m.backdrop; kMenuPanel = m.panel; kMenuCard = m.card; kMenuCardHover = m.cardHover;
    kMenuFrame = m.frame; kMenuFrameHover = m.frameHover; kMenuText = m.text; kMenuDim = m.dim; kMenuAccent = m.accent;
    kMenuAccentInk = m.accentInk; kMenuOk = m.ok; kMenuWarn = m.warn; kMenuLight = m.light; kMenuPicked = m.picked;
    kMenuPickedFrame = m.pickedFrame; kMenuOnPicked = m.onPicked; kMenuPlatePicked = m.platePicked; kMenuField = m.field;
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

// MENU LOOK as a list: the looks in the order the button stepped them, Classic first. Option i is kMenuLookOrder[i].
inline constexpr MenuLook kMenuLookOrder[3] = {MenuLook::Classic, MenuLook::Game, MenuLook::Dark};
inline const char* MenuLookName(MenuLook look) { return look == MenuLook::Classic ? "Classic" : look == MenuLook::Game ? "Game art" : "Dark"; }
inline std::vector<DropOption> MenuLookChoices() {
    std::vector<DropOption> v;
    for (MenuLook l : kMenuLookOrder) v.push_back({MenuLookName(l), true, ""});
    return v;
}
inline size_t MenuLookChoiceAt(MenuLook look) {
    for (size_t i = 0; i < 3; ++i) if (kMenuLookOrder[i] == look) return i;
    return 0;
}
inline MenuLook MenuLookPicked(size_t i) { return kMenuLookOrder[i < 3 ? i : 0]; }

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

// The same, mixing the display (sRGB) values, as the mock's CSS does: the numbers behind the contrast tests below. (Over mixes
// linear values, which makes a pale scene behind the smoke read much brighter than it is drawn.) That the engine blends
// in display space too is taken from the mock; it has not been checked in the game.
inline double ToDisplay(double c) { return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }
inline double FromDisplay(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
inline Rgba DisplayOf(Rgba c) { return {float(ToDisplay(c.r)), float(ToDisplay(c.g)), float(ToDisplay(c.b)), c.a}; }
inline Rgba LinearOf(Rgba c) { return {float(FromDisplay(c.r)), float(FromDisplay(c.g)), float(FromDisplay(c.b)), c.a}; }
inline Rgba OverDisplay(Rgba top, Rgba under) { return LinearOf(Over(DisplayOf(top), DisplayOf(under))); }

inline Rgba PanelOverScene() { return Over(kSmoke, kSceneGrey); }
inline Rgba PanelOverWhite() { return OverDisplay(kSmoke, kSceneWhite); }

// Contrast of text in colour `c` (its alpha included) on the panel over the reference scene.
inline double TextContrast(Rgba c) { Rgba p = PanelOverScene(); return Contrast(Over(c, p), p); }

// The same on the panel over a white scene, the worst case for light text.
inline double TextContrastOverWhite(Rgba c) { Rgba p = PanelOverWhite(); return Contrast(OverDisplay(c, p), p); }

inline Rgba Faded(Rgba c, float opacity) { return {c.r, c.g, c.b, c.a * opacity}; }

inline bool ReadableText(double contrast) { return contrast >= kMinTextContrast; }
inline bool ReadableDisabled(double contrast) { return contrast >= kMinDisabledContrast; }

// A disabled control's label: the face drawn at `alpha` over what is behind it, and the ink at `alpha` over that face (a
// widget's render opacity multiplies everything it draws, so a faded button fades its face and its label alike).
inline double DisabledContrast(Rgba ink, Rgba face, Rgba behind, float alpha = kDisabledAlpha) {
    const Rgba f = OverDisplay(Faded(face, alpha), behind);
    return Contrast(OverDisplay(Faded(ink, alpha), f), f);
}

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

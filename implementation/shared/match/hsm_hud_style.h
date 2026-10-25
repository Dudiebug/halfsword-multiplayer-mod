// HUD palette and the contrast arithmetic behind it (MATCH_ROUNDS_UI_CHAT_PLAN 3.1). Colours are linear
// FLinearColor values. Text is judged by WCAG contrast against the smoke panel laid over a mid-grey scene.
// Game-independent.
#pragma once
#include "match/hsm_match.h"
#include <algorithm>

namespace hsm::style {

inline constexpr Rgba kSmoke{0.02f, 0.017f, 0.014f, 0.82f};      // panel fill
inline constexpr Rgba kBone{0.93f, 0.89f, 0.80f, 1};             // body text
inline constexpr Rgba kBlood{0.70f, 0.05f, 0.04f, 1};            // accent: frames, bars, swatches (not text)
inline constexpr Rgba kBloodInk{0.95f, 0.32f, 0.26f, 1};         // the accent as text
inline constexpr Rgba kIron{0.45f, 0.47f, 0.50f, 1};             // secondary text, frames
inline constexpr Rgba kOk{0.35f, 0.65f, 0.25f, 1};
inline constexpr Rgba kWarn{0.85f, 0.62f, 0.05f, 1};
inline constexpr Rgba kSceneGrey{0.18f, 0.18f, 0.18f, 1};        // the reference scene behind a panel

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

// Team colour for text: TeamColor's tabard tints are too dark to read, so each team has a lighter ink of
// the same hue. No team (free-for-all): bone.
inline Rgba TeamInk(int team) {
    static constexpr Rgba kInk[] = {kBloodInk, {0.40f, 0.55f, 1.0f, 1}, {0.30f, 0.80f, 0.30f, 1}, kWarn};
    return team >= 0 && team < 4 ? kInk[team] : kBone;
}

}  // namespace hsm::style

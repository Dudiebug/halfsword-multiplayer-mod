// Name tags over the other fighters' heads (FFA / teams / duel): the HUD projects each head to the screen, this decides
// whether the tag shows and how it looks. Team ink in team formats, bone otherwise; far fighters fade and shrink.
// Game-independent.
#pragma once
#include "match/hsm_hud_style.h"
#include <algorithm>
#include <string>

namespace hsm {

// One tag's input from the session: where the head is in the world (cm) and who it is.
struct NameTagSrc {
    std::string name;
    double wx = 0, wy = 0, wz = 0;   // the point above the head
    int8_t team = kNoTeam;
    bool alive = true;
    float seen = 1.f;                // 0..1: how much of the tag is let through by the wall test (hsm_occlusion.h)
};

struct NameTagView {
    bool visible = false;
    float x = 0, y = 0;              // DPI-independent canvas units (pixels / viewport scale), the tag's bottom centre
    float fontSize = 0, alpha = 0;
    Rgba ink = style::kBone;
};

inline constexpr float kNameTagFarCm = 4000.f, kNameTagNearCm = 600.f;

// projectedOk: ProjectWorldLocationToScreen found the point in front of the camera. sx/sy are viewport pixels.
// seen: the opacity the wall test leaves (1 clear view, 0 behind a wall, between while it fades).
inline NameTagView NameTagLayout(bool projectedOk, float sx, float sy, float viewW, float viewH, float scale,
                                 float distanceCm, bool alive, int team, Format format, float seen = 1.f) {
    NameTagView v;
    if (!projectedOk || !alive || !(scale > 0) || !(distanceCm >= 0) || distanceCm > kNameTagFarCm) return v;
    if (!(seen > 0.01f)) return v;                                           // also rejects NaN
    if (!(sx >= 0 && sy >= 0 && sx <= viewW && sy <= viewH)) return v;       // also rejects NaN
    const float t = std::clamp((distanceCm - kNameTagNearCm) / (kNameTagFarCm - kNameTagNearCm), 0.f, 1.f);
    v.visible = true;
    v.x = sx / scale; v.y = sy / scale;
    v.fontSize = 22.f - 8.f * t;
    v.alpha = (1.f - 0.65f * t) * std::min(seen, 1.f);
    v.ink = TeamCount(format) >= 2 && team >= 0 ? style::TeamInk(team) : style::kBone;
    return v;
}

}  // namespace hsm

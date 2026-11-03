// Objectives: things in a match that belong to a team, sit somewhere and are in a state, which the HUD marks on the
// screen (Capture the Flag's flags first; other modes add kinds). Game-independent.
//   Objective        - kind, owner team, state (home / carried / dropped), carrier, place, time left
//   ValidObjective   - the wire decoder's check (MatchInfo.objectives)
//   MarkerSrc/Layout - one HUD marker per objective: where it is (a carried one rides on its carrier's head), its text
//                      and ink; MarkerLayout turns a projected point into a screen place the way NameTagLayout does
#pragma once
#include "match/hsm_hud_style.h"
#include "match/hsm_match.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm {

enum class ObjectiveKind : uint8_t { Flag = 0 };
constexpr uint8_t kObjectiveKindCount = 1;
// Home: at its base. Carried: held by `carrier` (an entity). Dropped: lying at its place, going home when timerMs runs out.
enum class ObjectiveState : uint8_t { Home = 0, Carried = 1, Dropped = 2 };
constexpr uint8_t kObjectiveStateCount = 3;
constexpr size_t kMaxObjectives = 8;
constexpr uint32_t kMaxObjectiveTimerMs = 60000;   // a dropped flag's return time tops out at kFlagReturnMaxS (hsm_ctf.h)

struct Objective {
    ObjectiveKind kind = ObjectiveKind::Flag;
    int8_t owner = kNoTeam;             // the team it belongs to
    ObjectiveState state = ObjectiveState::Home;
    uint8_t carrier = 0;                // Carried: the entity holding it; else 0
    float x = 0, y = 0, z = 0;          // Home and Dropped: where it lies (cm); Carried: where it was last seen
    uint32_t timerMs = 0;               // Dropped: time before it goes home; else 0
    bool operator==(const Objective&) const = default;
};

// What a decoder accepts: known kind and state, a team or none, a carrier exactly when carried, a timer only while
// dropped and within the cap, a finite place.
inline bool ValidObjective(const Objective& o) {
    if (uint8_t(o.kind) >= kObjectiveKindCount || uint8_t(o.state) >= kObjectiveStateCount) return false;
    if (o.owner < -1 || o.owner > 7) return false;
    if ((o.state == ObjectiveState::Carried) != (o.carrier != 0)) return false;
    if (o.timerMs > kMaxObjectiveTimerMs || (o.timerMs != 0 && o.state != ObjectiveState::Dropped)) return false;
    if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z)) return false;
    return std::fabs(o.x) <= 1e7f && std::fabs(o.y) <= 1e7f && std::fabs(o.z) <= 1e7f;
}

// ---------------------------------------------------------------- HUD markers
// One marker's input: where it hangs in the world (cm) and what it says.
struct MarkerSrc {
    std::string label;                  // "BLUE FLAG"
    double wx = 0, wy = 0, wz = 0;
    int8_t team = kNoTeam;              // the ink
    ObjectiveState state = ObjectiveState::Home;
    uint8_t carrier = 0;
    uint32_t timerMs = 0;
};

inline constexpr float kMarkerFarCm = 40000.f, kMarkerNearCm = 600.f;
inline constexpr double kMarkerAboveHeadCm = 60.0;   // a carried marker hangs this far over the carrier's head point
inline constexpr double kMarkerAboveGroundCm = 120.0;   // a flag marker hangs this far over its place

// The markers of `objs`. headOf(entity, x, y, z) gives the point above a carrier's head; a carrier it cannot place gets no
// marker (the rest do). Home and dropped ones hang over their place.
using HeadLookup = std::function<bool(uint8_t entity, double& x, double& y, double& z)>;
inline std::vector<MarkerSrc> MarkersFor(const std::vector<Objective>& objs, const HeadLookup& headOf) {
    std::vector<MarkerSrc> out;
    for (const Objective& o : objs) {
        if (!ValidObjective(o) || o.kind != ObjectiveKind::Flag) continue;
        MarkerSrc m;
        m.label = TeamName(o.owner) + " FLAG";
        m.team = o.owner; m.state = o.state; m.carrier = o.carrier; m.timerMs = o.timerMs;
        if (o.state == ObjectiveState::Carried) {
            if (!headOf || !headOf(o.carrier, m.wx, m.wy, m.wz)) continue;
            m.wz += kMarkerAboveHeadCm;
        } else { m.wx = o.x; m.wy = o.y; m.wz = o.z + kMarkerAboveGroundCm; }
        out.push_back(std::move(m));
    }
    return out;
}

struct MarkerView {
    bool visible = false;
    float x = 0, y = 0;                 // DPI-independent canvas units, the marker's bottom centre
    float fontSize = 0, alpha = 0;
    Rgba ink = style::kBone;
    std::string text;
};

inline std::string MarkerText(const MarkerSrc& m, float distanceCm) {
    std::string t = m.label;
    if (m.state == ObjectiveState::Dropped) t += " " + std::to_string((m.timerMs + 999) / 1000) + " s";
    else if (m.state == ObjectiveState::Carried) t += " CARRIED";
    if (m.state != ObjectiveState::Carried && distanceCm >= 0) t += "  " + std::to_string(int(distanceCm / 100.f + 0.5f)) + " m";
    return t;
}

// projectedOk: ProjectWorldLocationToScreen found the point in front of the camera; sx/sy are viewport pixels. Home
// flags are dimmer than the ones in play. Far markers shrink and fade, never to nothing.
inline MarkerView MarkerLayout(const MarkerSrc& m, bool projectedOk, float sx, float sy, float viewW, float viewH, float scale, float distanceCm) {
    MarkerView v;
    if (!projectedOk || !(scale > 0) || !(distanceCm >= 0) || distanceCm > kMarkerFarCm) return v;
    if (!(sx >= 0 && sy >= 0 && sx <= viewW && sy <= viewH)) return v;       // also rejects NaN
    const float t = std::clamp((distanceCm - kMarkerNearCm) / (kMarkerFarCm - kMarkerNearCm), 0.f, 1.f);
    v.visible = true;
    v.x = sx / scale; v.y = sy / scale;
    v.fontSize = 24.f - 8.f * t;
    v.alpha = (1.f - 0.5f * t) * (m.state == ObjectiveState::Home ? 0.6f : 1.f);
    v.ink = style::TeamInk(m.team);
    v.text = MarkerText(m, distanceCm);
    return v;
}

}  // namespace hsm

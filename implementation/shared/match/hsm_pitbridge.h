// Pit Bridge Duels, where the two fighters stand (Wave 3): at the two ends of the Pit's bridge. The bridge is the level's
// plank board (BP_Structure_Board_Destructible_1X6M_C); its ends come from the actor's bounds. Without the bridge the
// two fighters spawn on the two sides of the Pit's spawn points (PartitionSides with 2), which the host works out itself.
// Game-independent, unit-tested.
#pragma once
#include "session/hsm_spawn.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace hsm {

// A 1X6M board is 600 cm long; anything shorter than this half-length (cm) is not the bridge.
constexpr double kBridgeMinHalfLengthCm = 200.0;
// How far in from the very end of the board a fighter stands (cm) at most; never more than a quarter of the half length (75 cm on a 6 m board).
constexpr double kBridgeEndInsetCm = 100.0;

struct BridgeEnds { Point3 a, b; bool ok = false; };

// The ends of a board from its world bounds (origin and half extents, as AActor::GetActorBounds gives them). The long
// horizontal axis is the bridge's; each end is inset from the board's tip and stands on its top. ok is false for a box
// that is no board: not finite, too short, or not clearly longer than it is wide.
inline BridgeEnds BridgeEndsFrom(const Point3& origin, const Point3& extent) {
    BridgeEnds out;
    if (!std::isfinite(origin.x) || !std::isfinite(origin.y) || !std::isfinite(origin.z) ||
        !std::isfinite(extent.x) || !std::isfinite(extent.y) || !std::isfinite(extent.z)) return out;
    const bool alongX = extent.x >= extent.y;
    const double lng = alongX ? extent.x : extent.y, shrt = alongX ? extent.y : extent.x;
    if (lng < kBridgeMinHalfLengthCm || lng < 2.0 * shrt) return out;
    const double reach = lng - std::min(kBridgeEndInsetCm, lng * 0.25);
    const double top = origin.z + std::max(extent.z, 0.0);
    out.a = alongX ? Point3{origin.x - reach, origin.y, top} : Point3{origin.x, origin.y - reach, top};
    out.b = alongX ? Point3{origin.x + reach, origin.y, top} : Point3{origin.x, origin.y + reach, top};
    out.ok = true;
    return out;
}

// Of several boards the level has, the longest (the bridge); -1 when none is one. Ties go to the first.
inline int PickBridge(const std::vector<BridgeEnds>& boards, const std::vector<double>& lengthsCm) {
    int best = -1;
    for (size_t i = 0; i < boards.size() && i < lengthsCm.size(); ++i)
        if (boards[i].ok && (best < 0 || lengthsCm[i] > lengthsCm[size_t(best)])) best = int(i);
    return best;
}

// The side the fighter at place `index` of the fighting pair starts on: the first on side 0, the second on side 1.
inline int PitSideOf(int index) { return index == 0 ? 0 : 1; }

}  // namespace hsm

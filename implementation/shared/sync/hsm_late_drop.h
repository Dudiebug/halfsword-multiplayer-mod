#pragma once
// A drop this machine gave up on (ObjectSync::GiveUp: its stand-in's hand held no copy of it) is not lost for good. The
// owner keeps streaming the dropped weapon, and the stream says where it is: the loose weapon here nearest to that place
// (of the class the owner's hand showed, when that is known) is the drop's copy, and the drop becomes a prop here like
// any other (streamed, driven, despawned when idle). 0.8.0-pre18: given-up drops stayed untracked on that machine for
// their whole life (3-7 a session), so nobody streamed them there once the owner's lease lapsed and the idle despawn never
// saw them (the weapon census grew to 72-77, copies lay in different places).
//
// A weapon that is a prop here already under another id is never taken (two ids for one weapon): when it is the nearest
// one, PickLateMatch says so (aliased) and the caller binds nothing.
#include "sync/hsm_handmatch.h"
#include <cmath>
#include <vector>

namespace hsm::sync {

constexpr double kLateMatchRadiusUu = 150.0;       // the class is known: the copy lies this close to the stream (cm)
constexpr double kLateMatchNoClassRadiusUu = 60.0; // the class is not known: any weapon, but only right at the stream
constexpr double kLateDropKeepMs = 30000.0;        // a given-up drop waits this long for a stream and a copy

struct LateCandidate {
    V3 pos;                                        // the loose weapon's location here, world
    uint32_t classHash = 0;
    bool propElsewhere = false;                    // already a prop here under another id
};

struct LatePick {
    int index = -1;                                // into the candidates; -1: none within reach
    bool aliased = false;                          // the nearest is a prop here under another id: not bound
    double distUu = 0;
};

inline double LateDist(const V3& a, const V3& b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// hintClass: the class the owner's hand showed (kHandEmpty / kHandUnknown / 0: not known). The nearest compatible candidate
// within the radius; ties: the first. A candidate of another known class is never compatible with a known class.
inline LatePick PickLateMatch(const std::vector<LateCandidate>& cands, uint32_t hintClass, const V3& streamAt) {
    const bool known = hintClass != kHandEmpty && hintClass != kHandUnknown;
    const double radius = known ? kLateMatchRadiusUu : kLateMatchNoClassRadiusUu;
    LatePick best;
    for (size_t i = 0; i < cands.size(); ++i) {
        if (known && cands[i].classHash != hintClass) continue;
        const double d = LateDist(cands[i].pos, streamAt);
        if (!(d <= radius)) continue;              // also false for NaN
        if (best.index < 0 || d < best.distUu) {
            best.index = int(i);
            best.distUu = d;
            best.aliased = cands[i].propElsewhere;
        }
    }
    return best;
}

inline bool LateDropExpired(double givenUpMs, double nowMs) { return nowMs - givenUpMs >= kLateDropKeepMs; }

}  // namespace hsm::sync

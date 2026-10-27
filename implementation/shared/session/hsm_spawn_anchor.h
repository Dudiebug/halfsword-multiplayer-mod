// role=server has no host fighter to read a spawn centre from. The anchor is the first source that answers:
//   1. the stock pawn's position when the level made one (captured before the server strips it),
//   2. the level's spawners (the one nearest their centroid),
//   3. a PlayerStart actor,
//   4. a navigable point (the navmesh around the level's origin),
//   5. a floor found by a downward trace under the level's static meshes,
//   6. the level's origin, once kAnchorGiveUpMs have passed (never wait forever).
// SearchAnchor asks the sources lazily, in that order, so the costly ones (navmesh, traces) only run when the cheap ones
// came back empty. AnchorSearch is the timing: nothing but the pawn before kAnchorSpawnerWaitMs (the level's spawners and
// its pawn appear a moment after the load), then a pass every kAnchorRetryMs. Game-independent, unit-tested.
#pragma once
#include "session/hsm_spawn.h"
#include <functional>
#include <optional>
#include <vector>

namespace hsm {

enum class AnchorKind : uint8_t { StockPawn, Spawner, PlayerStart, Navmesh, Floor, Origin, None };
struct SpawnAnchor { Point3 pos; AnchorKind kind = AnchorKind::None; };

namespace anchor_detail {
inline Point3 Centroid(const std::vector<Point3>& v) {
    Point3 c;
    for (const Point3& p : v) { c.x += p.x; c.y += p.y; c.z += p.z; }
    const double n = double(v.size());
    return {c.x / n, c.y / n, c.z / n};
}
// Nearest to `to`; ties keep the lowest index. v must not be empty.
inline const Point3& Nearest(const std::vector<Point3>& v, const Point3& to) {
    size_t best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (Dist2(v[i], to) < Dist2(v[best], to)) best = i;
    return v[best];
}
}  // namespace anchor_detail

// The choice from sources already read: the stock pawn, else the spawner nearest the spawners' centroid, else the first
// PlayerStart.
inline SpawnAnchor ChooseAnchor(const std::optional<Point3>& stockPawn, const std::vector<Point3>& playerStarts, const std::vector<Point3>& spawners) {
    if (stockPawn) return {*stockPawn, AnchorKind::StockPawn};
    if (!spawners.empty()) return {anchor_detail::Nearest(spawners, anchor_detail::Centroid(spawners)), AnchorKind::Spawner};
    if (!playerStarts.empty()) return {playerStarts.front(), AnchorKind::PlayerStart};
    return {};
}

constexpr uint32_t kAnchorSpawnerWaitMs = 3000;    // after the level loads, the spawners (and the stock pawn) get this long to appear
constexpr uint32_t kAnchorRetryMs = 1000;          // between search passes
constexpr uint32_t kAnchorGiveUpMs = 20000;        // after the level loads: nothing answered, anchor at the origin

// The sources, each read only when the ones before it found nothing. An empty function is a source that is not there.
struct AnchorSources {
    std::function<std::vector<Point3>()> spawners;
    std::function<std::vector<Point3>()> playerStarts;
    std::function<std::optional<Point3>()> navmesh;
    std::function<std::optional<Point3>()> floor;
};

// One search pass (the stock pawn is the caller's: it needs no search). giveUp: the wait is over, so when nothing
// answers the origin is the anchor. Otherwise AnchorKind::None: try again next pass.
inline SpawnAnchor SearchAnchor(const AnchorSources& src, bool giveUp) {
    if (src.spawners) {
        const std::vector<Point3> v = src.spawners();
        if (!v.empty()) return ChooseAnchor(std::nullopt, {}, v);
    }
    if (src.playerStarts) {
        const std::vector<Point3> v = src.playerStarts();
        if (!v.empty()) return ChooseAnchor(std::nullopt, v, {});
    }
    if (src.navmesh)
        if (const std::optional<Point3> p = src.navmesh()) return {*p, AnchorKind::Navmesh};
    if (src.floor)
        if (const std::optional<Point3> p = src.floor()) return {*p, AnchorKind::Floor};
    if (giveUp) return {Point3{0, 0, 0}, AnchorKind::Origin};
    return {};
}

// When the passes run, per level. Times are the caller's millisecond clock (differences, so a wrap is harmless).
class AnchorSearch {
public:
    void OnLevel(uint32_t now) { levelAt_ = now; lastPass_ = 0; passes_ = 0; }
    // A pass is due: not before kAnchorSpawnerWaitMs, then the first at once and the next every kAnchorRetryMs.
    bool Due(uint32_t now) const {
        if (now - levelAt_ < kAnchorSpawnerWaitMs) return false;
        return passes_ == 0 || now - lastPass_ >= kAnchorRetryMs;
    }
    bool GiveUp(uint32_t now) const { return now - levelAt_ >= kAnchorGiveUpMs; }
    void Passed(uint32_t now) { lastPass_ = now; ++passes_; }
    uint32_t passes() const { return passes_; }
private:
    uint32_t levelAt_ = 0, lastPass_ = 0, passes_ = 0;
};

// Only a measured stock pawn gives a stand height; a PlayerStart's, a spawner's or a traced floor's is not a Willie's.
inline double StandOffsetFor(AnchorKind kind, double raw) { return kind == AnchorKind::StockPawn ? SaneStandOffset(raw) : kStandDefaultCm; }

}  // namespace hsm

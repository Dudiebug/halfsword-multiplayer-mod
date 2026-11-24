// Walking the host's dungeon (docs/PLAN_CRYPTS_COOP.md, PR B). Pure rules for three things the game layer needs:
//   - InBox: the host's rooms fire their events when the host's own pawn overlaps a room's boxes (SpawnCharacters, SpawnLevel,
//     Box); a remote stand-in is a mock body, so the host polls its position against each live room's boxes instead, and calls
//     the room's own event the first time a stand-in is inside (OnceTracker: once per room, box and player).
//   - StandCheck: the reason a spawn slot is refused (no floor under the fighter, a ledge, a wall in the way, outside the
//     arena), so the host's log says why a client spawned where it did (#3847: remote players stacked on one spot).
//   - PickEntranceSpots: remote players spread over the entrance room's standable spots near player 0, never on one spot.
// Game-independent and unit-tested (session/test_hsm_crypt_walk.cpp).
#pragma once
#include "session/hsm_spawn.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace hsm::crypt {

// A room's box in the world: centre, half extents and the yaw of the room (degrees). Pitch and roll are not used by the
// game's boxes.
struct WorldBox { Point3 centre; double hx = 0, hy = 0, hz = 0, yaw = 0; };

inline bool InBox(const WorldBox& b, const Point3& p, double margin = 0) {
    if (!(b.hx > 0 && b.hy > 0 && b.hz > 0)) return false;               // an unread box holds nothing
    const double r = -b.yaw * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    const double dx = p.x - b.centre.x, dy = p.y - b.centre.y, dz = p.z - b.centre.z;
    const double lx = c * dx - s * dy, ly = s * dx + c * dy;
    return std::fabs(lx) <= b.hx + margin && std::fabs(ly) <= b.hy + margin && std::fabs(dz) <= b.hz + margin;
}

// Which of a room's boxes.
enum class BoxKind : uint8_t { SpawnCharacters = 0, SpawnLevel = 1, Box = 2 };
inline const char* BoxName(BoxKind k) { return k == BoxKind::SpawnCharacters ? "SpawnCharacters" : k == BoxKind::SpawnLevel ? "SpawnLevel" : "Box"; }

// "Once per room per box" (per player): First() is true the first time it is asked about a key.
class OnceTracker {
public:
    bool First(uint16_t room, BoxKind box, uint8_t slot) { return seen_.insert(std::make_tuple(room, uint8_t(box), slot)).second; }
    bool Seen(uint16_t room, BoxKind box, uint8_t slot) const { return seen_.count(std::make_tuple(room, uint8_t(box), slot)) != 0; }
    void ForgetRoom(uint16_t room) { for (auto it = seen_.begin(); it != seen_.end();) it = std::get<0>(*it) == room ? seen_.erase(it) : std::next(it); }
    void Clear() { seen_.clear(); }
    size_t Size() const { return seen_.size(); }
private:
    std::set<std::tuple<uint16_t, uint8_t, uint8_t>> seen_;
};

// Why a slot was refused.
enum class StandRefusal : uint8_t { None, NoFloor, FloorStep, Wall, OutOfArea };
inline const char* StandRefusalName(StandRefusal r) {
    switch (r) {
        case StandRefusal::None: return "ok";
        case StandRefusal::NoFloor: return "no floor under the fighter";
        case StandRefusal::FloorStep: return "floor too far above or below the centre's";
        case StandRefusal::Wall: return "a wall in the way";
        case StandRefusal::OutOfArea: return "outside the arena";
    }
    return "?";
}

// The checks of Stands(c, p) (session.cpp), with the reason. floorAt(Point3) -> optional<double> (the floor under a point, nullopt:
// none or a trap); lineClear(from, to) -> bool; both at the engine. `cf` is the centre's floor.
template <class FloorAt, class LineClear>
StandRefusal StandCheck(std::optional<double> cf, const Point3& c, const Point3& p, FloorAt floorAt, LineClear lineClear) {
    static constexpr double kDir[5][2] = {{0, 0}, {1, 0}, {0, 1}, {-1, 0}, {0, -1}};
    bool step = false;
    for (const auto& d : kDir) {
        const std::optional<double> f = floorAt(Point3{p.x + d[0] * kFootprintCm, p.y + d[1] * kFootprintCm, p.z});
        if (!f || !std::isfinite(*f)) return StandRefusal::NoFloor;
        if (!FloorFits(cf, f)) step = true;
    }
    if (step) return StandRefusal::FloorStep;
    const double z = (cf ? *cf : c.z) + kWallProbeCm;
    if (!lineClear(Point3{c.x, c.y, z}, Point3{p.x, p.y, z})) return StandRefusal::Wall;
    return StandRefusal::None;
}

// Places for `count` remote players in the entrance room: its standable spots nearest `anchor` (player 0's place) first, each at
// least `gap` from the places before it and from `taken`; `stands(spot)` is the engine's StandCheck for the spot, and a refused
// spot is passed to `refused`. When the spots run out, the rest spread round the anchor (FreeSpot), so nobody stacks.
template <class Stands, class Refused>
std::vector<Point3> PickEntranceSpots(std::vector<Point3> spots, const Point3& anchor, size_t count, const std::vector<Point3>& taken,
                                      double gap, Stands stands, Refused refused) {
    std::stable_sort(spots.begin(), spots.end(), [&](const Point3& a, const Point3& b) { return FlatDist2(a, anchor) < FlatDist2(b, anchor); });
    std::vector<Point3> all = taken, out;
    const double g2 = gap * gap - 1e-6;
    for (const Point3& s : spots) {
        if (out.size() >= count) break;
        bool clear = true;
        for (const Point3& t : all) if (FlatDist2(s, t) < g2) { clear = false; break; }
        if (!clear) continue;
        const StandRefusal why = stands(s);
        if (why != StandRefusal::None) { refused(s, why); continue; }
        out.push_back(s); all.push_back(s);
    }
    while (out.size() < count) { const Point3 p = FreeSpot(anchor, all, 0, gap); out.push_back(p); all.push_back(p); }
    return out;
}

}  // namespace hsm::crypt

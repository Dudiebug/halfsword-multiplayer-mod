// The Crypts room catalog and the join rule (docs/PLAN_CRYPTS_COOP.md): the game's Crypts rooms, where each room's entrances
// sit and how two rooms meet, so the mod can place a room the game's director built (or build a fixed table of rooms) the same
// on every machine. The lobby has one Crypts map, the game's own dungeon; nothing here picks a layout for it any more, and
// BuildRooms (below) is waiting for the co-op milestones.
//   - the room catalog: each room class's three levels (Module, Lighting, Logic), its entrances in its own frame, its
//     Module bounds and its spawn points, all copied from the Crypts probe's log ([mod] crypts_probe=1, 2026-11-13);
//   - rows of {room, parent row, parent's entrance, own entrance};
//   - Place: each row's origin, by the join rule the probe measured on a walked run (all 11 rooms within 2 uu);
//   - Joins and OpenEntrances: where two rooms meet (their gates are cleared) and where an entrance leads nowhere (the
//     loader bars it with the game's unbreakable barricade);
//   - Check: a table that cannot be built (a missing room or entrance, an entrance used twice, two rooms that overlap
//     without joining) is refused;
//   - InstanceName: every streamed level's name, the same on every machine (never the engine's instance counter).
#pragma once
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm::crypts {

// A place and a turn, in Unreal units and degrees. Every entrance of every room has pitch and roll 0.
struct Pose { double x = 0, y = 0, z = 0, yaw = 0; };
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Bounds { Vec3 lo, hi; };

inline constexpr size_t kMaxEntrances = 4;
inline constexpr size_t kMaxRows = 16;
inline constexpr double kOverlapLimit = 100;   // two rooms that do not join may touch, never overlap by 1 m on every axis
inline constexpr double kJoinRadius = 50;      // a gate this close to a joint is that joint's (the block's own Door To Ignore radius)

inline constexpr const char* kBaseLevel = "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main";
inline constexpr const char* kAmbientLevel = "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Ambient";   // the director's, at (0,0,0)
inline constexpr std::string_view kInstancePrefix = "HSMCrypts_";

// A room class BP_Procedural_Block_<name>_C. Paths are package paths (LoadLevelInstance's LevelName); logic is empty
// when the class names no Logic level. spawns: BP_SpawnerPoint_Willies_C in its levels (Traps_2's Lighting level was
// not measured; Traps_1's has none).
struct RoomClass {
    const char* name;
    const char* module;
    const char* lighting;
    const char* logic;
    uint8_t entranceCount;
    Pose entrances[kMaxEntrances];
    Bounds bounds;   // the Module level's meshes, in the room's frame
    uint8_t spawns;
};

inline constexpr RoomClass kRooms[] = {
    {"Chamber_1", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Chamber_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Chamber_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Chamber_1_Arena_Logic", 3, {{-1760, 1180, 0, 90}, {590, -990, 0, -90}, {1890, 920, 0, 0}},
     {{-1878, -1113, -320}, {1981, 1275, 989}}, 17},
    {"Chamber_2", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Chamber_2", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Chamber_2_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Chamber_2_Arena_Logic", 3, {{-1240, 160, 0, 180}, {810, -1320, 0, -90}, {1400, 60, 0, 0}},
     {{-1331, -1406, -314}, {1473, 1766, 1300}}, 13},
    {"Chamber_3", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Chamber_3", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Chamber_3_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Chamber_3_Arena_Logic", 4, {{-1510, 0, 0, 180}, {-150, -2460, 0, -90}, {1610, -200, 0, 0}, {-100, 1960, -300, 90}},
     {{-1578, -2538, -1830}, {1726, 2016, 1000}}, 18},
    {"Chamber_4", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Chamber_4", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Chamber_4_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Chamber_4_Arena_Logic", 4, {{-2750, 1075, 0, 180}, {10, -2080, 0, -90}, {2020, -1880, 150, 0}, {110, 1430, 0, 90}},
     {{-2839, -2165, -1036}, {2097, 1519, 1619}}, 22},
    {"Chamber_5", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Chamber_5", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Chamber_5_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Chamber_5_Arena_Logic", 3, {{-1260, -90, 0, 180}, {-250, -1100, 0, -90}, {500, 1040, 0, 90}},
     {{-1339, -1205, -1053}, {1287, 1111, 1000}}, 10},
    {"Corridor_1", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Corridor_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Corridor_1_Arena_Logic", 3, {{-980, 0, 0, 180}, {1040, 0, 0, 0}, {30, -510, 0, -90}},
     {{-1084, -598, -27}, {1147, 544, 597}}, 3},
    {"Corridor_1_Shortcut", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_3", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Corridor_3_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Corridor_3_Arena_Logic", 2, {{-980, 0, 0, 180}, {1040, 0, 0, 0}},
     {{-1083, -353, -27}, {1151, 544, 597}}, 3},
    {"Corridor_2", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_2", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Corridor_2_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Corridor_2_Arena_Logic", 3, {{-930, -1690, 0, -90}, {1580, 0, 0, 0}, {-1440, 100, 0, 180}},
     {{-1510, -1770, -32}, {1656, 506, 597}}, 5},
    {"Corridor_3", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_3", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Corridor_3_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Corridor_3_Arena_Logic", 2, {{-980, 0, 0, 180}, {1040, 0, 0, 0}},
     {{-1083, -353, -27}, {1151, 544, 597}}, 3},
    {"Corridor_3_Exit", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_3", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Corridor_3_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Corridor_3_Arena_Logic", 2, {{-980, 0, 0, 180}, {1040, 0, 0, 0}},
     {{-1083, -353, -27}, {1151, 544, 597}}, 3},
    {"Entrance_2", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Exit_2", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Entrance_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Exit_1_Arena_Logic", 4, {{-420, -250, -440, 180}, {1610, -200, -890, 0}, {1400, -1210, -1190, -90}, {1500, 1410, -1190, 90}},
     {{-7354, -2103, -1229}, {1802, 1588, 3353}}, 7},
    {"Exit_1", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Exit_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Exit_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Exit_1_Arena_Logic", 3, {{1610, -200, -890, 0}, {1400, -1210, -1190, -90}, {1500, 1410, -1190, 90}},
     {{-4762, -1296, -1229}, {1794, 1498, 3048}}, 7},
    {"Exit_2", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Exit_2", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Exit_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Exit_1_Arena_Logic", 3, {{1610, -200, -890, 0}, {1400, -1210, -1190, -90}, {1500, 1410, -1190, 90}},
     {{-7354, -2103, -1229}, {1802, 1588, 3353}}, 7},
    {"Hall_1", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Hall_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Hall_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Hall_1_Arena_Logic", 4, {{100, 1950, 600, 90}, {-2730, -220, 300, 180}, {0, -2480, 150, -90}, {2410, 1380, 150, 0}},
     {{-2834, -2586, -1036}, {2512, 2075, 1197}}, 18},
    {"Sepulchre", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Treasury_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Treasury_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Treasury_1_Arena_Logic", 1, {{-1350, 0, 120, 180}},
     {{-1513, -3503, -1980}, {2072, 1419, 920}}, 31},
    {"Stairs_1", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Stairs_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Stairs_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Stairs_1_Arena_Logic", 3, {{-1010, -50, -130, 180}, {1100, 610, -880, 90}, {1000, -610, -880, -90}},
     {{-1108, -747, -915}, {1600, 717, 274}}, 4},
    {"Stairs_1_Shortcut", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Stairs_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Stairs_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Stairs_1_Arena_Logic", 3, {{-1010, -50, -130, 180}, {1100, 610, -880, 90}, {1000, -610, -880, -90}},
     {{-1108, -747, -915}, {1600, 717, 274}}, 4},
    {"Stairs_1_UP", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Stairs_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Stairs_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Stairs_1_Arena_Logic", 3, {{-1010, -50, -130, 180}, {1100, 610, -880, 90}, {1000, -610, -880, -90}},
     {{-1108, -747, -915}, {1600, 717, 274}}, 4},
    {"Stairs_2", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Stairs_2", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Stairs_2_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Stairs_2_Arena_Logic", 2, {{-1210, 0, 20, 180}, {610, 0, -430, 0}},
     {{-1303, -422, -452}, {701, 406, 440}}, 2},
    {"Stairs_2_UP", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Stairs_2", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Stairs_2_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Stairs_2_Arena_Logic", 2, {{-1210, 0, 20, 180}, {610, 0, -430, 0}},
     {{-1303, -422, -452}, {701, 406, 440}}, 2},
    {"Traps_1", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Traps_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Traps_1_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Traps_1_Arena_Logic", 2, {{-1020, 0, 0, 180}, {1000, 0, 0, 0}},
     {{-1125, -675, -38}, {1099, 528, 800}}, 2},
    {"Traps_2", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Traps_1", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Traps_2_Lighting", "/Game/Maps/Test/ProceduralBlocksTest/Logic/Map_Procedural_Block_Test_Module_Traps_1_Arena_Logic", 2, {{-1020, 0, 0, 180}, {1000, 0, 0, 0}},
     {{-1125, -675, -38}, {1099, 528, 800}}, 2},   // spawns unknown: ['lighting']
    {"Treasury_1", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Treasury_2", "/Game/Maps/Test/ProceduralBlocksTest/Lighting/Map_Procedural_Block_Test_Module_Treasury_2_Lighting", "", 1, {{-1400, 0, 65, 180}},
     {{-1511, -1105, 50}, {-345, 1112, 662}}, 0},
};
inline constexpr size_t kRoomCount = sizeof(kRooms) / sizeof(kRooms[0]);

inline const RoomClass* FindRoom(std::string_view name) {
    for (const RoomClass& r : kRooms) if (name == r.name) return &r;
    return nullptr;
}

// One room of a layout. Row 0 is the root (parent -1, parentEntrance -1): its ownEntrance (when >= 0) sits on the
// layout's start, as the game's placed block joins its start entrance to (0,0,0); with -1 its origin is the start.
// Every other row joins its ownEntrance to the parentEntrance of an earlier row.
struct Row {
    const char* room;
    int8_t parent;
    int8_t parentEntrance;
    int8_t ownEntrance;
};

struct Layout {
    const char* name;      // the instance names' prefix: letters and digits only
    const char* display;
    Pose start;
    const Row* rows;
    uint8_t count;
};

// ------------------------------------------------------------------ the join rule

// An angle in degrees, folded into [-180, 180).
inline double WrapYaw(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0) a += 360.0;
    return a - 180.0;
}

// A pose given in a room's frame, in the world: the room's origin turned by its yaw.
inline Pose ToWorld(const Pose& origin, const Pose& local) {
    const double r = origin.yaw * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    return {origin.x + c * local.x - s * local.y, origin.y + s * local.x + c * local.y, origin.z + local.z, WrapYaw(origin.yaw + local.yaw)};
}

// A room's origin when its entrance `own` (in its own frame) sits on `start` (a parent's entrance in the world): it faces
// back through the doorway, so it turns by start.yaw - own.yaw + 180.
inline Pose JoinAt(const Pose& start, const Pose& own) {
    Pose o;
    o.yaw = WrapYaw(start.yaw - own.yaw + 180.0);
    const double r = o.yaw * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
    o.x = start.x - (c * own.x - s * own.y);
    o.y = start.y - (s * own.x + c * own.y);
    o.z = start.z - own.z;
    return o;
}

// ------------------------------------------------------------------ checks

enum class LayoutError : uint8_t {
    None,
    Empty,            // no rows, or more than kMaxRows
    UnknownRoom,      // a row names no catalog room
    BadRoot,          // row 0 has a parent, or another row has none
    BadParent,        // a parent that is not an earlier row
    BadEntrance,      // an entrance index the room does not have
    EntranceReused,   // two joins on one entrance
    Overlap,          // two rooms that do not join overlap by more than kOverlapLimit on every axis
};
struct LayoutCheck {
    LayoutError error = LayoutError::None;
    int row = -1, other = -1;
    bool ok() const { return error == LayoutError::None; }
};

namespace layout_detail {
inline bool EntranceOk(const RoomClass* r, int e) { return r && e >= 0 && e < int(r->entranceCount); }
// The world box of a room's Module bounds at an origin (the bounds' four corners turned by its yaw).
inline Bounds WorldBox(const RoomClass& r, const Pose& o) {
    Bounds b{{1e300, 1e300, o.z + r.bounds.lo.z}, {-1e300, -1e300, o.z + r.bounds.hi.z}};
    for (double x : {r.bounds.lo.x, r.bounds.hi.x})
        for (double y : {r.bounds.lo.y, r.bounds.hi.y}) {
            const Pose w = ToWorld(o, {x, y, 0, 0});
            b.lo.x = std::fmin(b.lo.x, w.x); b.hi.x = std::fmax(b.hi.x, w.x);
            b.lo.y = std::fmin(b.lo.y, w.y); b.hi.y = std::fmax(b.hi.y, w.y);
        }
    return b;
}
inline bool Overlaps(const Bounds& a, const Bounds& b, double limit) {
    return std::fmin(a.hi.x, b.hi.x) - std::fmax(a.lo.x, b.lo.x) > limit && std::fmin(a.hi.y, b.hi.y) - std::fmax(a.lo.y, b.lo.y) > limit &&
           std::fmin(a.hi.z, b.hi.z) - std::fmax(a.lo.z, b.lo.z) > limit;
}
}  // namespace layout_detail

// Each row's origin in the world, by the join rule. Empty when the rows cannot be placed (see Check).
inline std::vector<Pose> Place(const Layout& l) {
    std::vector<Pose> out;
    if (!l.rows || l.count == 0 || l.count > kMaxRows) return {};
    for (size_t i = 0; i < l.count; ++i) {
        const Row& row = l.rows[i];
        const RoomClass* room = FindRoom(row.room ? row.room : "");
        if (!room) return {};
        if (i == 0) {
            if (row.parent != -1 || row.parentEntrance != -1) return {};
            if (row.ownEntrance == -1) { out.push_back(l.start); continue; }
            if (!layout_detail::EntranceOk(room, row.ownEntrance)) return {};
            out.push_back(JoinAt(l.start, room->entrances[row.ownEntrance]));
            continue;
        }
        if (row.parent < 0 || size_t(row.parent) >= i) return {};
        const RoomClass* parent = FindRoom(l.rows[row.parent].room);
        if (!layout_detail::EntranceOk(parent, row.parentEntrance) || !layout_detail::EntranceOk(room, row.ownEntrance)) return {};
        const Pose start = ToWorld(out[size_t(row.parent)], parent->entrances[row.parentEntrance]);
        out.push_back(JoinAt(start, room->entrances[row.ownEntrance]));
    }
    return out;
}

inline LayoutCheck Check(const Layout& l) {
    LayoutCheck c;
    auto fail = [&c](LayoutError e, int row, int other = -1) { c.error = e; c.row = row; c.other = other; return c; };
    if (!l.rows || l.count == 0 || l.count > kMaxRows) return fail(LayoutError::Empty, -1);
    // used[row] bit e: a join sits on entrance e of that row
    uint8_t used[kMaxRows] = {};
    for (size_t i = 0; i < l.count; ++i) {
        const Row& row = l.rows[i];
        const RoomClass* room = FindRoom(row.room ? row.room : "");
        if (!room) return fail(LayoutError::UnknownRoom, int(i));
        if ((i == 0) != (row.parent == -1)) return fail(LayoutError::BadRoot, int(i));
        if (i == 0) {
            if (row.parentEntrance != -1 || (row.ownEntrance != -1 && !layout_detail::EntranceOk(room, row.ownEntrance))) return fail(LayoutError::BadEntrance, 0);
            continue;
        }
        if (row.parent < 0 || size_t(row.parent) >= i) return fail(LayoutError::BadParent, int(i), row.parent);
        const RoomClass* parent = FindRoom(l.rows[row.parent].room);
        if (!layout_detail::EntranceOk(parent, row.parentEntrance)) return fail(LayoutError::BadEntrance, row.parent, int(i));
        if (!layout_detail::EntranceOk(room, row.ownEntrance)) return fail(LayoutError::BadEntrance, int(i), row.parent);
        const uint8_t pBit = uint8_t(1u << row.parentEntrance), oBit = uint8_t(1u << row.ownEntrance);
        // a child's own entrance is free when it is placed (only later rows join it), so only the parent's is checked
        if (used[row.parent] & pBit) return fail(LayoutError::EntranceReused, row.parent, int(i));
        used[row.parent] |= pBit;
        used[i] |= oBit;
    }
    const std::vector<Pose> at = Place(l);
    if (at.size() != l.count) return fail(LayoutError::BadEntrance, -1);
    for (size_t i = 0; i < l.count; ++i)
        for (size_t j = i + 1; j < l.count; ++j) {
            if (l.rows[j].parent == int(i) || l.rows[i].parent == int(j)) continue;   // a join: they share the doorway
            const Bounds a = layout_detail::WorldBox(*FindRoom(l.rows[i].room), at[i]);
            const Bounds b = layout_detail::WorldBox(*FindRoom(l.rows[j].room), at[j]);
            if (layout_detail::Overlaps(a, b, kOverlapLimit)) return fail(LayoutError::Overlap, int(i), int(j));
        }
    return c;
}

// ------------------------------------------------------------------ joins and open entrances

struct Join {
    uint8_t parent, parentEntrance, child, childEntrance;
    Pose at;   // the joint in the world: the parent's entrance
};
inline std::vector<Join> Joins(const Layout& l, const std::vector<Pose>& origins) {
    std::vector<Join> out;
    if (origins.size() != l.count) return out;
    for (size_t i = 1; i < l.count; ++i) {
        const Row& row = l.rows[i];
        const RoomClass* parent = FindRoom(l.rows[row.parent].room);
        out.push_back({uint8_t(row.parent), uint8_t(row.parentEntrance), uint8_t(i), uint8_t(row.ownEntrance),
                       ToWorld(origins[size_t(row.parent)], parent->entrances[row.parentEntrance])});
    }
    return out;
}

struct OpenEntrance {
    uint8_t row, entrance;
    Pose at;   // the entrance in the world
};
// Every entrance no join uses (the root's own entrance included: it sits on the start and leads nowhere).
inline std::vector<OpenEntrance> OpenEntrances(const Layout& l, const std::vector<Pose>& origins) {
    std::vector<OpenEntrance> out;
    if (origins.size() != l.count) return out;
    uint8_t used[kMaxRows] = {};
    for (size_t i = 1; i < l.count; ++i) {
        used[l.rows[i].parent] |= uint8_t(1u << l.rows[i].parentEntrance);
        used[i] |= uint8_t(1u << l.rows[i].ownEntrance);
    }
    for (size_t i = 0; i < l.count; ++i) {
        const RoomClass* room = FindRoom(l.rows[i].room);
        for (uint8_t e = 0; e < room->entranceCount; ++e)
            if (!(used[i] & (1u << e))) out.push_back({uint8_t(i), e, ToWorld(origins[i], room->entrances[e])});
    }
    return out;
}

inline int SpawnCount(const Layout& l) {
    int n = 0;
    for (size_t i = 0; i < l.count; ++i) if (const RoomClass* r = FindRoom(l.rows[i].room)) n += r->spawns;
    return n;
}

// ------------------------------------------------------------------ names

enum class Part : uint8_t { Module, Lighting, Logic };
inline const char* PartPath(const RoomClass& r, Part p) { return p == Part::Module ? r.module : p == Part::Lighting ? r.lighting : r.logic; }

// "HSMCrypts_<layout>_<row>_<M|L|G>": the name the loader gives a streamed level (OptionalLevelNameOverride).
inline std::string InstanceName(const Layout& l, size_t row, Part p) {
    return std::string(kInstancePrefix) + l.name + "_" + std::to_string(row) + "_" + (p == Part::Module ? "M" : p == Part::Lighting ? "L" : "G");
}
inline std::string AmbientInstanceName(const Layout& l) { return std::string(kInstancePrefix) + l.name + "_Ambient"; }
// The co-op Crypts (the host's rooms, numbered 1.. in the order the host saw them): "HSMCrypts_Coop_<seq>_<M|L|G>" and
// "HSMCrypts_Coop_Ambient". The names carry no layout, so none of them is a table's (the test below keeps the two sets apart).
inline std::string CoopInstanceName(unsigned seq, Part p) {
    return std::string(kInstancePrefix) + "Coop_" + std::to_string(seq) + "_" + (p == Part::Module ? "M" : p == Part::Lighting ? "L" : "G");
}
inline std::string CoopAmbientInstanceName() { return std::string(kInstancePrefix) + "Coop_Ambient"; }

// A package the loader streamed: its name's last path segment starts with kInstancePrefix.
inline bool IsInstancePackage(std::string_view package) {
    const size_t slash = package.rfind('/');
    const std::string_view last = slash == std::string_view::npos ? package : package.substr(slash + 1);
    return last.size() > kInstancePrefix.size() && last.substr(0, kInstancePrefix.size()) == kInstancePrefix;
}

// The shared dice's name for an actor's level (gameplay/shared_dice.cpp): the world's name, except for a level the loader
// streamed, whose world keeps its asset's name in every copy (Descent streams the Corridor_3 level three times) while its
// package carries the instance name.
inline std::string_view DiceLevelName(std::string_view worldName, std::string_view packageName) {
    if (!IsInstancePackage(packageName)) return worldName;
    const size_t slash = packageName.rfind('/');
    return slash == std::string_view::npos ? packageName : packageName.substr(slash + 1);
}


// ------------------------------------------------------------------ building a table

// Not used yet. The co-op Crypts (docs/PLAN_CRYPTS_COOP.md) build a room table from what the host's director placed: BuildRooms
// takes those rows (a root, then rooms joined to earlier rows), refuses a table Check does not accept, and returns each row's
// origin in the world (Place's answer; empty when refused).
inline std::vector<Pose> BuildRooms(const std::vector<Row>& rows) {
    if (rows.empty() || rows.size() > kMaxRows) return {};
    const Layout l{"Crypts", "Crypts", {}, rows.data(), uint8_t(rows.size())};
    if (!Check(l).ok()) return {};
    return Place(l);
}

}  // namespace hsm::crypts

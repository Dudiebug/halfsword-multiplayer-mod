// Unit tests for hsm_crypts_layout.h: the Crypts' rooms and the join rule the co-op Crypts build on (docs/PLAN_CRYPTS_COOP.md).
// Run via implementation/shared/run_tests.sh test_hsm_crypts_layout.
#include "match/hsm_crypts_layout.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <set>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
#define CHECKF(c, i) do { if (!(c)) { std::printf("FAIL %s:%d: %s (case %d)\n", __FILE__, __LINE__, #c, int(i)); ++g_fail; } } while (0)

using namespace hsm::crypts;

// The walked run of the 2026-11-13 probe log, a lone room and a lone hall: tables built by hand (the lobby has no layout maps now).
static const Row kDescentRows[] = {
    {"Entrance_2", -1, -1, 0},
    {"Corridor_3", 0, 1, 0},
    {"Corridor_1_Shortcut", 0, 2, 1},
    {"Chamber_1", 1, 1, 1},
    {"Stairs_1_Shortcut", 2, 0, 0},
    {"Corridor_1", 3, 0, 0},
    {"Chamber_5", 5, 1, 1},
    {"Stairs_2", 6, 0, 0},
    {"Hall_1", 7, 1, 3},
    {"Treasury_1", 8, 0, 0},
    {"Corridor_3_Exit", 8, 2, 0},
};
static const Row kSepulchreRows[] = {{"Sepulchre", -1, -1, -1}};
static const Row kHallRows[] = {{"Hall_1", -1, -1, -1}};
static const Layout kDescent{"Descent", "Descent", {}, kDescentRows, uint8_t(sizeof(kDescentRows) / sizeof(Row))};
static const Layout kSepulchre{"Purgatory", "Purgatory", {}, kSepulchreRows, 1};
static const Layout kHall{"BossHall", "Boss Hall", {}, kHallRows, 1};

static bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
static bool SameYaw(double a, double b, double tol) { return std::fabs(WrapYaw(a - b)) <= tol; }
static bool SamePose(const Pose& a, const Pose& b, double tol) {
    return Near(a.x, b.x, tol) && Near(a.y, b.y, tol) && Near(a.z, b.z, tol) && SameYaw(a.yaw, b.yaw, 0.01);
}

// The catalog: 23 room classes, unique, every entrance on a right angle with its levels in the Crypts' folder.
static void TestCatalog() {
    CHECK(kRoomCount == 23);
    std::set<std::string> names;
    for (const RoomClass& r : kRooms) {
        names.insert(r.name);
        CHECK(FindRoom(r.name) == &r);
        CHECK(r.entranceCount >= 1 && r.entranceCount <= kMaxEntrances);
        for (uint8_t e = 0; e < r.entranceCount; ++e) {
            const double y = r.entrances[e].yaw;
            CHECK(y == 0 || y == 90 || y == -90 || y == 180);
        }
        CHECK(r.bounds.lo.x < r.bounds.hi.x && r.bounds.lo.y < r.bounds.hi.y && r.bounds.lo.z < r.bounds.hi.z);
        for (const char* p : {r.module, r.lighting, r.logic}) {
            const std::string s = p;
            if (s.empty()) { CHECK(p == r.logic); continue; }   // only the Logic level may be missing (Treasury_1)
            CHECK(s.rfind("/Game/Maps/Test/ProceduralBlocksTest/", 0) == 0 && s.find('.') == std::string::npos);
        }
    }
    CHECK(names.size() == kRoomCount);
    CHECK(!FindRoom("") && !FindRoom("chamber_1") && !FindRoom("Chamber_9"));
    // spot checks against the probe log
    const RoomClass* c1 = FindRoom("Chamber_1");
    CHECK(c1 && c1->entranceCount == 3 && c1->entrances[0].x == -1760 && c1->entrances[0].y == 1180 && c1->entrances[0].yaw == 90);
    CHECK(c1 && c1->entrances[2].x == 1890 && c1->entrances[2].yaw == 0 && c1->spawns == 17);
    const RoomClass* sep = FindRoom("Sepulchre");
    CHECK(sep && sep->entranceCount == 1 && sep->entrances[0].x == -1350 && sep->entrances[0].z == 120 && sep->spawns == 31);
    CHECK(sep && std::string(sep->module) == "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Treasury_1");
    const RoomClass* hall = FindRoom("Hall_1");
    CHECK(hall && hall->entranceCount == 4 && hall->entrances[3].x == 2410 && hall->entrances[3].y == 1380 && hall->spawns == 18);
    const RoomClass* ent = FindRoom("Entrance_2");
    CHECK(ent && std::string(ent->module).find("Module_Exit_2") != std::string::npos && ent->entrances[0].z == -440);
    CHECK(std::string(FindRoom("Corridor_3_Exit")->module) == FindRoom("Corridor_3")->module);   // one level, three classes
    CHECK(std::string(FindRoom("Treasury_1")->logic).empty());
}

// The join rule, by hand: a straight join and a turned one.
static void TestJoinByHand() {
    // Corridor_3's e0 (-980,0,0) yaw 180 on a start at (100,200,10) yaw 0: the corridor runs on along +x.
    Pose o = JoinAt({100, 200, 10, 0}, FindRoom("Corridor_3")->entrances[0]);
    CHECK(SamePose(o, {1080, 200, 10, 0}, 1e-6));
    // Chamber_1's e0 (-1760,1180,0) yaw 90 on a start at the origin facing +x: the chamber turns by 90.
    o = JoinAt({0, 0, 0, 0}, FindRoom("Chamber_1")->entrances[0]);
    CHECK(SamePose(o, {1180, 1760, 0, 90}, 1e-6));
    const Pose back = ToWorld(o, FindRoom("Chamber_1")->entrances[0]);
    CHECK(SamePose(back, {0, 0, 0, 180}, 1e-6));   // its entrance on the start, facing back through it
    // A start below the origin: z carries straight through (Entrance_2's e0 at z -440 lifts the room by 440).
    o = JoinAt({0, 0, 0, 0}, FindRoom("Entrance_2")->entrances[0]);
    CHECK(SamePose(o, {420, 250, 440, 0}, 1e-6));
    CHECK(Near(WrapYaw(540), -180, 1e-9) && Near(WrapYaw(180), -180, 1e-9) && Near(WrapYaw(-190), 170, 1e-9) && Near(WrapYaw(-180), -180, 1e-9) && Near(WrapYaw(0), 0, 1e-9));
}

// The join rule for every catalog class and every pair of entrances: the child's entrance lands on the parent's,
// facing back through it.
static void TestJoinEveryPair() {
    const Pose parent{1234, -567, 89, 90};
    const RoomClass* hall = FindRoom("Hall_1");
    int n = 0;
    for (const RoomClass& r : kRooms)
        for (uint8_t i = 0; i < hall->entranceCount; ++i)
            for (uint8_t j = 0; j < r.entranceCount; ++j) {
                const Pose start = ToWorld(parent, hall->entrances[i]);
                const Pose child = JoinAt(start, r.entrances[j]);
                const Pose mine = ToWorld(child, r.entrances[j]);
                CHECKF(Near(mine.x, start.x, 1e-6) && Near(mine.y, start.y, 1e-6) && Near(mine.z, start.z, 1e-6), n);
                CHECKF(SameYaw(mine.yaw, start.yaw + 180, 1e-6), n);
                CHECKF(SameYaw(child.yaw, start.yaw - r.entrances[j].yaw + 180, 1e-6), n);
                ++n;
            }
    CHECK(n > 0);
}

// The whole walked run of the probe log: every room at its logged origin and yaw, to within 2 uu.
static void TestDescentChain() {
    const Layout* d = &kDescent;
    CHECK(d->count == 11);
    struct Logged { const char* room; Pose origin; };
    const Logged logged[] = {
        {"Entrance_2", {420, 250, 440, 0}},           {"Corridor_3", {3010, 50, -450, 0}},
        {"Corridor_1_Shortcut", {1820, -2000, -750, 90}}, {"Chamber_1", {5040, 640, -450, -90}},
        {"Stairs_1_Shortcut", {1870, -3990, -620, -90}}, {"Corridor_1", {7200, 2400, -450, 0}},
        {"Chamber_5", {9340, 2150, -450, -90}},       {"Stairs_2", {9250, 4620, -470, 90}},
        {"Hall_1", {7870, 7640, -1050, -90}},         {"Treasury_1", {11220, 7540, -515, 0}},
        {"Corridor_3_Exit", {4410, 7640, -900, 180}},
    };
    const std::vector<Pose> at = Place(*d);
    CHECK(at.size() == 11);
    for (size_t i = 0; i < at.size() && i < 11; ++i) {
        CHECKF(std::strcmp(d->rows[i].room, logged[i].room) == 0, i);
        CHECKF(SamePose(at[i], logged[i].origin, 2.0), i);
    }
    const LayoutCheck c = Check(*d);
    CHECK(c.ok());   // Entrance_2 and Corridor_1_Shortcut overlap by 9x10x6 m, but they join
    const std::vector<Join> joins = Joins(*d, at);
    CHECK(joins.size() == 10);
    for (const Join& j : joins) {   // each joint is also the child's own entrance
        const Pose mine = ToWorld(at[j.child], FindRoom(d->rows[j.child].room)->entrances[j.childEntrance]);
        CHECKF(Near(mine.x, j.at.x, 1e-6) && Near(mine.y, j.at.y, 1e-6) && Near(mine.z, j.at.z, 1e-6), j.child);
        CHECKF(j.parent == uint8_t(d->rows[j.child].parent) && j.parentEntrance == uint8_t(d->rows[j.child].parentEntrance), j.child);
    }
}

// The entrances that lead nowhere, which the loader bars.
static void TestOpenEntrances() {
    const Layout* d = &kDescent;
    const std::vector<OpenEntrance> open = OpenEntrances(*d, Place(*d));
    // row, entrance: Entrance_2 e0 and e3, Chamber_1 e2, Stairs_1_Shortcut e1 and e2, Corridor_1 e2, Chamber_5 e2, Hall_1 e1,
    // Corridor_3_Exit e1
    const std::set<std::pair<int, int>> want = {{0, 0}, {0, 3}, {3, 2}, {4, 1}, {4, 2}, {5, 2}, {6, 2}, {8, 1}, {10, 1}};
    std::set<std::pair<int, int>> got;
    const std::vector<Pose> at = Place(*d);
    for (const OpenEntrance& o : open) {
        got.insert({o.row, o.entrance});
        CHECKF(SamePose(o.at, ToWorld(at[o.row], FindRoom(d->rows[o.row].room)->entrances[o.entrance]), 1e-9), o.row);
    }
    CHECK(open.size() == 9 && got == want);
    // The root's own entrance sits on the start: Descent's e0 at the placed block's (0,0,0), facing back.
    for (const OpenEntrance& o : open) if (o.row == 0 && o.entrance == 0) CHECK(SamePose(o.at, {0, 0, 0, 180}, 1e-6));
    const Layout* p = &kSepulchre;
    const std::vector<OpenEntrance> po = OpenEntrances(*p, Place(*p));
    CHECK(po.size() == 1 && po[0].row == 0 && po[0].entrance == 0 && SamePose(po[0].at, {-1350, 0, 120, 180}, 1e-9));
    const Layout* b = &kHall;
    CHECK(OpenEntrances(*b, Place(*b)).size() == 4 && Joins(*b, Place(*b)).empty());
    CHECK(OpenEntrances(*b, {}).empty() && Joins(*d, {}).empty());   // origins that do not fit the layout: nothing
}

// The hand-built tables check out, place their root on the start and count their rooms' spawn points.
static void TestHandBuilt() {
    for (const Layout* l : {&kDescent, &kSepulchre, &kHall}) {
        CHECK(Check(*l).ok());
        CHECK(Place(*l).size() == l->count && l->count <= kMaxRows);
        for (const char* c = l->name; *c; ++c) CHECK((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'));
    }
    CHECK(SamePose(Place(kSepulchre)[0], {0, 0, 0, 0}, 0) && SamePose(Place(kHall)[0], {0, 0, 0, 0}, 0));
    CHECK(SpawnCount(kSepulchre) == 31 && SpawnCount(kHall) == 18);
    int descent = 0;
    for (size_t i = 0; i < 11; ++i) descent += FindRoom(kDescentRows[i].room)->spawns;
    CHECK(SpawnCount(kDescent) == descent && descent > 31);
}

// BuildRooms is the entry the co-op Crypts will call with the rows the host's director placed: the same answer as Place for a table
// Check accepts, nothing for one it refuses. Nothing calls it yet.
static void TestBuildRooms() {
    const std::vector<Row> rows(std::begin(kDescentRows), std::end(kDescentRows));
    const std::vector<Pose> built = BuildRooms(rows);
    const std::vector<Pose> placed = Place(kDescent);
    CHECK(built.size() == 11 && built.size() == placed.size());
    for (size_t i = 0; i < built.size() && i < placed.size(); ++i) CHECKF(SamePose(built[i], placed[i], 1e-9), i);
    CHECK(BuildRooms({}).empty());
    CHECK(BuildRooms({{"Hall_1", -1, -1, -1}, {"Kitchen", 0, 0, 0}}).empty());                 // an unknown room
    CHECK(BuildRooms({{"Entrance_2", -1, -1, 0}, {"Corridor_3", 0, 1, 0}, {"Corridor_3", 0, 1, 0}}).empty());   // an entrance used twice
    CHECK(BuildRooms(std::vector<Row>(kMaxRows + 1, Row{"Hall_1", -1, -1, -1})).empty());      // too many rows
    CHECK(BuildRooms({{"Hall_1", -1, -1, -1}}).size() == 1);
}

// Tables Check refuses.
static void TestCheckRefusals() {
    auto check = [](const Row* rows, uint8_t n) { return Check(Layout{"T", "T", {}, rows, n}); };
    // an entrance two joins use: rows 1 and 2 both on the root's e1
    Row reused[] = {{"Entrance_2", -1, -1, 0}, {"Corridor_3", 0, 1, 0}, {"Corridor_3", 0, 1, 0}};
    LayoutCheck c = check(reused, 3);
    CHECK(c.error == LayoutError::EntranceReused && c.row == 0 && c.other == 2);
    // a room's own entrance taken again by its child: caught as its parent entrance
    Row ownReused[] = {{"Corridor_3", -1, -1, -1}, {"Corridor_1", 0, 1, 0}, {"Traps_1", 1, 0, 0}};
    c = check(ownReused, 3);
    CHECK(c.error == LayoutError::EntranceReused && c.row == 1 && c.other == 2);
    // missing indices
    Row badParentEntrance[] = {{"Hall_1", -1, -1, -1}, {"Corridor_3", 0, 4, 0}};
    CHECK(check(badParentEntrance, 2).error == LayoutError::BadEntrance);
    Row badOwnEntrance[] = {{"Hall_1", -1, -1, -1}, {"Corridor_3", 0, 0, 2}};
    c = check(badOwnEntrance, 2);
    CHECK(c.error == LayoutError::BadEntrance && c.row == 1);
    Row badRootEntrance[] = {{"Sepulchre", -1, -1, 1}};
    CHECK(check(badRootEntrance, 1).error == LayoutError::BadEntrance && Place(Layout{"T", "T", {}, badRootEntrance, 1}).empty());
    Row negative[] = {{"Hall_1", -1, -1, -1}, {"Corridor_3", 0, -1, 0}};
    CHECK(check(negative, 2).error == LayoutError::BadEntrance && Place(Layout{"T", "T", {}, negative, 2}).empty());
    // unknown rooms and broken trees
    Row unknown[] = {{"Hall_1", -1, -1, -1}, {"Kitchen", 0, 0, 0}};
    c = check(unknown, 2);
    CHECK(c.error == LayoutError::UnknownRoom && c.row == 1);
    Row rootWithParent[] = {{"Hall_1", 0, 0, 0}};
    CHECK(check(rootWithParent, 1).error == LayoutError::BadRoot);
    Row secondRoot[] = {{"Hall_1", -1, -1, -1}, {"Corridor_3", -1, -1, -1}};
    c = check(secondRoot, 2);
    CHECK(c.error == LayoutError::BadRoot && c.row == 1);
    Row selfParent[] = {{"Hall_1", -1, -1, -1}, {"Corridor_3", 1, 0, 0}};
    CHECK(check(selfParent, 2).error == LayoutError::BadParent && Place(Layout{"T", "T", {}, selfParent, 2}).empty());
    Row laterParent[] = {{"Hall_1", -1, -1, -1}, {"Corridor_3", 2, 0, 0}, {"Corridor_3", 0, 0, 0}};
    CHECK(check(laterParent, 3).error == LayoutError::BadParent);
    CHECK(check(nullptr, 0).error == LayoutError::Empty && check(kDescentRows, 0).error == LayoutError::Empty);
    CHECK(check(kDescentRows, uint8_t(kMaxRows + 1)).error == LayoutError::Empty);
    // two halls on one corridor's two side doors: they do not join each other, and they overlap
    Row overlap[] = {{"Corridor_1", -1, -1, -1}, {"Hall_1", 0, 1, 0}, {"Hall_1", 0, 2, 0}};
    c = check(overlap, 3);
    CHECK(c.error == LayoutError::Overlap && c.row == 1 && c.other == 2);
    Row fine[] = {{"Corridor_1", -1, -1, -1}, {"Hall_1", 0, 1, 0}, {"Corridor_3", 0, 2, 0}};
    CHECK(check(fine, 3).ok());
}

// Touching is allowed; an overlap counts only past 1 m on every axis.
static void TestOverlapEdge() {
    const Bounds a{{0, 0, 0}, {1000, 1000, 1000}};
    auto at = [](double x, double y, double z) { return Bounds{{x, y, z}, {x + 1000, y + 1000, z + 1000}}; };
    CHECK(!layout_detail::Overlaps(a, at(900, 900, 900), kOverlapLimit));    // exactly 1 m each way
    CHECK(layout_detail::Overlaps(a, at(899, 899, 899), kOverlapLimit));
    CHECK(!layout_detail::Overlaps(a, at(0, 0, 901), kOverlapLimit));        // 0.99 m on z only
    CHECK(!layout_detail::Overlaps(a, at(0, 901, 0), kOverlapLimit) && !layout_detail::Overlaps(a, at(901, 0, 0), kOverlapLimit));
    CHECK(!layout_detail::Overlaps(a, at(0, 0, 900), kOverlapLimit) && !layout_detail::Overlaps(a, at(0, 900, 0), kOverlapLimit));   // 1 m on one axis
    CHECK(!layout_detail::Overlaps(a, at(900, 0, 0), kOverlapLimit) && layout_detail::Overlaps(a, at(0, 0, 899), kOverlapLimit));
    CHECK(layout_detail::Overlaps(a, at(-500, -500, -500), kOverlapLimit));
    // a turned box: Corridor_3 turned by 90 swaps its x and y extents
    const RoomClass& c3 = *FindRoom("Corridor_3");
    const Bounds w = layout_detail::WorldBox(c3, {100, 0, 50, 90});
    CHECK(Near(w.lo.x, 100 - c3.bounds.hi.y, 1e-6) && Near(w.hi.x, 100 - c3.bounds.lo.y, 1e-6));
    CHECK(Near(w.lo.y, c3.bounds.lo.x, 1e-6) && Near(w.hi.y, c3.bounds.hi.x, 1e-6));
    CHECK(Near(w.lo.z, 50 + c3.bounds.lo.z, 1e-9) && Near(w.hi.z, 50 + c3.bounds.hi.z, 1e-9));
}

// Instance names: the same on every machine, unique within a layout; the dice key's level name.
static void TestNames() {
    const Layout& d = kDescent;
    CHECK(InstanceName(d, 3, Part::Module) == "HSMCrypts_Descent_3_M");
    CHECK(InstanceName(d, 10, Part::Lighting) == "HSMCrypts_Descent_10_L");
    CHECK(InstanceName(d, 0, Part::Logic) == "HSMCrypts_Descent_0_G");
    CHECK(AmbientInstanceName(d) == "HSMCrypts_Descent_Ambient");
    std::set<std::string> all;
    for (const Layout& l : {kDescent, kSepulchre, kHall}) {
        all.insert(AmbientInstanceName(l));
        for (size_t i = 0; i < l.count; ++i)
            for (Part p : {Part::Module, Part::Lighting, Part::Logic}) all.insert(InstanceName(l, i, p));
    }
    CHECK(all.size() == 3 + 3 * (11 + 1 + 1));
    // the co-op Crypts' names (the host's rooms by seq): their own set, never a table's, and each an instance package
    std::set<std::string> coop{CoopAmbientInstanceName()};
    for (unsigned q = 1; q <= 96; ++q)
        for (Part p : {Part::Module, Part::Lighting, Part::Logic}) coop.insert(CoopInstanceName(q, p));
    CHECK(coop.size() == 1 + 96 * 3);
    for (const std::string& s : coop) CHECK(all.count(s) == 0 && IsInstancePackage("/Game/Maps/Test/ProceduralBlocksTest/" + s));
    CHECK(CoopInstanceName(7, Part::Module) == "HSMCrypts_Coop_7_M" && CoopInstanceName(12, Part::Logic) == "HSMCrypts_Coop_12_G");
    CHECK(CoopAmbientInstanceName() == "HSMCrypts_Coop_Ambient");
    for (const std::string& s : all) CHECK(IsInstancePackage("/Game/Maps/Test/ProceduralBlocksTest/" + s) && IsInstancePackage(s));
    CHECK(!IsInstancePackage("/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_3"));
    CHECK(!IsInstancePackage("HSMCrypts_") && !IsInstancePackage("") && !IsInstancePackage("/Game/HSMCrypts_x/Map_A"));
    CHECK(!IsInstancePackage("/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_3_LevelInstance_7"));
    CHECK(std::string(PartPath(*FindRoom("Chamber_1"), Part::Lighting)).find("Chamber_1_Lighting") != std::string::npos);
    CHECK(std::string(PartPath(*FindRoom("Chamber_1"), Part::Logic)).find("Chamber_1_Arena_Logic") != std::string::npos);
    // dice: a streamed copy keys by its package's instance name; every other level keeps its world's name
    CHECK(DiceLevelName("Map_Procedural_Block_Test_Module_Corridor_3", "/Game/Maps/Test/ProceduralBlocksTest/HSMCrypts_Descent_1_M") == "HSMCrypts_Descent_1_M");
    CHECK(DiceLevelName("Map_Procedural_Block_Test_Module_Corridor_3", "/Game/Maps/Test/ProceduralBlocksTest/HSMCrypts_Descent_10_M") == "HSMCrypts_Descent_10_M");
    CHECK(DiceLevelName("Map_Arena_Yard", "/Game/Maps/Arenas/Map_Arena_Yard") == "Map_Arena_Yard");
    CHECK(DiceLevelName("Map_Procedural_Block_Test_Module_Corridor_3", "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Module_Corridor_3_LevelInstance_3") == "Map_Procedural_Block_Test_Module_Corridor_3");
}

// The base level is the bare Crypts level; the old "<level>#<layout>" tags have no helpers left.
static void TestBaseLevel() {
    CHECK(std::string(kBaseLevel) == "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main");
}
int main() {
    TestCatalog();
    TestJoinByHand();
    TestJoinEveryPair();
    TestDescentChain();
    TestOpenEntrances();
    TestHandBuilt();
    TestBuildRooms();
    TestCheckRefusals();
    TestOverlapEdge();
    TestNames();
    TestBaseLevel();
    if (g_fail) { std::printf("test_hsm_crypts_layout: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypts_layout: all passed\n");
    return 0;
}

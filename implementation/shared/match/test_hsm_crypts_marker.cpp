// Unit tests for the Crypts key marker (match/hsm_crypts_marker.h): the key classes, the nearest locked door, its label,
// the direction word, and the line under the Crypts pill. Run via implementation/shared/run_tests.sh test_hsm_crypts_marker.
#include "match/hsm_crypts_marker.h"
#include <cmath>
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }

static void TestKeyClass() {
    CHECK(IsCryptsKeyClass("BP_Weapon_ToolKey_C") && IsCryptsKeyClass("BP_Loot_Key_C"));
    CHECK(!IsCryptsKeyClass("") && !IsCryptsKeyClass("BP_Weapon_ToolKey") && !IsCryptsKeyClass("BP_Loot_Key_C2") && !IsCryptsKeyClass("BP_Weapon_Sword_C"));
}

static void TestNearest() {
    CHECK(NearestLockedDoor({}, 0, 0, 0) == -1);
    std::vector<DoorSpot> d = {{100, 0, 0, false}, {500, 0, 0, true}, {-300, 0, 0, true}, {0, 0, 50, false}};
    CHECK(NearestLockedDoor(d, 0, 0, 0) == 2);                   // 300 cm beats 500; the open ones nearer are skipped
    CHECK(NearestLockedDoor(d, 400, 0, 0) == 1);
    CHECK(NearestLockedDoor(d, 0, 0, 1000) == 2);                // from above: 1044 vs 1118
    std::vector<DoorSpot> stairs = {{400, 0, 0, true}, {100, 0, 1000, true}};
    CHECK(NearestLockedDoor(stairs, 0, 0, 0) == 0);              // height counts (a floor up): 400 beats 1005
    CHECK(NearestLockedDoor(d, 0, 300, 0) == 2);                 // 424 vs 583
    d[1].locked = d[2].locked = false;
    CHECK(NearestLockedDoor(d, 0, 0, 0) == -1);                  // none locked
    const double nan = std::numeric_limits<double>::quiet_NaN();
    std::vector<DoorSpot> n = {{nan, 0, 0, true}, {900, 0, 0, true}};
    CHECK(NearestLockedDoor(n, 0, 0, 0) == 1);                   // a NaN place is never picked
    std::vector<DoorSpot> first = {{200, 0, 0, true}, {200, 0, 0, true}};
    CHECK(NearestLockedDoor(first, 0, 0, 0) == 0);               // equal: the first
    std::vector<DoorSpot> later = {{900, 0, 0, true}, {200, 0, 0, true}, {200, 0, 0, true}};
    CHECK(NearestLockedDoor(later, 0, 0, 0) == 1);               // nearer later in the list, then the first of equals
}

static void TestLabel() {
    CHECK(LockedDoorLabel(4200) == "Locked door 42 m");
    CHECK(LockedDoorLabel(4249) == "Locked door 42 m" && LockedDoorLabel(4250) == "Locked door 43 m");
    CHECK(LockedDoorLabel(0) == "Locked door 0 m" && LockedDoorLabel(49) == "Locked door 0 m" && LockedDoorLabel(-500) == "Locked door 0 m");
    CHECK(LockedDoorLabel(1e12) == "Locked door 99999 m");
    CHECK(std::string(kNoLockedDoorLine) == "Locked room not found yet");                  // the chip's words; the notice says the rest
}

// The notice for a chip with no locked door goes up once a run: when the chip first reads the no-door words, not before.
static void TestKeyLineNotice() {
    CHECK(KeyLineNeedsNotice(kNoLockedDoorLine, false));
    CHECK(!KeyLineNeedsNotice(kNoLockedDoorLine, true));                                    // told already this run
    CHECK(!KeyLineNeedsNotice("Locked door 42 m, ahead-left", false));                      // a door is known: nothing to explain
    CHECK(!KeyLineNeedsNotice("", false) && !KeyLineNeedsNotice("", true));                 // no key held: no chip
    CHECK(!KeyLineNeedsNotice("Locked room not found yet: it shows once its room is built", false));   // only the chip's own words
    CHECK(KeyLineNeedsNotice(KeyMarkerLine(true, {}, 0, 0, 0, 0), false) && !KeyLineNeedsNotice(KeyMarkerLine(false, {}, 0, 0, 0, 0), false));
    CHECK(!KeyLineNeedsNotice(KeyMarkerLine(true, {{4200, 0, 0, true}}, 0, 0, 0, 0), false));
}

static void TestYaw() {
    CHECK(Near(RelativeYawDeg(0, 0, 0, 100, 0), 0));             // straight ahead
    CHECK(Near(RelativeYawDeg(0, 0, 0, 0, 100), 90));            // UE: +Y is to the right of +X
    CHECK(Near(RelativeYawDeg(0, 0, 0, 0, -100), -90));
    CHECK(Near(std::fabs(RelativeYawDeg(0, 0, 0, -100, 0)), 180));
    CHECK(Near(RelativeYawDeg(0, 0, 90, 0, 100), 0));            // the camera turned to face it
    CHECK(Near(RelativeYawDeg(0, 0, 350, 100, 10), 15.71f));     // wraps across 0
    CHECK(Near(RelativeYawDeg(0, 0, -720, 100, 0), 0));
    CHECK(Near(RelativeYawDeg(50, 50, 0, 150, 150), 45));        // from the eye, not the origin
    for (double cam : {-540.0, -90.0, 0.0, 33.0, 179.0, 720.0}) {
        const float r = RelativeYawDeg(10, 20, cam, -50, 70);
        CHECK(r >= -180.f && r <= 180.f);
    }
}

static void TestDirection() {
    CHECK(std::string(DirectionWord(0)) == "ahead" && std::string(DirectionWord(22.5f)) == "ahead" && std::string(DirectionWord(-22.5f)) == "ahead");
    CHECK(std::string(DirectionWord(22.6f)) == "ahead-right" && std::string(DirectionWord(67.5f)) == "ahead-right");
    CHECK(std::string(DirectionWord(-22.6f)) == "ahead-left" && std::string(DirectionWord(-67.5f)) == "ahead-left");
    CHECK(std::string(DirectionWord(67.6f)) == "right" && std::string(DirectionWord(90)) == "right" && std::string(DirectionWord(112.5f)) == "right");
    CHECK(std::string(DirectionWord(-67.6f)) == "left" && std::string(DirectionWord(-112.5f)) == "left");
    CHECK(std::string(DirectionWord(112.6f)) == "behind-right" && std::string(DirectionWord(157.5f)) == "behind-right");
    CHECK(std::string(DirectionWord(-112.6f)) == "behind-left" && std::string(DirectionWord(-157.5f)) == "behind-left");
    CHECK(std::string(DirectionWord(157.6f)) == "behind" && std::string(DirectionWord(180)) == "behind" && std::string(DirectionWord(-180)) == "behind");
    CHECK(std::string(DirectionWord(std::numeric_limits<float>::quiet_NaN())).empty());
}

static void TestLine() {
    std::vector<DoorSpot> d = {{4200, 0, 0, true}, {0, 900, 0, false}};
    CHECK(KeyMarkerLine(false, d, 0, 0, 0, 0).empty());                                  // no key: no line
    CHECK(KeyMarkerLine(true, d, 0, 0, 0, 0) == "Locked door 42 m, ahead");
    CHECK(KeyMarkerLine(true, d, 0, 0, 0, 90) == "Locked door 42 m, left");              // facing +Y, the door is to the left
    CHECK(KeyMarkerLine(true, d, 0, 0, 0, -135) == "Locked door 42 m, behind-right");
    CHECK(KeyMarkerLine(true, d, 4200, -3000, 0, 0) == "Locked door 30 m, right");       // distance from the eye
    CHECK(KeyMarkerLine(true, d, 0, 0, 0, 30) == "Locked door 42 m, ahead-left");
    CHECK(KeyMarkerLine(true, {{0, 0, 4000, true}}, 0, 0, 1000, 0) == "Locked door 30 m, ahead");   // straight above: ahead (atan2 0, 0)
    d[0].locked = false;
    CHECK(KeyMarkerLine(true, d, 0, 0, 0, 0) == kNoLockedDoorLine);                      // no locked door known
    CHECK(KeyMarkerLine(true, {}, 0, 0, 0, 0) == kNoLockedDoorLine);
    CHECK(KeyMarkerLine(false, {}, 0, 0, 0, 0).empty());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(KeyMarkerLine(true, {{1000, 0, 0, true}}, 0, 0, 0, nan) == "Locked door 10 m"); // a NaN view: no direction word
}

int main() {
    TestKeyClass();
    TestNearest();
    TestLabel();
    TestKeyLineNotice();
    TestYaw();
    TestDirection();
    TestLine();
    if (g_fail) { std::printf("test_hsm_crypts_marker: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypts_marker: ok\n");
    return 0;
}

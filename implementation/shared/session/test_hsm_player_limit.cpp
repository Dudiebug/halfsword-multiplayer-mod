// Unit tests for hsm_player_limit.h. Run via implementation/shared/run_tests.sh test_hsm_player_limit.
#include "session/hsm_player_limit.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static void TestClamp() {
    CHECK(ClampPlayerLimit(4, 1) == 4);
    CHECK(ClampPlayerLimit(1, 1) == 2 && ClampPlayerLimit(0, 0) == 2 && ClampPlayerLimit(-5, 1) == 2);   // never below 2
    CHECK(ClampPlayerLimit(9, 1) == 8 && ClampPlayerLimit(99, 8) == 8);                                   // never above 8
    CHECK(ClampPlayerLimit(2, 5) == 5 && ClampPlayerLimit(4, 5) == 5 && ClampPlayerLimit(6, 5) == 6);     // not below the players in
    CHECK(ClampPlayerLimit(2, 8) == 8 && ClampPlayerLimit(3, 20) == 8);                                   // a lobby of 8 stays at 8
    CHECK(ClampPlayerLimit(2, 0) == 2 && ClampPlayerLimit(2, -1) == 2);
}

static void TestParse() {
    CHECK(ParsePlayerLimit("") == 8 && ParsePlayerLimit("abc") == 8 && ParsePlayerLimit("4x") == 8 && ParsePlayerLimit("-3") == 8);   // absent: the old 8
    CHECK(ParsePlayerLimit("4") == 4 && ParsePlayerLimit("2") == 2 && ParsePlayerLimit("8") == 8);
    CHECK(ParsePlayerLimit("1") == 2 && ParsePlayerLimit("0") == 2 && ParsePlayerLimit("9") == 8 && ParsePlayerLimit("999") == 8);
    CHECK(ParsePlayerLimit("1000") == 8 && ParsePlayerLimit("99999999999") == 8);
}

static void TestStep() {
    CHECK(StepPlayerLimit(4, 1, 1) == 5 && StepPlayerLimit(4, 1, -1) == 3);
    CHECK(StepPlayerLimit(8, 1, 1) == 2 && StepPlayerLimit(2, 1, -1) == 8);     // wraps
    CHECK(StepPlayerLimit(5, 5, -1) == 8 && StepPlayerLimit(8, 5, 1) == 5);     // 5 players in: the stepper bottoms out at 5
    CHECK(StepPlayerLimit(6, 5, -1) == 5 && StepPlayerLimit(5, 3, -1) == 4);
    CHECK(StepPlayerLimit(2, 5, 1) == 6);                                       // a value below the floor starts from the floor
    CHECK(StepPlayerLimit(99, 1, 1) == 2 && StepPlayerLimit(99, 1, -1) == 7);
}

static void TestFullAndRoom() {
    CHECK(!LobbyIsFull(3, 4) && LobbyIsFull(4, 4) && LobbyIsFull(5, 4) && !LobbyIsFull(7, 8) && LobbyIsFull(8, 8));
    CHECK(RoomForHello(3, false, 4) && !RoomForHello(4, false, 4) && !RoomForHello(8, false, 8));
    CHECK(RoomForHello(4, true, 4) && RoomForHello(8, true, 2));                // a rejoin / repeat keeps its seat
    CHECK(!RoomForHello(5, false, 2));                                          // a limit set below the roster turns newcomers away, kicks nobody
}

int main() {
    TestClamp();
    TestParse();
    TestStep();
    TestFullAndRoom();
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hsm_player_limit: all tests passed\n");
    return 0;
}

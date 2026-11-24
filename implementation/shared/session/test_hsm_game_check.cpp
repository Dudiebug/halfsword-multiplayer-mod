// Unit tests for hsm_game_check.h. Run via implementation/shared/run_tests.sh test_hsm_game_check.
#include "session/hsm_game_check.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

// "" takes any type; otherwise the actual class must be one whole alternative.
static void TestTypeOk() {
    CHECK(GameCheckTypeOk("", "DoubleProperty"));
    CHECK(GameCheckTypeOk("", ""));
    CHECK(GameCheckTypeOk("DoubleProperty", "DoubleProperty"));
    CHECK(!GameCheckTypeOk("DoubleProperty", "FloatProperty"));
    CHECK(GameCheckTypeOk("ByteProperty|EnumProperty", "ByteProperty"));
    CHECK(GameCheckTypeOk("ByteProperty|EnumProperty", "EnumProperty"));
    CHECK(!GameCheckTypeOk("ByteProperty|EnumProperty", "IntProperty"));
    CHECK(!GameCheckTypeOk("ByteProperty|EnumProperty", "Byte"));            // a prefix is not a match
    CHECK(!GameCheckTypeOk("ByteProperty|EnumProperty", "ByteProperty|"));
    CHECK(!GameCheckTypeOk("ObjectProperty", "ObjectPropertyX"));
    CHECK(!GameCheckTypeOk("ObjectProperty", ""));
    CHECK(GameCheckTypeOk("A|B|C", "C"));
    CHECK(!GameCheckTypeOk("A|B|C", "D"));
}

static void TestLooks() {
    CHECK(GameCheckLookProperty(false, "", "") == GameCheckLook::Missing);
    CHECK(GameCheckLookProperty(false, "DoubleProperty", "") == GameCheckLook::Missing);
    CHECK(GameCheckLookProperty(true, "", "FloatProperty") == GameCheckLook::Ok);
    CHECK(GameCheckLookProperty(true, "DoubleProperty", "DoubleProperty") == GameCheckLook::Ok);
    CHECK(GameCheckLookProperty(true, "DoubleProperty", "FloatProperty") == GameCheckLook::WrongType);
    // A parameter of a function that is gone is not a second miss: the function is the one.
    CHECK(GameCheckLookParam(false, false) == GameCheckLook::Skipped);
    CHECK(GameCheckLookParam(false, true) == GameCheckLook::Skipped);
    CHECK(GameCheckLookParam(true, true) == GameCheckLook::Ok);
    CHECK(GameCheckLookParam(true, false) == GameCheckLook::Missing);
}

// A miss counts only when a second look at least kGameCheckConfirmMs later agrees; a find at any look is final.
static void TestConfirm() {
    GameCheckTally t(3);
    CHECK(t.Size() == 3);
    CHECK(t.Open(0) && t.Open(2) && !t.Open(3));
    CHECK(!t.Record(0, GameCheckLook::Missing, 1000));                    // first look: a suspect only
    CHECK(t.State(0) == GameCheckState::Suspect && t.Suspects() == 1 && t.Changed() == 0);
    CHECK(!t.Record(0, GameCheckLook::Missing, 1000 + kGameCheckConfirmMs - 1));   // too soon: still a suspect
    CHECK(t.State(0) == GameCheckState::Suspect && t.Changed() == 0);
    CHECK(t.Record(0, GameCheckLook::Missing, 1000 + kGameCheckConfirmMs));        // confirmed, logged once
    CHECK(t.State(0) == GameCheckState::Missing && t.Changed() == 1 && t.Suspects() == 0 && !t.Open(0));
    CHECK(!t.Record(0, GameCheckLook::Missing, 99999));                  // final: no second log
    CHECK(!t.Record(0, GameCheckLook::Ok, 99999));
    CHECK(t.State(0) == GameCheckState::Missing && t.Changed() == 1 && t.Ok() == 0);

    // The class was still loading: the second look finds it.
    CHECK(!t.Record(1, GameCheckLook::Missing, 500));
    CHECK(!t.Record(1, GameCheckLook::Ok, 500 + kGameCheckConfirmMs));
    CHECK(t.State(1) == GameCheckState::Ok && t.Ok() == 1 && t.Changed() == 1 && t.Suspects() == 0);

    // A wrong type is confirmed the same way and counts as changed.
    CHECK(!t.Record(2, GameCheckLook::WrongType, 0));
    CHECK(t.Record(2, GameCheckLook::WrongType, kGameCheckConfirmMs + 7));
    CHECK(t.State(2) == GameCheckState::WrongType && t.Changed() == 2);
    CHECK(!t.Record(7, GameCheckLook::Missing, 0));                      // out of range: ignored
    CHECK(t.State(7) == GameCheckState::Unchecked);
}

// Ok and Skipped are final at the first look; GetTickCount wrapping between the looks still counts the time between.
static void TestFinalAndWrap() {
    GameCheckTally t(3);
    CHECK(!t.Record(0, GameCheckLook::Ok, 0));
    CHECK(t.State(0) == GameCheckState::Ok && !t.Open(0) && t.Ok() == 1);
    CHECK(!t.Record(0, GameCheckLook::Missing, kGameCheckConfirmMs * 3));
    CHECK(t.State(0) == GameCheckState::Ok);
    CHECK(!t.Record(1, GameCheckLook::Skipped, 0));
    CHECK(t.State(1) == GameCheckState::Skipped && !t.Open(1) && t.Ok() == 1 && t.Changed() == 0 && t.Suspects() == 0);
    GameCheckTally gone(1);                                              // a parameter's suspect whose function then went
    CHECK(!gone.Record(0, GameCheckLook::Missing, 0));
    CHECK(gone.Suspects() == 1);
    CHECK(!gone.Record(0, GameCheckLook::Skipped, 1));
    CHECK(gone.State(0) == GameCheckState::Skipped && gone.Suspects() == 0 && gone.Ok() == 0 && gone.Changed() == 0);
    CHECK(!t.Record(2, GameCheckLook::Missing, 0xFFFFFF00u));
    CHECK(!t.Record(2, GameCheckLook::Missing, 0x00000010u));            // 0x110 ms later
    CHECK(t.Record(2, GameCheckLook::Missing, 0xFFFFFF00u + kGameCheckConfirmMs));
    CHECK(t.Changed() == 1);
}

static void TestLogLine() {
    const GameCheckEntry prop{GameCheckKind::Property, "ModularWeaponBP_C", "Parent Actor", "", "ObjectProperty", "gameplay/adapter.cpp"};
    CHECK(GameCheckLogLine(prop, GameCheckState::Missing) == "game check: ModularWeaponBP_C.Parent Actor missing (gameplay/adapter.cpp)");
    CHECK(GameCheckLogLine(prop, GameCheckState::WrongType, "ArrayProperty") ==
          "game check: ModularWeaponBP_C.Parent Actor is ArrayProperty, not ObjectProperty (gameplay/adapter.cpp)");
    const GameCheckEntry fn{GameCheckKind::Function, "Willie_BP_C", "Dislocate", "", "", "gameplay/combat_sync.cpp"};
    CHECK(GameCheckLogLine(fn, GameCheckState::Missing) == "game check: Willie_BP_C.Dislocate (function) missing (gameplay/combat_sync.cpp)");
    const GameCheckEntry native{GameCheckKind::NativeFunction, "", "K2_GetActorLocation", "", "", "gameplay/adapter.cpp"};
    CHECK(GameCheckLogLine(native, GameCheckState::Missing) == "game check: native function K2_GetActorLocation missing (gameplay/adapter.cpp)");
    const GameCheckEntry param{GameCheckKind::Param, "Willie_BP_C", "Apply Damage", "Damage", "", "gameplay/claims.cpp"};
    CHECK(GameCheckLogLine(param, GameCheckState::Missing) == "game check: Willie_BP_C.Apply Damage has no parameter Damage (gameplay/claims.cpp)");
    const GameCheckEntry nativeParam{GameCheckKind::Param, "", "SetActorLocation", "bSweep", "", "gameplay/adapter.cpp"};
    CHECK(GameCheckLogLine(nativeParam, GameCheckState::Missing) == "game check: SetActorLocation has no parameter bSweep (gameplay/adapter.cpp)");
}

static void TestNotice() {
    CHECK(GameCheckNotice(26737211, 0).empty());
    CHECK(GameCheckNotice(0, 0).empty());
    CHECK(GameCheckNotice(26737211, -1).empty());
    CHECK(GameCheckNotice(26800000, 1) ==
          "This Half Sword build (26800000) changed 1 thing HSMMod uses; some features may not work. Look for an HSMMod update.");
    CHECK(GameCheckNotice(26800000, 31) ==
          "This Half Sword build (26800000) changed 31 things HSMMod uses; some features may not work. Look for an HSMMod update.");
    CHECK(GameCheckNotice(0, 2) == "This Half Sword build changed 2 things HSMMod uses; some features may not work. Look for an HSMMod update.");
}

int main() {
    TestTypeOk();
    TestLooks();
    TestConfirm();
    TestFinalAndWrap();
    TestLogLine();
    TestNotice();
    if (g_fail) { std::printf("test_hsm_game_check: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_game_check: ok\n");
    return 0;
}

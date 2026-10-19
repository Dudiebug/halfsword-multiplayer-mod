// Unit tests for hsm_names.h. Run via implementation/shared/run_tests.sh test_hsm_names.
#include "util/hsm_names.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using hsm::SameNameAscii;
using hsm::StartsWithNameAscii;

// The test-4 case: the parameter is "Bone" but the name table displays it as "bone".
static void TestCaseIgnored() {
    CHECK(SameNameAscii("Bone", "bone"));
    CHECK(SameNameAscii("bone", "Bone"));
    CHECK(SameNameAscii("Bone", "BONE"));
    CHECK(SameNameAscii("Last Complex Damage Impulse", "last complex damage IMPULSE"));
    CHECK(SameNameAscii("Weapon_R_1", "weapon_r_1"));
}

static void TestDifferentNames() {
    CHECK(!SameNameAscii("Bone", "Bone_1"));     // longer
    CHECK(!SameNameAscii("Bone_1", "Bone"));     // shorter
    CHECK(!SameNameAscii("Bone", "Bonf"));
    CHECK(!SameNameAscii("Bone", "Bon"));
    CHECK(!SameNameAscii("", "a"));
    CHECK(!SameNameAscii("a", ""));
    CHECK(SameNameAscii("", ""));
    CHECK(SameNameAscii("Bone", "Bone"));
}

// Only 'A'-'Z' fold: '[' and '{' (and '@'/'`', '^'/'~') differ by the same bit but are different characters, and
// bytes above 0x7F compare exactly.
static void TestOnlyAsciiLettersFold() {
    CHECK(!SameNameAscii("[", "{"));
    CHECK(!SameNameAscii("@", "`"));
    CHECK(!SameNameAscii("^", "~"));
    CHECK(!SameNameAscii("\xC4", "\xE4"));
    CHECK(SameNameAscii("\xC4", "\xC4"));
    CHECK(!SameNameAscii("a b", "a_b"));
    CHECK(SameNameAscii("Z", "z"));
    CHECK(SameNameAscii("A", "a"));
}

static void TestPrefix() {
    CHECK(StartsWithNameAscii("Bone_12_0123ABCD", "bone_"));
    CHECK(StartsWithNameAscii("bone_12", "Bone_12"));
    CHECK(StartsWithNameAscii("Bone", "Bone"));
    CHECK(StartsWithNameAscii("anything", ""));
    CHECK(!StartsWithNameAscii("Bo", "Bone"));
    CHECK(!StartsWithNameAscii("Bane_1", "Bone"));
    CHECK(!StartsWithNameAscii("", "B"));
}

int main() {
    TestCaseIgnored();
    TestDifferentNames();
    TestOnlyAsciiLettersFold();
    TestPrefix();
    if (g_fail) { std::printf("test_hsm_names: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_names: ok\n");
    return 0;
}

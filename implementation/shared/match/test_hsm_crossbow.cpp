// Unit tests for hsm_crossbow.h. Run via implementation/shared/run_tests.sh test_hsm_crossbow.
#include "match/hsm_crossbow.h"
#include <cstdio>
#include <set>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static void TestCounts() {
    CHECK(CrossbowCount(0, 5) == 0 && CrossbowCount(0, 0) == 0);
    CHECK(CrossbowCount(1, 5) == 1 && CrossbowCount(1, 0) == 0);
    CHECK(CrossbowCount(2, 5) == 2 && CrossbowCount(2, 1) == 1 && CrossbowCount(2, 0) == 0);
    CHECK(CrossbowCount(3, 4) == 2 && CrossbowCount(3, 5) == 3 && CrossbowCount(3, 1) == 1 && CrossbowCount(3, 0) == 0);
    CHECK(CrossbowCount(4, 6) == 6 && CrossbowCount(4, 0) == 0);
    CHECK(CrossbowCount(9, 6) == 0);                         // an unknown mode gives none
    CHECK(ValidCrossbows(0) && ValidCrossbows(4) && !ValidCrossbows(5) && !ValidCrossbows(255));
    CHECK(std::string(CrossbowsName(0)) == "Off" && std::string(CrossbowsName(3)) == "Half" && std::string(CrossbowsName(77)) == "Off");
}

static void TestPick() {
    const std::vector<uint8_t> all = {1, 2, 3, 4, 5, 6};
    CHECK(PickCrossbowHolders(0, all, 7).empty());           // Off: nobody
    CHECK(PickCrossbowHolders(4, all, 7) == all);            // All: everybody, in order
    CHECK(PickCrossbowHolders(1, {}, 7).empty() && PickCrossbowHolders(4, {}, 7).empty());
    CHECK(PickCrossbowHolders(2, {9}, 7) == std::vector<uint8_t>{9});            // clamped to who is there
    for (uint8_t mode = 1; mode <= 3; ++mode) {
        for (uint32_t seed = 0; seed < 200; ++seed) {
            std::vector<uint8_t> p = PickCrossbowHolders(mode, all, seed);
            CHECK(p.size() == CrossbowCount(mode, all.size()));
            CHECK(std::is_sorted(p.begin(), p.end()) && std::set<uint8_t>(p.begin(), p.end()).size() == p.size());   // distinct
            for (uint8_t e : p) CHECK(e >= 1 && e <= 6);     // only fighters of the list
            CHECK(p == PickCrossbowHolders(mode, all, seed));                    // deterministic
        }
    }
    // The order and the repeats of the input do not matter.
    CHECK(PickCrossbowHolders(2, {6, 5, 4, 3, 2, 1, 3, 3}, 11) == PickCrossbowHolders(2, all, 11));
    // The seed matters: over many seeds every fighter is picked at some point, and not always the same pair.
    std::set<uint8_t> seen; std::set<std::vector<uint8_t>> pairs;
    for (uint32_t seed = 1; seed <= 300; ++seed) { auto p = PickCrossbowHolders(2, all, seed); seen.insert(p.begin(), p.end()); pairs.insert(p); }
    CHECK(seen.size() == all.size() && pairs.size() > 5);
    std::set<uint8_t> one;
    for (uint32_t seed = 1; seed <= 300; ++seed) one.insert(PickCrossbowHolders(1, all, seed)[0]);
    CHECK(one.size() == all.size());
}

int main() {
    TestCounts();
    TestPick();
    if (g_fail) { std::printf("test_hsm_crossbow: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crossbow: ok\n");
    return 0;
}

// Unit tests for hsm_dice.h. Run via implementation/shared/run_tests.sh test_hsm_dice.
#include "session/hsm_dice.h"
#include <cstdio>
#include <limits>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::dice;

static void TestStreamIsAFunctionOfItsInputs() {
    const uint64_t k = IdentityKey("Map_Airplane/BP_Airplane_C_1");
    CHECK(Draw(5, k, 0) == Draw(5, k, 0));                 // same machine or another: same bits
    CHECK(Draw(5, k, 0) != Draw(5, k, 1));                 // the next roll differs
    CHECK(Draw(5, k, 0) != Draw(6, k, 0));                 // another level's seed differs
    CHECK(Draw(5, k, 0) != Draw(5, IdentityKey("Map_Airplane/BP_Movement_C_1"), 0));   // another object differs
    CHECK(Draw(5, k, 3, 0) != Draw(5, k, 3, 1));           // sub-draws of one roll are independent
    CHECK(IdentityKey("a") != IdentityKey("b") && IdentityKey("") != IdentityKey("a"));
    CHECK(IdentityKey("Map/Door") == IdentityKey(std::string("Map/Door")));
}

static void TestStreamIsSpreadOut() {
    std::set<uint64_t> seen;
    int heads = 0;
    double sum = 0;
    const uint64_t k = IdentityKey("x");
    for (uint64_t n = 0; n < 20000; ++n) {
        const uint64_t b = Draw(0x1234, k, n);
        seen.insert(b);
        const double u = Unit(b);
        CHECK(u >= 0.0 && u < 1.0);
        sum += u;
        heads += RandomBool(u);
    }
    CHECK(seen.size() == 20000);
    CHECK(std::fabs(sum / 20000 - 0.5) < 0.01);
    CHECK(heads > 9700 && heads < 10300);
    CHECK(Unit(0) == 0.0 && Unit(~0ull) < 1.0 && Unit(~0ull) > 0.999999);
}

static void TestIntegerRanges() {
    CHECK(RandomInteger(0.0, 5) == 0 && RandomInteger(0.999999, 5) == 4 && RandomInteger(0.5, 5) == 2);
    CHECK(RandomInteger(0.7, 0) == 0 && RandomInteger(0.7, -3) == 0);            // Max <= 0 gives 0
    CHECK(RandomInteger(0.9999999999999999, std::numeric_limits<int32_t>::max()) == std::numeric_limits<int32_t>::max() - 1);
    // Inclusive at both ends.
    CHECK(RandomIntegerInRange(0.0, 3, 7) == 3 && RandomIntegerInRange(0.9999999, 3, 7) == 7);
    int lo = 99, hi = -99;
    for (uint64_t n = 0; n < 5000; ++n) {
        const int v = RandomIntegerInRange(Unit(Draw(1, 2, n)), -2, 2);
        CHECK(v >= -2 && v <= 2);
        lo = v < lo ? v : lo; hi = v > hi ? v : hi;
    }
    CHECK(lo == -2 && hi == 2);                                                   // both ends are reachable
    CHECK(RandomIntegerInRange(0.4, 5, 5) == 5);                                  // a single value
    CHECK(RandomIntegerInRange(0.4, 9, 2) == 9);                                  // Max < Min: Min
    const int32_t big = std::numeric_limits<int32_t>::max(), small = std::numeric_limits<int32_t>::min();
    const int32_t wide = RandomIntegerInRange(0.5, small, big);                   // the range does not fit in 32 bits
    CHECK(wide >= -2 && wide <= 1);
    CHECK(RandomIntegerInRange(0.9999999999999999, small, big) == big);
}

static void TestFloatsAndBools() {
    CHECK(RandomFloatInRange(0.0, 2.0, 6.0) == 2.0 && RandomFloatInRange(0.5, 2.0, 6.0) == 4.0);
    CHECK(RandomFloatInRange(0.9999999999999999, 2.0, 6.0) < 6.0);                // [Min, Max)
    CHECK(RandomFloatInRange(0.9999999999999999, 0.0, 1e300) < 1e300);
    CHECK(RandomFloatInRange(0.3, 5.0, 5.0) == 5.0);
    CHECK(RandomFloatInRange(0.5, -4.0, -2.0) == -3.0);
    CHECK(RandomFloatInRange(0.5, 1.0, std::numeric_limits<double>::infinity()) == 1.0);   // non-finite: Min
    CHECK(!RandomBool(0.4999) && RandomBool(0.5) && RandomBool(0.99));
    CHECK(!RandomBoolWithWeight(0.0, 0.0f) && !RandomBoolWithWeight(0.0, -1.0f));  // Weight <= 0: never
    CHECK(RandomBoolWithWeight(0.9999999, 1.0f) && RandomBoolWithWeight(0.0, 1.0f)); // Weight 1: always
    CHECK(RandomBoolWithWeight(0.2, 0.25f) && !RandomBoolWithWeight(0.3, 0.25f));
    int hits = 0;
    for (uint64_t n = 0; n < 20000; ++n) hits += RandomBoolWithWeight(Unit(Draw(9, 9, n)), 0.25f);
    CHECK(hits > 4700 && hits < 5300);
}

static void TestVectors() {
    for (uint64_t n = 0; n < 2000; ++n) {
        const Vec3 v = RandomUnitVector(Unit(Draw(3, 4, n, 0)), Unit(Draw(3, 4, n, 1)));
        CHECK(std::fabs(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) - 1.0) < 1e-9);
    }
    CHECK(RandomUnitVector(1.0, 0.0).z == 1.0 && RandomUnitVector(0.0, 0.0).z == -1.0);
    const Vec3 r = RandomRotator(0.25, 0.5, 0.75, false);
    CHECK(r.x == 90.0 && r.y == 180.0 && r.z == 0.0);
    CHECK(RandomRotator(0.25, 0.5, 0.75, true).z == 270.0);
    const Vec3 p = RandomPointInBox(0.5, 0.0, 1.0, {10, 20, 30}, {1, 2, 3});
    CHECK(p.x == 10.0 && p.y == 18.0 && p.z == 33.0);
}

// Two machines roll the same objects in a different order and still agree: a roll depends on the object's own count.
static void TestInterleavingDoesNotMatter() {
    const uint64_t door = IdentityKey("W/Door"), wind = IdentityKey("W/Wind");
    uint64_t aDoor = 0, aWind = 0, bDoor = 0, bWind = 0;
    std::vector<int32_t> a, b;
    a.push_back(RandomIntegerInRange(Unit(Draw(77, door, aDoor++)), 0, 100));
    a.push_back(RandomIntegerInRange(Unit(Draw(77, wind, aWind++)), 0, 100));
    a.push_back(RandomIntegerInRange(Unit(Draw(77, door, aDoor++)), 0, 100));
    b.push_back(RandomIntegerInRange(Unit(Draw(77, wind, bWind++)), 0, 100));
    b.push_back(RandomIntegerInRange(Unit(Draw(77, door, bDoor++)), 0, 100));
    b.push_back(RandomIntegerInRange(Unit(Draw(77, door, bDoor++)), 0, 100));
    CHECK(a[0] == b[1] && a[1] == b[0] && a[2] == b[2]);
}

static void TestLevelSeedWire() {
    LevelSeedMsg m; m.travelSeq = 9; m.seed = 0xFEDCBA9876543210ull;
    std::vector<uint8_t> b = Encode(m);
    CHECK(b.size() == 4 + 4 + 8);
    LevelSeedMsg back;
    CHECK(Decode(b.data(), b.size(), back) && back.travelSeq == 9 && back.seed == m.seed);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, back));          // every truncation fails
    std::vector<uint8_t> longer = b; longer.push_back(0);
    CHECK(!Decode(longer.data(), longer.size(), back));                                      // trailing bytes fail
    std::vector<uint8_t> wrong = b; wrong[3] = uint8_t(MsgType::Travel);
    CHECK(!Decode(wrong.data(), wrong.size(), back));                                        // another message type
    wrong = b; wrong[2] = uint8_t(kProtocolRevision + 1);
    CHECK(!Decode(wrong.data(), wrong.size(), back));                                        // another revision
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::LevelSeed);
    m.travelSeq = 0; m.seed = 0; b = Encode(m);
    CHECK(Decode(b.data(), b.size(), back) && back.travelSeq == 0 && back.seed == 0);        // zero is a valid seed
}

int main() {
    TestStreamIsAFunctionOfItsInputs();
    TestStreamIsSpreadOut();
    TestIntegerRanges();
    TestFloatsAndBools();
    TestVectors();
    TestInterleavingDoesNotMatter();
    TestLevelSeedWire();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_dice: ok\n");
    return 0;
}

// A drop given up on (its stand-in's hand held no copy of it when the owner's pose settled: "heard with that hand empty",
// 3-7 a session in the pre7 logs) is bound later, when the owner's stream says where the weapon is: the nearest loose
// weapon here of the class the owner's hand showed, within reach (sync/hsm_late_drop.h). It never takes a weapon of
// another class, one out of reach, or one that is a prop here under another id (that one is reported as aliased).
#include "sync/hsm_late_drop.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static const uint32_t kSword = 0x1111, kAxe = 0x2222;

static LateCandidate Cand(double x, double y, double z, uint32_t cls, bool prop = false) {
    LateCandidate c;
    c.pos = V3{x, y, z};
    c.classHash = cls;
    c.propElsewhere = prop;
    return c;
}

// The log's case: nothing in the stand-in's hand, the weapon lies here near where the owner's stream puts it.
static void TestBindsNearestOfClass() {
    const std::vector<LateCandidate> c = {Cand(900, 0, 0, kSword), Cand(40, 10, 0, kSword), Cand(20, 0, 0, kAxe), Cand(120, 0, 0, kSword)};
    const LatePick p = PickLateMatch(c, kSword, V3{0, 0, 0});
    CHECK(p.index == 1 && !p.aliased);
    CHECK(std::fabs(p.distUu - std::sqrt(40.0 * 40.0 + 10.0 * 10.0)) < 1e-9);
}

static void TestClassMustMatchWhenKnown() {
    const std::vector<LateCandidate> c = {Cand(10, 0, 0, kAxe)};
    CHECK(PickLateMatch(c, kSword, V3{0, 0, 0}).index == -1);
    CHECK(PickLateMatch(c, kAxe, V3{0, 0, 0}).index == 0);
}

static void TestReach() {
    const std::vector<LateCandidate> c = {Cand(kLateMatchRadiusUu + 1, 0, 0, kSword)};
    CHECK(PickLateMatch(c, kSword, V3{0, 0, 0}).index == -1);
    const std::vector<LateCandidate> in = {Cand(kLateMatchRadiusUu, 0, 0, kSword)};
    CHECK(PickLateMatch(in, kSword, V3{0, 0, 0}).index == 0);
    // Height counts: a weapon one floor up is not the drop's.
    const std::vector<LateCandidate> up = {Cand(0, 0, 400, kSword)};
    CHECK(PickLateMatch(up, kSword, V3{0, 0, 0}).index == -1);
}

// The class is not known (the hand was empty already when the drop was heard): any weapon, but only right at the stream.
static void TestUnknownClassIsStricter() {
    const std::vector<LateCandidate> c = {Cand(kLateMatchNoClassRadiusUu - 1, 0, 0, kAxe), Cand(kLateMatchNoClassRadiusUu + 20, 0, 0, kSword)};
    CHECK(PickLateMatch(c, kHandEmpty, V3{0, 0, 0}).index == 0);
    CHECK(PickLateMatch(c, kHandUnknown, V3{0, 0, 0}).index == 0);
    const std::vector<LateCandidate> far = {Cand(kLateMatchNoClassRadiusUu + 20, 0, 0, kSword)};
    CHECK(PickLateMatch(far, kHandEmpty, V3{0, 0, 0}).index == -1);
}

// A weapon that is a prop here under another id is not bound; when it is the nearest it hides a farther loose one (the
// stream is of that weapon), and when it is farther it does not matter.
static void TestPropElsewhereIsNotTaken() {
    std::vector<LateCandidate> c = {Cand(30, 0, 0, kSword, true), Cand(80, 0, 0, kSword)};
    LatePick p = PickLateMatch(c, kSword, V3{0, 0, 0});
    CHECK(p.index == 0 && p.aliased);
    c = {Cand(80, 0, 0, kSword, true), Cand(30, 0, 0, kSword)};
    p = PickLateMatch(c, kSword, V3{0, 0, 0});
    CHECK(p.index == 1 && !p.aliased);
}

static void TestTiesAndEmpty() {
    CHECK(PickLateMatch({}, kSword, V3{0, 0, 0}).index == -1);
    const std::vector<LateCandidate> c = {Cand(50, 0, 0, kSword), Cand(0, 50, 0, kSword)};
    CHECK(PickLateMatch(c, kSword, V3{0, 0, 0}).index == 0);   // the first of equals
}

static void TestNonFiniteIsNeverMatched() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<LateCandidate> c = {Cand(nan, 0, 0, kSword), Cand(10, 0, 0, kSword)};
    CHECK(PickLateMatch(c, kSword, V3{0, 0, 0}).index == 1);
    CHECK(PickLateMatch(c, kSword, V3{nan, 0, 0}).index == -1);
}

static void TestExpiry() {
    CHECK(!LateDropExpired(1000, 1000 + kLateDropKeepMs - 1));
    CHECK(LateDropExpired(1000, 1000 + kLateDropKeepMs));
}

int main() {
    TestBindsNearestOfClass();
    TestClassMustMatchWhenKnown();
    TestReach();
    TestUnknownClassIsStricter();
    TestPropElsewhereIsNotTaken();
    TestTiesAndEmpty();
    TestNonFiniteIsNeverMatched();
    TestExpiry();
    if (g_fail == 0) std::printf("hsm_late_drop: all tests passed\n");
    return g_fail != 0;
}

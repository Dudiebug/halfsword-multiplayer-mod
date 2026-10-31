// Constraint-built level props (session/hsm_structure.h). soft (default): streamed by the host as one snapshot at 20 Hz while any
// body moves, steered on receivers, never leased or reset. local: never streamed, driven, claimed or reset. full: as pre17.
// A body that detached streams; a plain prop, a one-body prop, a prop with no constraints and a weapon are unchanged.
// sync_structures parses "soft", "local", "full"; the old "0" is soft and "1" is full.
#include "session/hsm_structure.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static const uint64_t kBridge = MakeLevelPropId(12);
static const StructureMode kSoft = StructureMode::Soft, kLocal = StructureMode::Local, kFull = StructureMode::Full;

static void TestStructureShape() {
    // The Pit's bridge: several planks joined by constraints. Whether a joint has broken is not part of the rule: the
    // bodies still joined are treated alike either way.
    CHECK(IsStructure(kBridge, false, 8, 7));
    CHECK(IsStructure(kBridge, false, 2, 1));                      // the smallest: two bodies, one joint
    CHECK(IsStructure(MakeLevelPropId(0), false, 30, 29));         // a chain
}

static void TestModesPickTheHandling() {
    CHECK(StructureSoft(kSoft, kBridge, false, 8, 7));
    CHECK(!StructureLocal(kSoft, kBridge, false, 8, 7));
    CHECK(StructureOwnCopy(kSoft, kBridge, false, 8, 7));          // no lease, no claimed blow, no reset move
    CHECK(StructureLocal(kLocal, kBridge, false, 8, 7));
    CHECK(!StructureSoft(kLocal, kBridge, false, 8, 7));
    CHECK(StructureOwnCopy(kLocal, kBridge, false, 8, 7));
    CHECK(!StructureSoft(kFull, kBridge, false, 8, 7));            // full: as pre17, streamed, teleported, leased and reset
    CHECK(!StructureLocal(kFull, kBridge, false, 8, 7));
    CHECK(!StructureOwnCopy(kFull, kBridge, false, 8, 7));
}

static void TestDetachedBodyStreams() {
    // A body that left the structure is its own object: one body, no constraints, a sub of the same actor.
    for (StructureMode m : {kSoft, kLocal, kFull}) {
        CHECK(!StructureOwnCopy(m, MakeSpawnedId(0, 5), true, 1, 0));
        CHECK(!StructureOwnCopy(m, kBridge, true, 1, 0));          // even under a level id, a detached body is not one
        CHECK(!StructureOwnCopy(m, kBridge, true, 8, 7));
    }
}

static void TestOtherPropsUnchanged() {
    for (StructureMode m : {kSoft, kLocal, kFull}) {
        CHECK(!StructureOwnCopy(m, kBridge, false, 1, 0));         // a barrel: one body
        CHECK(!StructureOwnCopy(m, kBridge, false, 1, 3));         // one body is never a structure
        CHECK(!StructureOwnCopy(m, kBridge, false, 4, 0));         // several bodies, nothing joining them
        CHECK(!StructureOwnCopy(m, kBridge, false, 0, 0));
    }
}

static void TestWeaponsAndSpawnedNeverStructures() {
    for (StructureMode m : {kSoft, kLocal, kFull}) {
        CHECK(!StructureOwnCopy(m, MakeWeaponPropId(1, 0), false, 3, 2));
        CHECK(!StructureOwnCopy(m, MakeDropPropId(2, 1, 3), false, 3, 2));
        CHECK(!StructureOwnCopy(m, MakeSpawnedId(1, 9), false, 3, 2));
    }
    CHECK(!IsStructure(MakeWeaponPropId(1, 0), false, 3, 2));
}

static void TestIniValues() {
    CHECK(ParseStructureMode("soft") == kSoft);
    CHECK(ParseStructureMode("local") == kLocal);
    CHECK(ParseStructureMode("full") == kFull);
    CHECK(ParseStructureMode(" Full ") == kFull);                  // case and blanks do not matter
    CHECK(ParseStructureMode("LOCAL") == kLocal);
    CHECK(ParseStructureMode("0") == kSoft);                       // an old INI: 0 meant "not pre17"
    CHECK(ParseStructureMode("1") == kFull);                       // 1 meant "pre17"
    CHECK(ParseStructureMode("") == kSoft);                        // no line: the default
    CHECK(ParseStructureMode("bogus") == kSoft);
    CHECK(std::string(StructureModeName(kSoft)) == "soft" && std::string(StructureModeName(kLocal)) == "local" &&
          std::string(StructureModeName(kFull)) == "full");
}

static void TestStreamRate() {
    // Moving: every 50 ms (20 Hz), not the loose prop's 16 ms; at rest once a second.
    CHECK(!StructureStreamDue(1000.0, false, 1000.0 + kStructureStreamMs - 1));
    CHECK(StructureStreamDue(1000.0, false, 1000.0 + kStructureStreamMs));
    CHECK(!StructureStreamDue(1000.0, true, 1000.0 + kFreeIdleMs - 1));
    CHECK(StructureStreamDue(1000.0, true, 1000.0 + kFreeIdleMs));
    CHECK(StructureStreamDue(-1e18, false, 0.0));                  // never sent: due at once
    CHECK(kStructureStreamMs > kFreeStreamMs);                     // slower than a loose moving prop (60 Hz)
}

int main() {
    TestStructureShape();
    TestModesPickTheHandling();
    TestDetachedBodyStreams();
    TestOtherPropsUnchanged();
    TestWeaponsAndSpawnedNeverStructures();
    TestIniValues();
    TestStreamRate();
    if (g_fail) { std::printf("test_hsm_structure: %d check(s) failed\n", g_fail); return 1; }
    std::printf("test_hsm_structure: all checks passed\n");
    return 0;
}

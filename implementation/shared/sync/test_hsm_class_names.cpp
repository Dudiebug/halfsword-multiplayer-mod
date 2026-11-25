// Class names for log lines. Run via implementation/shared/run_tests.sh test_hsm_class_names.
#include "sync/hsm_class_names.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

int main() {
    CHECK(ShortClassName("/Game/Props/BP_Barrel.BP_Barrel_C") == "BP_Barrel_C");
    CHECK(ShortClassName("BP_Crate_C") == "BP_Crate_C");
    CHECK(ShortClassName("Class /Script/Engine.Actor") == "Actor");

    ClassNames n;
    const uint32_t barrel = WeaponClassHash("BP_Barrel_C");
    CHECK(n.Describe(barrel).rfind("unknown class (", 0) == 0);      // never seen here
    CHECK(!n.Known(barrel));
    CHECK(n.Note("BP_Barrel_C") == barrel);
    CHECK(n.Known(barrel));
    char want[64];
    std::snprintf(want, sizeof want, "BP_Barrel_C (%08x)", unsigned(barrel));
    CHECK(n.Describe(barrel) == want);
    CHECK(n.Note("") == 0);
    CHECK(n.Describe(0).rfind("unknown class (00000000)", 0) == 0);
    // Case folds as the hash does: another spelling of the same class finds the same entry.
    CHECK(n.Describe(WeaponClassHash("bp_barrel_c")) == want);
    const uint32_t pathed = n.Note("/Game/Props/BP_Crate.BP_Crate_C");   // a path is shortened for the line
    CHECK(n.Describe(pathed).rfind("BP_Crate_C (", 0) == 0);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_class_names: ok\n");
    return 0;
}

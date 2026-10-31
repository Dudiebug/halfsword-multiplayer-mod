// BlobAssetNames: the class names found in a passport blob, for the weapon-filter log lines.
#include "match/hsm_gear_names.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

int main() {
    const std::string nul(1, '\0');
    CHECK(BlobAssetNames({}).empty());
    std::string s = std::string("\x01\x02xx", 4) + "BP_Sword_Long_C" + nul + "junk" + std::string(1, '\x07') + "/Game/Weapons/BP_Axe.BP_Axe_C" + nul + "BP_Sword_Long_C";
    std::vector<std::string> n = BlobAssetNames(Bytes(s));
    CHECK(n.size() == 2 && n[0] == "BP_Sword_Long_C" && n[1] == "/Game/Weapons/BP_Axe.BP_Axe_C");   // distinct, first-seen order
    CHECK(BlobAssetNames(Bytes("BP_A" + nul + "xxxxxxxx")).empty());                                  // too short / not a class name
    CHECK(BlobAssetNames(Bytes("BP_Mace_Flanged_C" + std::string(1, '\x01') + "BP_Axe_C" + std::string(1, '\x01') + "BP_Pike_C"), 2).size() == 2);   // capped
    CHECK(BlobAssetNames(Bytes(std::string("BP_") + std::string(200, 'z')), 6, 6, 20)[0].size() == 20);   // cut
    CHECK(BlobAssetNames(Bytes("prefix_BP_Spear_C")).at(0) == "BP_Spear_C");                          // starts at the name
    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("test_hsm_gear_names: all passed\n");
    return 0;
}

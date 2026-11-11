// Unit tests for hsm_region.h: the labels and keys, reading a key back, the picker stepping, the region in use, and the guess
// from the Steam relay data centres (which must say "unknown" whenever it is not clear). Run via
// implementation/shared/run_tests.sh test_hsm_region.
#include "session/hsm_region.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestLabelsAndKeys() {
    CHECK(kRegionCount == 7);
    CHECK(std::string(RegionLabel(Region::Auto)) == "Auto" && std::string(RegionLabel(Region::NaEast)) == "NA East");
    CHECK(std::string(RegionLabel(Region::NaWest)) == "NA West" && std::string(RegionLabel(Region::SouthAmerica)) == "South America");
    CHECK(std::string(RegionLabel(Region::Europe)) == "Europe" && std::string(RegionLabel(Region::Asia)) == "Asia");
    CHECK(std::string(RegionLabel(Region::Oceania)) == "Oceania");
    CHECK(std::string(RegionKey(Region::Auto)) == "auto" && std::string(RegionKey(Region::NaEast)) == "na-east");
    CHECK(std::string(RegionKey(Region::NaWest)) == "na-west" && std::string(RegionKey(Region::SouthAmerica)) == "south-america");
    CHECK(std::string(RegionKey(Region::Europe)) == "europe" && std::string(RegionKey(Region::Asia)) == "asia");
    CHECK(std::string(RegionKey(Region::Oceania)) == "oceania");
    CHECK(!RegionKnown(Region::Auto) && RegionKnown(Region::NaEast) && RegionKnown(Region::Oceania));
    // Every key reads back to its own region.
    for (size_t i = 0; i < kRegionCount; ++i) {
        Region r = Region::Oceania;
        CHECK(ParseRegion(RegionKey(Region(i)), r) && r == Region(i));
    }
}

static void TestParse() {
    Region r = Region::Asia;
    CHECK(ParseRegion("EUROPE", r) && r == Region::Europe);
    CHECK(ParseRegion("Na-West", r) && r == Region::NaWest);
    r = Region::Asia;
    CHECK(!ParseRegion("", r) && r == Region::Asia);                  // nothing to read: the old value stands
    CHECK(!ParseRegion("mars", r) && r == Region::Asia);
    CHECK(!ParseRegion("europe ", r) && r == Region::Asia);           // no trimming here: the caller does that
    CHECK(ParseRegion("auto", r) && r == Region::Auto);
    CHECK(LobbyRegion("asia") == Region::Asia && LobbyRegion("") == Region::Auto && LobbyRegion("atlantis") == Region::Auto);
}

static void TestPicker() {
    // Auto, then the six regions in the order the page lists them, then Auto again.
    Region r = Region::Auto;
    const Region order[] = {Region::NaEast, Region::NaWest, Region::SouthAmerica, Region::Europe, Region::Asia, Region::Oceania, Region::Auto};
    for (Region want : order) { r = NextRegion(r); CHECK(r == want); }
    CHECK(EffectiveRegion(Region::Auto, Region::Europe) == Region::Europe);       // Auto: the guess
    CHECK(EffectiveRegion(Region::Auto, Region::Auto) == Region::Auto);
    CHECK(EffectiveRegion(Region::Asia, Region::Europe) == Region::Asia);         // a pick wins over the guess
    CHECK(EffectiveRegion(Region::NaWest, Region::Auto) == Region::NaWest);
    CHECK(RegionPickLabel(Region::Auto, Region::Europe) == "Auto (Europe)");
    CHECK(RegionPickLabel(Region::Auto, Region::Auto) == "Auto (unknown)");
    CHECK(RegionPickLabel(Region::Oceania, Region::Europe) == "Oceania");
    CHECK(RegionPickLabel(Region::NaEast, Region::Auto) == "NA East");
}

static void TestPopRegions() {
    CHECK(RegionOfPop("iad") == Region::NaEast && RegionOfPop("atl") == Region::NaEast && RegionOfPop("ord") == Region::NaEast);
    CHECK(RegionOfPop("lax") == Region::NaWest && RegionOfPop("sea") == Region::NaWest && RegionOfPop("sjc") == Region::NaWest);
    CHECK(RegionOfPop("gru") == Region::SouthAmerica && RegionOfPop("scl") == Region::SouthAmerica);
    CHECK(RegionOfPop("fra") == Region::Europe && RegionOfPop("ams") == Region::Europe && RegionOfPop("lhr") == Region::Europe);
    CHECK(RegionOfPop("sgp") == Region::Asia && RegionOfPop("tyo") == Region::Asia && RegionOfPop("hkg") == Region::Asia);
    CHECK(RegionOfPop("syd") == Region::Oceania && RegionOfPop("akl") == Region::Oceania);
    CHECK(RegionOfPop("FRA") == Region::Europe);                    // any case
    CHECK(RegionOfPop("sto2") == Region::Europe && RegionOfPop("sgp1") == Region::Asia);   // a number after the code
    CHECK(RegionOfPop("jnb") == Region::Auto);                      // Africa: no region
    CHECK(RegionOfPop("zzz") == Region::Auto && RegionOfPop("") == Region::Auto && RegionOfPop("fr") == Region::Auto);
}

static std::vector<PopPing> Pops(std::initializer_list<std::pair<const char*, int>> l) {
    std::vector<PopPing> v;
    for (const auto& p : l) v.push_back({p.first, p.second});
    return v;
}

static void TestGuess() {
    // A clear case: the nearest is in Europe and the next is far behind.
    RegionGuess g = GuessRegion(Pops({{"fra", 18}, {"ams", 24}, {"iad", 95}, {"sgp", 180}}));
    CHECK(g.region == Region::Europe && g.pop == "fra" && g.pingMs == 18);
    // Nearest of another region far enough behind.
    g = GuessRegion(Pops({{"iad", 12}, {"fra", 90}, {"lax", 60}}));
    CHECK(g.region == Region::NaEast && g.pop == "iad" && g.pingMs == 12);
    // The order of the list does not matter.
    g = GuessRegion(Pops({{"sgp", 180}, {"ams", 24}, {"fra", 18}}));
    CHECK(g.region == Region::Europe && g.pop == "fra");
    // Two data centres of the same region close together: still clear.
    g = GuessRegion(Pops({{"fra", 20}, {"ams", 21}, {"lhr", 23}}));
    CHECK(g.region == Region::Europe && g.pop == "fra");
    // No data at all, or only unmeasured ones (-1): unknown.
    CHECK(GuessRegion({}).region == Region::Auto);
    CHECK(GuessRegion(Pops({{"fra", -1}, {"iad", -1}})).region == Region::Auto);
    CHECK(GuessRegion(Pops({{"fra", -1}, {"ams", 30}})).pop == "ams");               // an unmeasured one is skipped, not the best
    // The nearest is too far to say where the player is (90 ms is still fine).
    CHECK(GuessRegion(Pops({{"fra", 91}, {"iad", 200}})).region == Region::Auto);
    CHECK(GuessRegion(Pops({{"fra", 90}, {"iad", 200}})).region == Region::Europe);
    // The nearest is a data centre the table does not know: unknown, even with a known one behind it.
    g = GuessRegion(Pops({{"jnb", 15}, {"fra", 140}}));
    CHECK(g.region == Region::Auto && g.pop.empty() && g.pingMs == -1);
    // A data centre of another region within the margin behind the nearest: too close to call (7 ms is in, 8 ms is out).
    CHECK(GuessRegion(Pops({{"fra", 40}, {"iad", 47}})).region == Region::Auto);
    CHECK(GuessRegion(Pops({{"fra", 40}, {"iad", 48}})).region == Region::Europe);
    // The same, with an unknown data centre close behind: it is not in the nearest's region either.
    CHECK(GuessRegion(Pops({{"fra", 40}, {"jnb", 44}})).region == Region::Auto);
    // An unmeasured one in the margin does not count.
    CHECK(GuessRegion(Pops({{"fra", 40}, {"iad", -1}})).region == Region::Europe);
    // A tie between regions is too close to call.
    CHECK(GuessRegion(Pops({{"fra", 40}, {"iad", 40}})).region == Region::Auto);
}

static void TestDistance() {
    // The numbers Steam's ELobbyDistanceFilter uses.
    CHECK(int(LobbyDistance::Close) == 0 && int(LobbyDistance::Default) == 1);
    CHECK(int(LobbyDistance::Far) == 2 && int(LobbyDistance::Worldwide) == 3);
}

int main() {
    TestLabelsAndKeys();
    TestParse();
    TestPicker();
    TestPopRegions();
    TestGuess();
    TestDistance();
    if (g_fail) { std::printf("%d region checks failed\n", g_fail); return 1; }
    std::printf("all region tests passed\n");
    return 0;
}

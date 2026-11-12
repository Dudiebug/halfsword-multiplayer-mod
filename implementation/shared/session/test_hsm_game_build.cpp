// Unit tests for hsm_game_build.h. Run via implementation/shared/run_tests.sh test_hsm_game_build.
#include "session/hsm_game_build.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

// 26568321 has the same exe as the target, so only the Steam build id tells it apart: it is older.
static void TestCheck() {
    CHECK(kTargetSteamBuild == 26615445);
    CHECK(CheckSteamBuild(26615445) == SteamBuildCheck::Target);
    CHECK(CheckSteamBuild(26568321) == SteamBuildCheck::Older);
    CHECK(CheckSteamBuild(26615444) == SteamBuildCheck::Older);
    CHECK(CheckSteamBuild(26615446) == SteamBuildCheck::Newer);
    CHECK(CheckSteamBuild(0) == SteamBuildCheck::Unknown);
    CHECK(CheckSteamBuild(-1) == SteamBuildCheck::Unknown);
}

// No line on the target build or when Steam cannot say; the older line says to update the game, the newer one the mod.
static void TestNotice() {
    CHECK(SteamBuildNotice(26615445).empty());
    CHECK(SteamBuildNotice(0).empty());
    CHECK(SteamBuildNotice(26568321) ==
          "Half Sword is not up to date (Steam build 26568321). Update it in Steam: this HSMMod is made for build 26615445.");
    CHECK(SteamBuildNotice(27000000) ==
          "Half Sword has an update (Steam build 27000000) newer than this HSMMod (made for build 26615445). Look for an HSMMod update.");
}

// Peer-written: digits only, at most 9 of them.
static void TestParse() {
    CHECK(ParseSteamBuild("26615445") == 26615445);
    CHECK(ParseSteamBuild("1") == 1);
    CHECK(ParseSteamBuild("999999999") == 999999999);
    CHECK(ParseSteamBuild("") == 0);
    CHECK(ParseSteamBuild("1234567890") == 0);
    CHECK(ParseSteamBuild("26615445 ") == 0);
    CHECK(ParseSteamBuild("-5") == 0);
    CHECK(ParseSteamBuild("2661544x") == 0);
}

// Joinable on one build or when either side cannot say; otherwise the reason names who has to update.
static void TestLobbyReason() {
    CHECK(LobbySteamBuildReason(26615445, 26615445).empty());
    CHECK(LobbySteamBuildReason(0, 26568321).empty());
    CHECK(LobbySteamBuildReason(26615445, 0).empty());
    CHECK(LobbySteamBuildReason(26568321, 26615445) == "update Half Sword in Steam (the host is on build 26615445)");
    CHECK(LobbySteamBuildReason(26615445, 26568321) == "the host's Half Sword is not up to date (build 26568321)");
}

int main() {
    TestCheck();
    TestNotice();
    TestParse();
    TestLobbyReason();
    if (g_fail) { std::printf("test_hsm_game_build: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_game_build: ok\n");
    return 0;
}

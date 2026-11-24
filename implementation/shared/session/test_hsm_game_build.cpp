// Unit tests for hsm_game_build.h. Run via implementation/shared/run_tests.sh test_hsm_game_build.
#include "session/hsm_game_build.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

// The one line tools/game_watch.py bumps with kVerifiedSteamBuild; every other case is relative to it.
static constexpr int kBuild = 26820056;
static const std::string kB = std::to_string(kBuild);

// 26568321, 26615445 and 26641406 have the same exe as later pak-only builds, so only the Steam build id tells them apart.
static void TestCheck() {
    CHECK(kVerifiedSteamBuild == kBuild);
    CHECK(CheckSteamBuild(kBuild) == SteamBuildCheck::Verified);
    CHECK(CheckSteamBuild(26568321) == SteamBuildCheck::Older);
    CHECK(CheckSteamBuild(26615445) == SteamBuildCheck::Older);
    CHECK(CheckSteamBuild(26641406) == SteamBuildCheck::Older);
    CHECK(CheckSteamBuild(kBuild - 1) == SteamBuildCheck::Older);
    CHECK(CheckSteamBuild(kBuild + 1) == SteamBuildCheck::Newer);
    CHECK(CheckSteamBuild(0) == SteamBuildCheck::Unknown);
    CHECK(CheckSteamBuild(-1) == SteamBuildCheck::Unknown);
}

// Only an older build has a line (update the game). A newer one has none: the game check says what it changed.
static void TestNotice() {
    CHECK(SteamBuildNotice(kBuild).empty());
    CHECK(SteamBuildNotice(0).empty());
    CHECK(SteamBuildNotice(-3).empty());
    CHECK(SteamBuildNotice(26568321) ==
          "Half Sword is not up to date (Steam build 26568321). Update it in Steam: this HSMMod was checked on build " + kB + ".");
    CHECK(SteamBuildNotice(kBuild - 1) ==
          "Half Sword is not up to date (Steam build " + std::to_string(kBuild - 1) + "). Update it in Steam: this HSMMod was "
          "checked on build " + kB + ".");
    CHECK(SteamBuildNotice(kBuild + 1).empty());
    CHECK(SteamBuildNotice(999999999).empty());
}

// Peer-written: digits only, at most 9 of them.
static void TestParse() {
    CHECK(ParseSteamBuild("26737211") == 26737211);
    CHECK(ParseSteamBuild("1") == 1);
    CHECK(ParseSteamBuild("999999999") == 999999999);
    CHECK(ParseSteamBuild("") == 0);
    CHECK(ParseSteamBuild("1234567890") == 0);
    CHECK(ParseSteamBuild("26737211 ") == 0);
    CHECK(ParseSteamBuild("-5") == 0);
    CHECK(ParseSteamBuild("2661544x") == 0);
}

// Joinable on one build or when either side cannot say; otherwise the reason names who has to update.
static void TestLobbyReason() {
    CHECK(LobbySteamBuildReason(kBuild, kBuild).empty());
    CHECK(LobbySteamBuildReason(0, 26568321).empty());
    CHECK(LobbySteamBuildReason(kBuild, 0).empty());
    CHECK(LobbySteamBuildReason(26568321, kBuild) == "update Half Sword in Steam (the host is on build " + kB + ")");
    CHECK(LobbySteamBuildReason(kBuild, 26568321) == "the host's Half Sword is not up to date (build 26568321)");
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

// Who has to update when a host refuses another HSMMod version (#3073).
#include "session/hsm_version_refusal.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestTestBuilds() {
    auto v = ParseModVersion("0.8.0-pre49-test");
    CHECK(v && v->pre == 49 && v->minor == 8);
    v = ParseModVersion("0.8.0-test");
    CHECK(v && v->pre == 0);
    CHECK(!ParseModVersion("-test"));
    CHECK(!ParseModVersion("0.8.0-pre49-test-test"));
    CHECK(WhoIsBehind("0.8.0-pre49", "0.8.0-pre49-test") == Behind::Unknown);   // the same version
    CHECK(WhoIsBehind("0.8.0-pre48", "0.8.0-pre49-test") == Behind::Host);
}

// The report: the host was on pre46, the player on pre48, and the line did not say who must update.
static void TestTheReport() {
    CHECK(WhoIsBehind("0.8.0-pre46", "0.8.0-pre48") == Behind::Host);
    CHECK(VersionRefusalText("0.8.0-pre46", "0.8.0-pre48") ==
          "The host is on 0.8.0-pre46 and you are on 0.8.0-pre48. Ask the host to update HSMMod.");
    CHECK(VersionLobbyReason("0.8.0-pre46", "0.8.0-pre48") == "host on older 0.8.0-pre46 (you 0.8.0-pre48)");
}

static void TestPlayerBehind() {
    CHECK(WhoIsBehind("0.8.0-pre50", "0.8.0-pre48") == Behind::Me);
    CHECK(WhoIsBehind("0.8.0", "0.8.0-pre48") == Behind::Me);        // the release is newer than every pre of it
    CHECK(WhoIsBehind("0.8.0-pre9", "0.8.0-pre10") == Behind::Host);  // numbers, not text
    CHECK(VersionRefusalText("0.8.0-pre50", "0.8.0-pre48") ==
          "The host is on 0.8.0-pre50 and you are on 0.8.0-pre48. Update HSMMod to join.");
    CHECK(VersionLobbyReason("0.8.0-pre50", "0.8.0-pre48") == "host on newer 0.8.0-pre50: update HSMMod");
}

// An old host sends no detail; an unreadable or equal version says nothing about who is behind.
static void TestUnknown() {
    const std::string generic = "The host runs another HSMMod version; everyone needs the same one.";
    CHECK(WhoIsBehind("", "0.8.0-pre48") == Behind::Unknown);
    CHECK(VersionRefusalText("", "0.8.0-pre48") == generic);
    CHECK(VersionRefusalText("garbage", "0.8.0-pre48") == generic);
    CHECK(VersionRefusalText("0.8.0-pre48", "0.8.0-pre48") == generic);   // same version, other protocol: a dev build
    CHECK(VersionRefusalText("0.8.0-pre48", "") == generic);
    CHECK(VersionLobbyReason("", "0.8.0-pre48") == "different mod version (host , you 0.8.0-pre48)");
    CHECK(VersionLobbyReason("0.8.0-pre48", "0.8.0-pre48") == "different mod version (host 0.8.0-pre48, you 0.8.0-pre48)");
}

int main() {
    TestTestBuilds();
    TestTheReport();
    TestPlayerBehind();
    TestUnknown();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_version_refusal: OK\n");
    return 0;
}

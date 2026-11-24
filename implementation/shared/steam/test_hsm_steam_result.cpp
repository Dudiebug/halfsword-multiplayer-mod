// Steam call results: each EResult a lobby call answers with has its own text with the number in it, an unknown one
// falls back to its number, and a call times out at kCallTimeoutMs across a tick counter wrap.
#include "steam/hsm_steam_result.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::steam;

static void TestText() {
    CHECK(ResultText(kResultFail) == "Steam failed (error 2), try again");
    CHECK(ResultText(kResultNoConnection) == "Steam is offline (error 3), check your connection");
    CHECK(ResultText(kResultAccessDenied) == "Steam refused it (error 15)");
    CHECK(ResultText(kResultTimeout) == "Steam timed out (error 16), try again");
    CHECK(ResultText(kResultLimitExceeded) == "too many lobbies made (error 25), wait a minute");
    CHECK(ResultText(kResultNoResult) == "Steam gave no answer, try again");
    CHECK(ResultText(kResultNoAnswer) == "Steam did not answer in 20 s, try again");
    CHECK(ResultText(42) == "Steam error 42");
    CHECK(ResultText(kResultOK) == "Steam error 1");   // never shown: 1 is success
}

static void TestTimeout() {
    CHECK(!CallTimedOut(1000, 1000));
    CHECK(!CallTimedOut(1000, 1000 + kCallTimeoutMs - 1));
    CHECK(CallTimedOut(1000, 1000 + kCallTimeoutMs));
    CHECK(CallTimedOut(1000, 1000 + kCallTimeoutMs * 3));
    const uint64_t nearWrap = ~uint64_t(0) - 5000;   // the unsigned difference holds across a wrap
    CHECK(!CallTimedOut(nearWrap, nearWrap + 10000));
    CHECK(CallTimedOut(nearWrap, nearWrap + kCallTimeoutMs));
}

int main() {
    TestText();
    TestTimeout();
    if (g_fail) { std::printf("test_hsm_steam_result: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_steam_result: ok\n");
    return 0;
}

// Unit tests for hsm_ue4ss_wait.h (when HSMMod loads UE4SS; public #4221, #4251, #4255). Run via
// implementation/shared/run_tests.sh ue4ss_wait.
#include "session/hsm_ue4ss_wait.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static Ue4ssLoadWait W(bool waitOn, bool hookIn, bool gameRan, uint32_t waitedMs) {
    Ue4ssLoadWait w; w.waitOn = waitOn; w.hookIn = hookIn; w.gameRan = gameRan; w.waitedMs = waitedMs; return w;
}

// The reports: order=after, HSMMod's hook in, UE4SS loaded 0.6-1.1 s later, its call the first; it now waits for the game's.
static void TestWaitsForTheGame() {
    CHECK(Ue4ssLoadGateOf(W(true, true, false, 0)) == Ue4ssLoadGate::Wait);
    CHECK(Ue4ssLoadGateOf(W(true, true, false, 1000)) == Ue4ssLoadGate::Wait);
    CHECK(Ue4ssLoadGateOf(W(true, true, false, kUe4ssLoadWaitMs - 1)) == Ue4ssLoadGate::Wait);
    CHECK(Ue4ssLoadGateOf(W(true, true, true, 0)) == Ue4ssLoadGate::GameRan);       // already ran: no wait at all
    CHECK(Ue4ssLoadGateOf(W(true, true, true, 4744)) == Ue4ssLoadGate::GameRan);    // the median start
}

static void TestTimeout() {
    CHECK(Ue4ssLoadGateOf(W(true, true, false, kUe4ssLoadWaitMs)) == Ue4ssLoadGate::TimedOut);
    CHECK(Ue4ssLoadGateOf(W(true, true, false, 0xFFFFFFFFu)) == Ue4ssLoadGate::TimedOut);
    CHECK(Ue4ssLoadGateOf(W(true, true, true, kUe4ssLoadWaitMs)) == Ue4ssLoadGate::GameRan);   // the call counts first
    CHECK(kUe4ssLoadWaitMs >= 58000);                                                           // the longest wait seen
    CHECK(kUe4ssLoadWaitMs + kUe4ssHookWaitMs < kUe4ssPendingWaitMs);
}

// No hook to see the call (role=off, ue4ss_order=before, a failed start), or ue4ss_wait=0: at once, as before.
static void TestNoWait() {
    CHECK(Ue4ssLoadGateOf(W(true, false, false, 0)) == Ue4ssLoadGate::NoWait);
    CHECK(Ue4ssLoadGateOf(W(false, true, false, 0)) == Ue4ssLoadGate::NoWait);
    CHECK(Ue4ssLoadGateOf(W(false, false, true, 0)) == Ue4ssLoadGate::NoWait);
    CHECK(Ue4ssLoadGateOf(W(false, true, true, kUe4ssLoadWaitMs)) == Ue4ssLoadGate::NoWait);
    CHECK(Ue4ssLoadGateOf(Ue4ssLoadWait{}) == Ue4ssLoadGate::NoWait);              // default: no hook
}

static void TestText() {
    CHECK(std::string(Ue4ssLoadGateText(Ue4ssLoadGate::Wait)) == "waiting");
    CHECK(std::string(Ue4ssLoadGateText(Ue4ssLoadGate::NoWait)) == "no wait");
    CHECK(std::string(Ue4ssLoadGateText(Ue4ssLoadGate::GameRan)) == "the game made its first ProcessEvent call");
    CHECK(std::string(Ue4ssLoadGateText(Ue4ssLoadGate::TimedOut)) == "no ProcessEvent call from the game in time");
}

int main() {
    TestWaitsForTheGame(); TestTimeout(); TestNoWait(); TestText();
    if (g_fail) { std::printf("test_hsm_ue4ss_wait: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_ue4ss_wait: ok\n");
    return 0;
}

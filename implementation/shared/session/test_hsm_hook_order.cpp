// Unit tests for hsm_hook_order.h. Run via implementation/shared/run_tests.sh test_hsm_hook_order.
#include "session/hsm_hook_order.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static VmHookWait W(Ue4ssPhase p, bool foreign, uint32_t waited, uint32_t sinceLoaded) {
    VmHookWait w; w.phase = p; w.foreignHook = foreign; w.waitedMs = waited; w.sinceLoadedMs = sinceLoaded;
    return w;
}

int main() {
    // The reports: HSMMod wanted its hook while UE4SS's prompt was up, then 0.4 s after UE4SS loaded. Both wait.
    CHECK(!VmHookMayInstall(W(Ue4ssPhase::Pending, false, 0, 0)));
    CHECK(!VmHookMayInstall(W(Ue4ssPhase::Pending, false, 11000, 0)));
    CHECK(!VmHookMayInstall(W(Ue4ssPhase::Loaded, false, 11500, 400)));
    // Every other start: UE4SS's jump is there by the first tick.
    CHECK(VmHookMayInstall(W(Ue4ssPhase::Loaded, true, 0, 4500)));
    CHECK(VmHookMayInstall(W(Ue4ssPhase::Pending, true, 0, 0)));     // a UE4SS that loaded itself
    // No UE4SS: at once.
    CHECK(VmHookMayInstall(W(Ue4ssPhase::Absent, false, 0, 0)));
    // A UE4SS that never hooks the VM, a prompt nobody answers: the wait ends.
    CHECK(!VmHookMayInstall(W(Ue4ssPhase::Loaded, false, 60000, kUe4ssHookWaitMs - 1)));
    CHECK(VmHookMayInstall(W(Ue4ssPhase::Loaded, false, 60000, kUe4ssHookWaitMs)));
    CHECK(!VmHookMayInstall(W(Ue4ssPhase::Pending, false, kUe4ssPendingWaitMs - 1, 0)));
    CHECK(VmHookMayInstall(W(Ue4ssPhase::Pending, false, kUe4ssPendingWaitMs, 0)));
    CHECK(!VmHookMayInstall(W(Ue4ssPhase::Loaded, false, kUe4ssPendingWaitMs, 100)));   // loaded at last: its own time now
    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("hsm_hook_order: all tests passed\n");
    return 0;
}

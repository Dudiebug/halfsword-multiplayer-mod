// When HSMMod may hook the script VM's executor, a function UE4SS hooks too.
//
// UE4SS patches code with the game's threads running. Put in after HSMMod's hook, it writes over the jump HSMMod's hook
// runs through, and a thread in that jump reads half of it: a crash at HSMMod's own jump, 0.7 to 0.9 s after UE4SS loaded
// (reports of 2026-11; in each one UE4SS was loaded late, after its "replace this build?" prompt, and HSMMod had hooked
// first). Put in before, HSMMod chains onto UE4SS's jump with every thread held, which is how every other start goes.
// So while UE4SS is on its way, HSMMod's hook waits for UE4SS's.
#pragma once
#include <cstdint>

namespace hsm {

enum class Ue4ssPhase : uint8_t {
    Pending,   // the load is not decided yet (a prompt may be on screen)
    Absent,    // no UE4SS this run: off, not installed, or it failed to load
    Loaded,    // UE4SS runs in this process
};

constexpr uint32_t kUe4ssHookWaitMs = 20000;       // after UE4SS loaded: its hook is in within about 5 s of its start
constexpr uint32_t kUe4ssPendingWaitMs = 120000;   // while its load is undecided

struct VmHookWait {
    Ue4ssPhase phase = Ue4ssPhase::Pending;
    bool foreignHook = false;      // the executor already starts with another mod's jump
    uint32_t waitedMs = 0;         // since HSMMod first wanted its hook
    uint32_t sinceLoadedMs = 0;    // Loaded: since UE4SS was loaded
};

// true: hook now. UE4SS's hook is in, or there is no UE4SS, or it had its time (a UE4SS set not to hook the VM).
inline bool VmHookMayInstall(const VmHookWait& w) {
    if (w.foreignHook || w.phase == Ue4ssPhase::Absent) return true;
    if (w.phase == Ue4ssPhase::Pending) return w.waitedMs >= kUe4ssPendingWaitMs;
    return w.sinceLoadedMs >= kUe4ssHookWaitMs;
}

}  // namespace hsm

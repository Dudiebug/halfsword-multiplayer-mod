// When HSMMod may hook the script VM's executor, a function UE4SS hooks too.
//
// UE4SS finds the function to hook by following jumps from its start (RC::ASM::resolve_function_address_from_potential_jmp),
// and for a `jmp [rip+0]` it takes the address of the pointer behind the jump as the target, not the pointer. Put in after
// HSMMod's hook, with MinHook's stock relay (hook_relay=legacy), UE4SS then hooks or calls the 8 bytes of address at relay+6:
// a crash at HSMMod's own relay, 0.4 to 0.9 s after UE4SS loaded (public #3172; UE4SS loaded late, after its "replace this
// build?" prompt, and HSMMod had hooked first). The default relay (hsm_relay.h) is code UE4SS stops at. Put in before,
// HSMMod chains onto UE4SS's jump with every thread held, which is how every other start goes. So while UE4SS is on its
// way, HSMMod's hook still waits for UE4SS's.
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

// Worker thread: load config, then (unless role=off) bring up the engine layer and networking.
#include <windows.h>
#include "config.h"
#include "hsm_protocol.h"
#include "gameplay/save_guard.h"
#include "log.h"
#include "ue4ss_load.h"

namespace hsm {
bool StartMod(const Config& cfg);   // implemented in gameplay/session.cpp

DWORD WINAPI ModMain(void*) {
    LogInit();
    InstallCrashLog();
    HSM_LOG("HalfSwordMP loaded (protocol rev %u)", unsigned(kProtocolRevision));
    Config cfg = LoadConfig();
    game::SweepAtStartup();   // S1: before the role gate, so role=off still handles a crashed session's backup
    // UE4SS is chain-loaded here, on the worker thread (never in DllMain: the loader lock). role=off still loads it:
    // HSMMod's proxy replaced UE4SS's, so skipping it would silently turn the player's mods off.
    if (cfg.ue4ssBefore) ue4ss::Run(cfg);
    if (cfg.role == Role::Off) { HSM_LOG("role=off: staying vanilla"); ue4ss::Run(cfg); return 0; }
    if (!StartMod(cfg)) HSM_LOG("startup failed; game continues unmodified");
    ue4ss::Run(cfg);   // order=after: StartMod ends with InstallProcessEventHook, so HSMMod's hook is in (a no-op if already run)
    return 0;
}
}  // namespace hsm

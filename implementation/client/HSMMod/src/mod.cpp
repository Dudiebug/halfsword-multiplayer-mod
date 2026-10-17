#include "diagnostics/stall.h"
// Worker thread: load config, then (unless role=off) bring up the engine layer and networking.
#include <windows.h>
#include "config.h"
#include "hsm_protocol.h"
#include "log.h"

namespace hsm {
bool StartMod(const Config& cfg);   // implemented in gameplay/session.cpp

DWORD WINAPI ModMain(void*) {
    LogInit();
    HSM_LOG("HalfSwordMP loaded (protocol rev %u)", unsigned(kProtocolRevision));
    Config cfg = LoadConfig();
    if (cfg.role == Role::Off) { HSM_LOG("role=off: staying vanilla"); return 0; }
    if (!StartMod(cfg)) HSM_LOG("startup failed; game continues unmodified");
    else diagnostics::RunWatchdog();
    return 0;
}
}  // namespace hsm

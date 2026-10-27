// Last words in HSMMod.log when the game faults: a crash otherwise just ends the log mid-session. A vectored handler
// sees the fault first (before the engine's crash reporter), writes code, address and module+offset, and lets the
// fault carry on unchanged. It never handles anything. Faults inside KernelBase are skipped: IsBadReadPtr probes fault
// there by design and are caught.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include "log.h"

namespace hsm {
namespace {
std::atomic<int> g_logged{0};

bool Fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case 0xC0000374 /* heap corruption */: case 0xC0000409 /* stack buffer overrun */:
            return true;
        default: return false;
    }
}

LONG CALLBACK OnFault(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
    if (!Fatal(r.ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(r.ExceptionAddress), &mod) && mod) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            const char* base = path;
            for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
            std::snprintf(name, sizeof name, "%s", base);
        }
    }
    if (_stricmp(name, "KernelBase.dll") == 0) return EXCEPTION_CONTINUE_SEARCH;
    if (g_logged.fetch_add(1) >= 8) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t off = mod ? uintptr_t(r.ExceptionAddress) - uintptr_t(mod) : uintptr_t(r.ExceptionAddress);
    unsigned long long target = r.NumberParameters >= 2 ? (unsigned long long)r.ExceptionInformation[1] : 0;
    const char* op = r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 1
        ? (r.ExceptionInformation[0] == 0 ? " reading" : r.ExceptionInformation[0] == 1 ? " writing" : " executing") : "";
    LogTry("crash: fault 0x%08lX at %s+0x%llX%s 0x%llX (thread %lu); the game may close now",
           r.ExceptionCode, name, (unsigned long long)off, op, target, GetCurrentThreadId());
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashLog() {
    if (AddVectoredExceptionHandler(1, OnFault)) HSM_LOG("crash log: armed");
}
}  // namespace hsm

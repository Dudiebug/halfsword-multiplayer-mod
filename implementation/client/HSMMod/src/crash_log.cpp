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
std::atomic<uintptr_t> g_last{0};   // the same fault again (a crash reporter retrying it): logged once

bool Fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case 0xC0000374 /* heap corruption */: case 0xC0000409 /* stack buffer overrun */:
            return true;
        default: return false;
    }
}

// A jump through a bad pointer leaves no module at the fault address, so name the code addresses on the stack instead:
// the first few that land inside a loaded module are the likely callers (a scan, not an unwind; good enough to say
// whether HSMMod or the game made the call).
void ModuleOf(uintptr_t a, char* name, size_t n, uintptr_t* off) {
    HMODULE m = nullptr;
    name[0] = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(a), &m) || !m) return;
    char path[MAX_PATH];
    DWORD k = GetModuleFileNameA(m, path, MAX_PATH);
    if (!k || k >= MAX_PATH) return;
    const char* base = path;
    for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    std::snprintf(name, n, "%s", base);
    *off = a - uintptr_t(m);
}

void LogCallers(uintptr_t rsp) {
    char line[512]; size_t used = 0; int found = 0;
    line[0] = 0;
    for (int i = 0; i < 256 && found < 6; ++i) {
        uintptr_t v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(rsp + uintptr_t(i) * 8), &v, sizeof v, nullptr)) break;
        if (v < 0x10000) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<const void*>(v), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        char mod[MAX_PATH]; uintptr_t off = 0;
        ModuleOf(v, mod, sizeof mod, &off);
        if (!mod[0]) continue;
        int w = std::snprintf(line + used, sizeof line - used, " %s+0x%llX", mod, (unsigned long long)off);
        if (w <= 0 || size_t(w) >= sizeof line - used) break;
        used += size_t(w); ++found;
    }
    if (found) LogTry("crash: stack code addresses:%s", line);
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
    if (g_last.exchange(uintptr_t(r.ExceptionAddress)) == uintptr_t(r.ExceptionAddress)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_logged.fetch_add(1) >= 8) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t off = mod ? uintptr_t(r.ExceptionAddress) - uintptr_t(mod) : uintptr_t(r.ExceptionAddress);
    unsigned long long target = r.NumberParameters >= 2 ? (unsigned long long)r.ExceptionInformation[1] : 0;
    const char* op = r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 1
        ? (r.ExceptionInformation[0] == 0 ? " reading" : r.ExceptionInformation[0] == 1 ? " writing" : " executing") : "";
    LogTry("crash: fault 0x%08lX at %s+0x%llX%s 0x%llX (thread %lu); the game may close now",
           r.ExceptionCode, name, (unsigned long long)off, op, target, GetCurrentThreadId());
    if (ep->ContextRecord) LogCallers(ep->ContextRecord->Rsp);
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashLog() {
    if (AddVectoredExceptionHandler(1, OnFault)) HSM_LOG("crash log: armed");
}
}  // namespace hsm

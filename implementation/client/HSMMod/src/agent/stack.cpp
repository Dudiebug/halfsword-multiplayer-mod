#include "agent/stack.h"
#include "agent/hsm_agent.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>

namespace hsm::agent {
namespace {
std::string Frame(DWORD64 pc) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(pc), &mod) && mod) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(mod, path, MAX_PATH)) { const char* b = path; for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') b = p + 1; std::snprintf(name, sizeof name, "%s", b); }
    }
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof out, "%s+0x%llx", name, static_cast<unsigned long long>(mod ? pc - reinterpret_cast<DWORD64>(mod) : pc));
    return out;
}
}  // namespace

std::string SampleStackJson(unsigned long threadId, int maxFrames) {
    if (!threadId) return "{\"ok\":false,\"error\":\"game thread not known yet\"}";
    if (threadId == GetCurrentThreadId()) return "{\"ok\":false,\"error\":\"cannot sample the bridge thread\"}";
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, threadId);
    if (!t) return "{\"ok\":false,\"error\":\"OpenThread failed\"}";
    if (GetProcessIdOfThread(t) != GetCurrentProcessId()) { CloseHandle(t); return "{\"ok\":false,\"error\":\"not a thread of this process\"}"; }
    // Only fixed-size work while the thread is suspended: it may hold the heap or loader lock, so no allocation,
    // string formatting or module lookups until it runs again.
    DWORD64 pcs[64] = {};
    int count = 0;
    if (maxFrames > 64) maxFrames = 64;
    bool suspended = SuspendThread(t) != DWORD(-1);
    if (suspended) {
        CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t, &ctx)) {
            while (count < maxFrames && ctx.Rip) {
                pcs[count++] = ctx.Rip;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!fn) {                                    // leaf function: return address is at RSP (read safely)
                    DWORD64 ret = 0; SIZE_T got = 0;
                    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(ctx.Rsp), &ret, sizeof ret, &got) || got != sizeof ret) break;
                    ctx.Rip = ret; ctx.Rsp += 8;
                    continue;
                }
                void* handlerData = nullptr; DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
        }
        ResumeThread(t);
    }
    if (!suspended) { CloseHandle(t); return "{\"ok\":false,\"error\":\"SuspendThread failed\"}"; }
    std::string frames;
    for (int i = 0; i < count; ++i) frames += (i ? "," : "") + JsonString(Frame(pcs[i]));
    CloseHandle(t);
    char tid[32]; std::snprintf(tid, sizeof tid, "%lu", threadId);
    return std::string("{\"ok\":true,\"thread\":") + tid + ",\"frames\":[" + frames + "]}";
}
std::string ThreadsJson() {
    using GetDescFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    auto getDesc = reinterpret_cast<GetDescFn>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetThreadDescription"));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return "{\"ok\":false,\"error\":\"snapshot failed\"}";
    std::string j = "{\"ok\":true,\"threads\":[";
    bool first = true;
    THREADENTRY32 te{}; te.dwSize = sizeof te;
    for (BOOL more = Thread32First(snap, &te); more; more = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        std::string name;
        if (getDesc) if (HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID)) {
            PWSTR w = nullptr;
            if (SUCCEEDED(getDesc(t, &w)) && w) { for (const wchar_t* p = w; *p; ++p) name.push_back(*p < 128 ? char(*p) : '?'); LocalFree(w); }
            CloseHandle(t);
        }
        char id[32]; std::snprintf(id, sizeof id, "%lu", te.th32ThreadID);
        j += std::string(first ? "" : ",") + "{\"id\":" + id + ",\"name\":" + JsonString(name) + "}";
        first = false;
    }
    CloseHandle(snap);
    return j + "]}";
}
}  // namespace hsm::agent

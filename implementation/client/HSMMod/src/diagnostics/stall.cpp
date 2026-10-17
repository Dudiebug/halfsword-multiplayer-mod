#include "diagnostics/stall.h"
#include "diagnostics/hsm_stall.h"
#include "ue/hook.h"
#include "paths.h"
#include "log.h"
#include <cstdio>
#include <cstring>

namespace hsm::diagnostics {
namespace {
StallTrace g_trace;
bool OnGameThread() { return GetCurrentThreadId() == ue::GameThreadId(); }
void Write(HANDLE file, const char* text) {
    DWORD written = 0;
    WriteFile(file, text, DWORD(std::strlen(text)), &written, nullptr);
}
}
void Heartbeat() { if (OnGameThread()) g_trace.Heartbeat(GetTickCount64()); }
Scope::Scope(const char* stage, uint64_t detail) : active_(OnGameThread()) {
    if (active_) slot_ = g_trace.Push(stage, detail, GetTickCount64());
}
Scope::~Scope() { if (active_) g_trace.Pop(slot_, GetTickCount64()); }
void Scope::Stage(const char* stage) { if (active_) g_trace.Stage(slot_, stage, GetTickCount64()); }
void Scope::Name(const char* name) { if (active_) g_trace.Name(slot_, name, GetTickCount64()); }

void RunWatchdog() {
    const auto path = InstanceFilePath(L"HSMMod-watchdog", L".log");
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { HSM_LOG("watchdog: cannot open diagnostic log (%lu)", GetLastError()); return; }
    char line[512];
    std::snprintf(line, sizeof(line), "watchdog started pid=%lu; stall=5000ms repeat=10000ms; depth limit=32\r\n", GetCurrentProcessId());
    Write(file, line);
    HSM_LOG("watchdog: active; stalls go to %ls (independent log)", path.c_str());
    StallAlarm alarm;
    StallTrace::Snapshot sample;
    for (;;) {
        Sleep(1000);
        const bool fresh = g_trace.Read(sample);
        const uint64_t now = GetTickCount64();
        const auto event = alarm.Poll(now, sample.heartbeat);
        if (event == StallAlarm::Event::None) continue;
        SYSTEMTIME utc; GetSystemTime(&utc);
        std::snprintf(line, sizeof(line),
            "%04u-%02u-%02uT%02u:%02u:%02uZ %s tid=%lu tick_age_ms=%llu progress_age_ms=%llu serial=%llu depth=%zu snapshot=%s\r\n",
            utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
            event == StallAlarm::Event::Stalled ? "STALL" : "RECOVERED", ue::GameThreadId(),
            static_cast<unsigned long long>(now - sample.heartbeat),
            static_cast<unsigned long long>(now - sample.progress), static_cast<unsigned long long>(sample.serial),
            sample.depth, fresh ? "current" : "last coherent");
        Write(file, line);
        if (event == StallAlarm::Event::Stalled) {
            for (size_t i = 0; i < std::min(sample.depth, StallTrace::kDepth); ++i) {
                const auto& f = sample.frames[i];
                std::snprintf(line, sizeof(line), "  #%zu %s age_ms=%llu detail=0x%llx function=%s\r\n", i,
                    f.stage ? f.stage : "?", static_cast<unsigned long long>(now - f.since),
                    static_cast<unsigned long long>(f.detail), f.name.data());
                Write(file, line);
            }
            if (sample.depth > StallTrace::kDepth) Write(file, "  deeper scopes omitted (depth limit)\r\n");
        }
        FlushFileBuffers(file);
    }
}
} // namespace hsm::diagnostics

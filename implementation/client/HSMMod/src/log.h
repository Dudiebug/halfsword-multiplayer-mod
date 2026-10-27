// Minimal thread-safe file logger: <game exe dir>/HSMMod.log
#pragma once
#include <cstdarg>

namespace hsm {
void LogInit();
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Like Log, but gives up instead of waiting if another thread holds the log (for the fault handler).
void LogTry(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void InstallCrashLog();   // crash_log.cpp
// Logs "<what>: the code addresses on this thread's stack" as module+offset (crash_log.cpp), for a call that is refused.
void LogCallStack(const char* what);
}  // namespace hsm

#define HSM_LOG(...) ::hsm::Log(__VA_ARGS__)

#include "log.h"
#include "paths.h"
#include <windows.h>
#include <cstdio>
#include <mutex>

namespace hsm {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
}

void LogInit() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) return;
    std::wstring path = InstanceFilePath(L"HSMMod", L".log");
    MoveFileExW(path.c_str(), InstanceFilePath(L"HSMMod", L".prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);   // keep the last session
    g_file = _wfopen(path.c_str(), L"w");
}

namespace {
void Write(bool wait, const char* fmt, va_list ap) {
    char line[2048];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = std::snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d][%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    std::vsnprintf(line + n, sizeof line - size_t(n), fmt, ap);
    std::unique_lock<std::mutex> lock(g_mu, std::defer_lock);
    if (wait) lock.lock(); else if (!lock.try_lock()) return;
    if (!g_file) return;
    std::fputs(line, g_file); std::fputc('\n', g_file); std::fflush(g_file);
}
}  // namespace

void Log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write(true, fmt, ap); va_end(ap); }
void LogTry(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write(false, fmt, ap); va_end(ap); }
}  // namespace hsm

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

void Log(const char* fmt, ...) {
    char line[2048];
    SYSTEMTIME t; GetLocalTime(&t);
    int n = std::snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d][%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(line + n, sizeof line - size_t(n), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_file) return;
    std::fputs(line, g_file); std::fputc('\n', g_file); std::fflush(g_file);
}
}  // namespace hsm

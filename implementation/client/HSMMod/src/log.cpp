#include "log.h"
#include "paths.h"
#include "util/hsm_logguard.h"
#include "session/hsm_report.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hsm {
namespace {
std::mutex g_mu;
FILE* g_file = nullptr;
util::LogGuard g_guard;   // under g_mu: the session size cap, the lines-per-second throttle, the collapse of repeats (M3)
wchar_t g_exitPath[MAX_PATH] = {};   // HSMMod.log, for LogExit; set once the log is open
}

void LogInit() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) return;
    std::wstring path = InstanceFilePath(L"HSMMod", L".log");
    MoveFileExW(path.c_str(), InstanceFilePath(L"HSMMod", L".prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);   // keep the last session
    g_file = _wfopen(path.c_str(), L"w");
    if (g_file && path.size() < MAX_PATH) std::wcscpy(g_exitPath, path.c_str());
}

void LogExit() {
    if (!g_exitPath[0]) return;
    HANDLE h = CreateFileW(g_exitPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char line[128];
    SYSTEMTIME t; GetLocalTime(&t);
    const int n = std::snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d][%lu] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                                GetCurrentThreadId(), kCleanExitLine);
    DWORD wrote = 0;
    if (n > 0 && size_t(n) < sizeof line) WriteFile(h, line, DWORD(n), &wrote, nullptr);
    CloseHandle(h);
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
    if (wait) {   // LogTry is the crash path: its lines always go out, past the cap and the throttle
        const util::LogGuard::Verdict v = g_guard.Filter(line + n, double(GetTickCount64()));
        if (!v.note.empty()) {
            char head[64];
            const int h = std::snprintf(head, sizeof head, "[%02d:%02d:%02d.%03d][log] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            std::fputs(head, g_file); std::fputs(v.note.c_str(), g_file); std::fputc('\n', g_file);
            g_guard.Wrote(size_t(h) + v.note.size() + 1);
        }
        if (!v.write) { if (!v.note.empty()) std::fflush(g_file); return; }
        g_guard.Wrote(std::strlen(line) + 1);
    }
    std::fputs(line, g_file); std::fputc('\n', g_file); std::fflush(g_file);
}
}  // namespace

void Log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write(true, fmt, ap); va_end(ap); }
void LogTry(const char* fmt, ...) { va_list ap; va_start(ap, fmt); Write(false, fmt, ap); va_end(ap); }
}  // namespace hsm

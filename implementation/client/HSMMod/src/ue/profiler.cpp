#include "ue/profiler.h"
#include "ue/engine.h"
#include "ue/hook.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <intrin.h>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace hsm::ue::prof {
std::atomic<bool> g_recording{false};

namespace {
constexpr double kDefaultSeconds = 30, kMaxSeconds = 300;
constexpr double kDefaultTimelineUs = 20;
constexpr size_t kTimelineMaxEvents = 1000000;   // about 100 MB of JSON at most

hsm::prof::Profiler g_prof;   // game thread only
double g_limitSeconds = kDefaultSeconds, g_timelineUs = kDefaultTimelineUs, g_overheadNs = 0;
uint64_t g_tsc0 = 0;
int64_t g_qpc0 = 0;
bool g_keyDown = false;

int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
double QpcMs(int64_t ticks) {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return double(f.QuadPart); }();
    return double(ticks) * 1000.0 / freq;
}
uint64_t Now() { return __rdtsc(); }

// TSC ticks per millisecond over the capture so far (QueryPerformanceCounter as the reference).
double TicksPerMs() {
    const double ms = QpcMs(Qpc() - g_qpc0);
    return ms > 0 ? double(Now() - g_tsc0) / ms : 1.0;
}

std::string NameOf(const void* key, uint8_t kind) {
    if (kind == hsm::prof::kMod) return static_cast<const char*>(key);
    const UObject* fn = static_cast<const UObject*>(key);
    const UObject* owner = OuterOf(fn);
    return (owner ? ObjectName(owner) + "." : std::string()) + ObjectName(fn);
}

// One Begin/End pair as the detours pay it (thread check included), measured on a scratch profiler, ns.
double MeasureOverheadNs() {
    hsm::prof::Profiler p;
    p.Start(Now(), {});
    static int key;
    constexpr int kIters = 20000;
    const int64_t t0 = Qpc();
    for (int i = 0; i < kIters; ++i) {
        if (GetCurrentThreadId() == GameThreadId() && p.Begin(&key, hsm::prof::kScript, Now(), [] { return std::string("x"); }))
            p.End(Now());
    }
    return QpcMs(Qpc() - t0) * 1e6 / kIters;
}

std::string Json(bool ok, const std::string& fields) { return std::string("{\"ok\":") + (ok ? "true" : "false") + fields + "}"; }

std::string Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    std::snprintf(b, sizeof b, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

bool WriteText(const std::wstring& path, const std::string& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }   // ASCII file names only

std::string Start(double seconds, double timelineUs) {
    if (g_prof.recording()) return "profiler: already recording";
    if (!GameThreadId()) return "profiler: the game thread is not known yet";
    g_limitSeconds = seconds > 0 ? std::min(seconds, kMaxSeconds) : kDefaultSeconds;
    g_timelineUs = timelineUs >= 0 ? timelineUs : kDefaultTimelineUs;
    g_overheadNs = MeasureOverheadNs();
    g_qpc0 = Qpc();
    g_tsc0 = Now();
    // The TSC rate is only known once the capture ends; the timeline threshold uses a 1 ms estimate of it.
    Sleep(1);
    const double tpm = TicksPerMs();
    g_qpc0 = Qpc();
    g_tsc0 = Now();
    hsm::prof::Options o;
    o.traceMinTicks = uint64_t(g_timelineUs * tpm / 1000.0);
    o.traceMaxEvents = kTimelineMaxEvents;
    g_prof.Start(g_tsc0, o);
    g_recording.store(true, std::memory_order_relaxed);
    char b[160];
    std::snprintf(b, sizeof b, "profiler: recording for up to %.0f s (Ctrl+Shift+P stops); timeline keeps calls of %.0f us and more",
                  g_limitSeconds, g_timelineUs);
    HSM_LOG("%s; %.0f ns per call measured", b, g_overheadNs);
    return b;
}

std::string Stop() {
    if (!g_prof.recording()) return "profiler: not recording";
    g_recording.store(false, std::memory_order_relaxed);
    const double tpm = TicksPerMs();
    g_prof.Stop(Now());
    const std::string base = "HSMProfile-" + Stamp();
    hsm::prof::ReportInfo r;
    r.ticksPerMs = tpm;
    r.overheadNsPerCall = g_overheadNs;
    r.title = "HSMMod Blueprint profile " + base;
    if (!g_prof.trace().empty() || g_prof.frameMarks().size() > 1) {
        bool ok = false;
        if (FILE* f = _wfopen(ModFilePath(Wide(base + ".json").c_str()).c_str(), L"wb")) {
            ok = true;
            hsm::prof::WriteTrace(g_prof, tpm, [&](std::string_view s) { ok = ok && std::fwrite(s.data(), 1, s.size(), f) == s.size(); });
            ok = std::fclose(f) == 0 && ok;
        }
        if (ok) r.traceFile = base + ".json";
        else HSM_LOG("profiler: could not write %s.json", base.c_str());
    }
    const std::string report = hsm::prof::Report(g_prof, r);
    const bool saved = WriteText(ModFilePath(Wide(base + ".txt").c_str()), report);
    const std::vector<uint64_t> ft = g_prof.FrameTicks();
    double avg = 0;
    for (uint64_t t : ft) avg += double(t);
    if (!ft.empty()) avg = avg / double(ft.size()) / tpm;
    char b[200];
    std::snprintf(b, sizeof b, "profiler: %s %s.txt (%.1f s, %zu frames, avg %.1f ms)", saved ? "saved" : "could not write",
                  base.c_str(), double(g_prof.durationTicks()) / tpm / 1000.0, ft.size(), avg);
    HSM_LOG("%s; %llu calls, %zu functions, %zu timeline events (%llu not kept)", b, (unsigned long long)g_prof.calls(),
            g_prof.stats().size(), g_prof.trace().size(), (unsigned long long)g_prof.dropped());
    return b;
}

bool LimitReached() { return g_prof.recording() && QpcMs(Qpc() - g_qpc0) >= g_limitSeconds * 1000.0; }
}  // namespace

std::string StopCapture() { return g_prof.recording() ? Stop() : std::string(); }

bool BeginCall(const void* key, Kind kind) {
    if (GetCurrentThreadId() != GameThreadId()) return false;
    return g_prof.Begin(key, kind, Now(), [&] { return NameOf(key, kind); });
}

void EndCall() { g_prof.End(Now()); }

std::string Frame() {
    if (!g_prof.recording()) return {};
    g_prof.FrameMark(Now());
    return LimitReached() ? Stop() : std::string();
}

std::string PollHotkey() {
    // The keys first: asking whose window is in front is the dearer call (Proton), and is made only while P is held.
    bool down = (GetAsyncKeyState('P') & 0x8000) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    if (down) {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        down = pid == GetCurrentProcessId();
    }
    const bool pressed = down && !g_keyDown;
    g_keyDown = down;
    if (pressed) return g_prof.recording() ? Stop() : Start(kDefaultSeconds, kDefaultTimelineUs);
    return LimitReached() ? Stop() : std::string();   // a capture outside any game mode tick still ends
}

std::string Command(const std::vector<std::string>& args) {
    const std::string verb = args.empty() ? "status" : args[0];
    auto quoted = [](const std::string& s) { std::string j = "\""; hsm::prof::JsonEscape(j, s); return j + "\""; };
    if (verb == "start") {
        const double seconds = args.size() > 1 ? std::atof(args[1].c_str()) : kDefaultSeconds;
        const double us = args.size() > 2 ? std::atof(args[2].c_str()) : kDefaultTimelineUs;
        const bool was = g_prof.recording();
        const std::string msg = Start(seconds, us);
        return Json(!was && g_prof.recording(), ",\"message\":" + quoted(msg));
    }
    if (verb == "stop") {
        const bool was = g_prof.recording();
        return Json(was, ",\"message\":" + quoted(Stop()));
    }
    if (verb == "status") {
        char b[160];
        std::snprintf(b, sizeof b, ",\"recording\":%s,\"seconds\":%.2f,\"calls\":%llu,\"functions\":%zu", g_prof.recording() ? "true" : "false",
                      g_prof.recording() ? QpcMs(Qpc() - g_qpc0) / 1000.0 : 0.0, (unsigned long long)g_prof.calls(), g_prof.stats().size());
        return Json(true, b);
    }
    return Json(false, ",\"error\":\"usage: prof start [seconds] [timeline_min_us] | stop | status\"");
}

}  // namespace hsm::ue::prof

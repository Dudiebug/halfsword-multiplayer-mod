#include "net/report_client.h"
#include "net/http_client.h"
#include "gameplay/lobby.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

namespace hsm::report {
namespace {
std::mutex g_mu;
std::deque<Result> g_results;
std::atomic<bool> g_busy{false};
bool g_enabled = false;
bool g_secure = true;
std::wstring g_host, g_base;   // base: "" or "/path" without a trailing slash
uint16_t g_port = 443;
constexpr unsigned kReportTimeoutMs = 120000;   // the relay answers once the issue exists; GitHub itself can take a while

// The end of one log file (shared with the logger, which keeps HSMMod.log open), cut and redacted.
std::optional<ReportLog> ReadLog(const wchar_t* ext, const char* name, uint64_t salt) {
    std::wstring path = InstanceFilePath(L"HSMMod", ext);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return std::nullopt;
    LARGE_INTEGER size{};
    std::string text;
    uint64_t skipped = 0;
    if (GetFileSizeEx(f, &size)) {
        uint64_t total = uint64_t(size.QuadPart);
        skipped = total > kReportLogTailBytes ? total - kReportLogTailBytes : 0;
        LARGE_INTEGER at{}; at.QuadPart = LONGLONG(skipped);
        text.resize(size_t(total - skipped));
        DWORD got = 0;
        if (!SetFilePointerEx(f, at, nullptr, FILE_BEGIN) || !ReadFile(f, text.data(), DWORD(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(f);
    return ReportLog{name, RedactLog(LogTail(text, kReportLogTailBytes, skipped), salt)};
}

std::vector<ReportLog> ReadLogs() {
    std::random_device rd;
    const uint64_t salt = (uint64_t(rd()) << 32) ^ rd() ^ GetTickCount64();   // a new salt per report: ids never match across reports
    std::vector<ReportLog> logs;
    if (auto l = ReadLog(L".log", "HSMMod.log", salt)) logs.push_back(std::move(*l));
    if (auto l = ReadLog(L".prev.log", "HSMMod.prev.log", salt)) logs.push_back(std::move(*l));
    return logs;
}

struct Response { ReportTransport transport = ReportTransport::Ok; bool sent = false; unsigned status = 0; unsigned winError = 0; std::string body; };

Response Post(const std::wstring& path, const std::string& body) {
    using Fail = net::HttpResponse::Fail;
    net::HttpRequest req;
    req.method = L"POST"; req.host = g_host; req.path = path; req.port = g_port; req.secure = g_secure;
    req.headers = L"Content-Type: application/json";
    req.agent = L"HSMMod/" + net::Wide(game::ModBuild());
    req.body = body;
    req.timeoutMs = kReportTimeoutMs;
    net::HttpResponse h = net::Http(req);
    Response r;
    r.status = h.status; r.body = std::move(h.body); r.sent = h.sent; r.winError = h.winError;
    switch (h.fail) {
        case Fail::None: r.transport = ReportTransport::Ok; break;
        case Fail::Unavailable: r.transport = ReportTransport::Unavailable; break;
        case Fail::NoConnection: r.transport = ReportTransport::NoConnection; break;
        case Fail::Timeout: r.transport = ReportTransport::Timeout; break;
        case Fail::Certificate: r.transport = ReportTransport::Certificate; break;
        case Fail::Unreachable: r.transport = ReportTransport::Unreachable; break;
        case Fail::Other: r.transport = ReportTransport::Other; break;
    }
    return r;
}

const char* OutcomeName(ReportOutcome o) {
    return o == ReportOutcome::Done ? "done" : o == ReportOutcome::MaybeDone ? "maybe done (no usable answer after sending)"
         : o == ReportOutcome::NotSent ? "not sent" : "refused";
}

// The judged answer, with one log line of what came back (the player's own line is logged by the session).
Result Judge(ReportJob job, const char* what, const Response& r) {
    Result res;
    res.job = job;
    res.verdict = JudgeReportAnswer(job, r.transport, r.sent, r.status, r.body);
    HSM_LOG("report: %s -> status %u, transport %d (winerror %u), sent %d: %s%s%s", what, r.status, int(r.transport), r.winError, r.sent ? 1 : 0,
            OutcomeName(res.verdict.outcome), res.verdict.reason.empty() ? "" : ", ", res.verdict.reason.c_str());
    return res;
}

// Queues the result and frees the worker slot; the slot is freed even when queueing throws.
void Finish(Result res) {
    try {
        std::lock_guard<std::mutex> lock(g_mu);
        g_results.push_back(std::move(res));
    } catch (...) {}
    g_busy = false;
}

// Runs `work` (it returns the Result) on a detached thread. Whatever it throws becomes a failed Result, and g_busy is
// always reset, so one bad report cannot block every later one (R8).
bool Start(ReportJob job, std::function<Result()> work) {
    if (!g_enabled) return false;
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    try {
        std::thread([job, work = std::move(work)] {
            Result res;
            res.job = job;
            try { res = work(); }
            catch (const std::exception& e) { res.verdict = ReportVerdict{}; res.verdict.reason = "the report failed on this PC"; HSM_LOG("report: worker threw: %s", e.what()); }
            catch (...) { res.verdict = ReportVerdict{}; res.verdict.reason = "the report failed on this PC"; HSM_LOG("report: worker threw"); }
            Finish(std::move(res));
        }).detach();
    } catch (...) {   // no thread could be started
        g_busy = false;
        return false;
    }
    return true;
}
}  // namespace

void Configure(bool enabled, const std::string& url) {
    g_enabled = false;
    if (!enabled) { HSM_LOG("report: in-game bug reports off"); return; }
    std::string rest;
    if (url.rfind("https://", 0) == 0) { g_secure = true; g_port = 443; rest = url.substr(8); }
    else if (url.rfind("http://127.0.0.1", 0) == 0 && (url.size() == 16 || url[16] == '/' || url[16] == ':')) { g_secure = false; g_port = 80; rest = url.substr(7); }   // a local relay, for testing: exactly 127.0.0.1, not 127.0.0.1.evil.example (R6)
    else { HSM_LOG("report: report_url must be https://...; bug reports off"); return; }
    size_t slash = rest.find('/');
    std::string host = rest.substr(0, slash), base = slash == std::string::npos ? "" : rest.substr(slash);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (size_t colon = host.find(':'); colon != std::string::npos) {
        int p = std::atoi(host.c_str() + colon + 1);
        if (p <= 0 || p > 65535) { HSM_LOG("report: bad port in report_url; bug reports off"); return; }
        g_port = uint16_t(p); host.resize(colon);
    }
    for (char ch : host + base)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '/' || ch == '_')) {
            HSM_LOG("report: bad report_url; bug reports off"); return;
        }
    if (host.empty()) { HSM_LOG("report: bad report_url; bug reports off"); return; }
    g_host = net::Wide(host); g_base = net::Wide(base); g_enabled = true;
    HSM_LOG("report: in-game bug reports go to %s (only on /bug or /sendlog)", url.c_str());
}

bool Enabled() { return g_enabled; }
bool Busy() { return g_busy; }

bool Submit(const std::string& description, const ReportFacts& facts, ReportJob job) {
    return Start(job, [description, facts, job]() -> Result {
        Result res = Judge(job, job == ReportJob::Crash ? "crash report" : "submit", Post(g_base + L"/v1/reports", ReportJson(description, facts, ReadLogs())));
        if (res.verdict.outcome == ReportOutcome::Done) HSM_LOG("report: filed as %s", res.verdict.id.c_str());
        return res;
    });
}

bool Attach(const std::string& id, const std::string& token, const ReportFacts& facts) {
    if (!ValidReportId(id) || !ValidReportToken(token)) return false;
    return Start(ReportJob::Log, [id, token, facts]() -> Result {
        const std::string what = "attach to " + id;
        return Judge(ReportJob::Log, what.c_str(), Post(g_base + L"/v1/reports/" + net::Wide(id) + L"/logs", AttachJson(token, facts, ReadLogs())));
    });
}

std::optional<Result> Poll() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_results.empty()) return std::nullopt;
    Result r = std::move(g_results.front());
    g_results.pop_front();
    return r;
}

std::string OsName() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    using WineVersionFn = const char*(CDECL*)();
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    OSVERSIONINFOW v{}; v.dwOSVersionInfoSize = sizeof v;
    std::string win = "Windows";
    if (auto get = nt ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion"))) : nullptr; get && get(&v) == 0)
        win += " " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber);
    if (auto wine = nt ? reinterpret_cast<WineVersionFn>(reinterpret_cast<void*>(GetProcAddress(nt, "wine_get_version"))) : nullptr) {
        std::string w;
        for (const char* p = wine(); p && *p && w.size() < 32; ++p) if (*p >= 0x20 && *p < 0x7F) w.push_back(*p);
        return "Wine " + w + " (" + win + ")";
    }
    return win;
}
}  // namespace hsm::report

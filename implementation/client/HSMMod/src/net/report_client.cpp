#include "net/report_client.h"
#include "gameplay/lobby.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <thread>

namespace hsm::report {
namespace {
// Kept off the import table (tools/check_dll.py): resolved from System32's winhttp.dll on the first request.
struct WinHttp {
    decltype(&WinHttpOpen) Open = nullptr;
    decltype(&WinHttpSetTimeouts) SetTimeouts = nullptr;
    decltype(&WinHttpConnect) Connect = nullptr;
    decltype(&WinHttpOpenRequest) OpenRequest = nullptr;
    decltype(&WinHttpAddRequestHeaders) AddRequestHeaders = nullptr;
    decltype(&WinHttpSendRequest) SendRequest = nullptr;
    decltype(&WinHttpReceiveResponse) ReceiveResponse = nullptr;
    decltype(&WinHttpQueryHeaders) QueryHeaders = nullptr;
    decltype(&WinHttpReadData) ReadData = nullptr;
    decltype(&WinHttpCloseHandle) CloseHandle = nullptr;
    bool ok = false;
};

std::mutex g_mu;
std::deque<Result> g_results;
std::atomic<bool> g_busy{false};
bool g_enabled = false;
bool g_secure = true;
std::wstring g_host, g_base;   // base: "" or "/path" without a trailing slash
INTERNET_PORT g_port = 443;

const WinHttp& Api() {
    static WinHttp api = [] {
        WinHttp a;
        wchar_t dir[MAX_PATH];
        UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return a;
        HMODULE m = LoadLibraryW((std::wstring(dir, n) + L"\\winhttp.dll").c_str());
        if (!m) return a;
        auto get = [m](auto& fn, const char* name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(m, name))); return fn != nullptr; };
        a.ok = get(a.Open, "WinHttpOpen") && get(a.SetTimeouts, "WinHttpSetTimeouts") && get(a.Connect, "WinHttpConnect") &&
               get(a.OpenRequest, "WinHttpOpenRequest") && get(a.AddRequestHeaders, "WinHttpAddRequestHeaders") &&
               get(a.SendRequest, "WinHttpSendRequest") && get(a.ReceiveResponse, "WinHttpReceiveResponse") &&
               get(a.QueryHeaders, "WinHttpQueryHeaders") && get(a.ReadData, "WinHttpReadData") && get(a.CloseHandle, "WinHttpCloseHandle");
        return a;
    }();
    return api;
}

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

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

struct Response { DWORD status = 0; std::string body; std::string error; };

Response Post(const std::wstring& path, const std::string& body) {
    Response r;
    const WinHttp& h = Api();
    if (!h.ok) { r.error = "WinHTTP is unavailable"; return r; }
    const std::wstring agent = L"HSMMod/" + Wide(game::ModBuild());
    // Automatic proxy (Windows 8.1+), else the system default.
    HINTERNET s = h.Open(agent.c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = h.Open(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { r.error = "no connection"; return r; }
    h.SetTimeouts(s, 10000, 10000, 60000, 30000);
    HINTERNET c = h.Connect(s, g_host.c_str(), g_port, 0);
    HINTERNET q = c ? h.OpenRequest(c, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    g_secure ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    // Certificate checks stay on (WinHTTP's default): a relay without a valid certificate gets nothing.
    if (q && h.AddRequestHeaders(q, L"Content-Type: application/json", DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD) &&
        h.SendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0) &&
        h.ReceiveResponse(q, nullptr)) {
        DWORD len = sizeof r.status;
        h.QueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &r.status, &len, WINHTTP_NO_HEADER_INDEX);
        char buf[4096];
        DWORD got = 0;
        while (r.body.size() < 65536 && h.ReadData(q, buf, sizeof buf, &got) && got) r.body.append(buf, got);
    } else {
        DWORD e = GetLastError();
        r.error = e == ERROR_WINHTTP_TIMEOUT ? "the report server did not answer"
                : e == ERROR_WINHTTP_SECURE_FAILURE ? "the report server's certificate is not valid"
                : e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT ? "cannot reach the report server"
                : "request failed (" + std::to_string(e) + ")";
    }
    if (q) h.CloseHandle(q);
    if (c) h.CloseHandle(c);
    h.CloseHandle(s);
    return r;
}

// A short reason from a non-2xx answer: the relay's "error" field (printable ASCII only), else the status.
std::string ErrorOf(const Response& r) {
    if (!r.error.empty()) return r.error;
    if (r.status == 429) return "too many reports right now, try again later";
    std::string e = JsonStringField(r.body, "error");
    std::string clean;
    for (char ch : e) if (ch >= 0x20 && ch < 0x7F && clean.size() < 80) clean.push_back(ch);
    return clean.empty() ? "the report server answered " + std::to_string(r.status) : clean;
}

void Finish(Result res) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_results.push_back(std::move(res));
    g_busy = false;
}

bool Start(std::function<void()> work) {
    if (!g_enabled) return false;
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) return false;
    std::thread(std::move(work)).detach();
    return true;
}
}  // namespace

void Configure(bool enabled, const std::string& url) {
    g_enabled = false;
    if (!enabled) { HSM_LOG("report: in-game bug reports off"); return; }
    std::string rest;
    if (url.rfind("https://", 0) == 0) { g_secure = true; g_port = 443; rest = url.substr(8); }
    else if (url.rfind("http://127.0.0.1", 0) == 0) { g_secure = false; g_port = 80; rest = url.substr(7); }   // a local relay, for testing
    else { HSM_LOG("report: report_url must be https://...; bug reports off"); return; }
    size_t slash = rest.find('/');
    std::string host = rest.substr(0, slash), base = slash == std::string::npos ? "" : rest.substr(slash);
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (size_t colon = host.find(':'); colon != std::string::npos) {
        int p = std::atoi(host.c_str() + colon + 1);
        if (p <= 0 || p > 65535) { HSM_LOG("report: bad port in report_url; bug reports off"); return; }
        g_port = INTERNET_PORT(p); host.resize(colon);
    }
    for (char ch : host + base)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '/' || ch == '_')) {
            HSM_LOG("report: bad report_url; bug reports off"); return;
        }
    if (host.empty()) { HSM_LOG("report: bad report_url; bug reports off"); return; }
    g_host = Wide(host); g_base = Wide(base); g_enabled = true;
    HSM_LOG("report: in-game bug reports go to %s (only on /report or /sendlog)", url.c_str());
}

bool Enabled() { return g_enabled; }
bool Busy() { return g_busy; }

bool Submit(const std::string& description, const ReportFacts& facts) {
    return Start([description, facts] {
        Response r = Post(g_base + L"/v1/reports", ReportJson(description, facts, ReadLogs()));
        Result res;
        if (r.status == 200 || r.status == 201) {
            res.id = JsonStringField(r.body, "id"); res.token = JsonStringField(r.body, "token"); res.issue = JsonStringField(r.body, "issue");
            if (ValidReportId(res.id) && ValidReportToken(res.token)) {
                res.kind = Result::Kind::Filed;
                if (!ValidIssueUrl(res.issue)) res.issue.clear();
            } else res.error = "the report server's answer was not understood";
        } else res.error = ErrorOf(r);
        HSM_LOG("report: submit -> %lu %s", r.status, res.kind == Result::Kind::Filed ? res.id.c_str() : res.error.c_str());
        Finish(std::move(res));
    });
}

bool Attach(const std::string& id, const std::string& token, const ReportFacts& facts) {
    if (!ValidReportId(id) || !ValidReportToken(token)) return false;
    return Start([id, token, facts] {
        Response r = Post(g_base + L"/v1/reports/" + Wide(id) + L"/logs", AttachJson(token, facts, ReadLogs()));
        Result res;
        res.id = id;
        if (r.status == 200 || r.status == 201) res.kind = Result::Kind::Attached;
        else res.error = ErrorOf(r);
        HSM_LOG("report: attach to %s -> %lu %s", id.c_str(), r.status, res.error.c_str());
        Finish(std::move(res));
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

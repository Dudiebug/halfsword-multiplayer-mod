#include "net/http_client.h"
#include <windows.h>
#include <winhttp.h>
#include <type_traits>

namespace hsm::net {
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
}  // namespace

std::wstring Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

HttpResponse Http(const HttpRequest& req) {
    using Fail = HttpResponse::Fail;
    HttpResponse r;
    const WinHttp& h = Api();
    if (!h.ok) { r.fail = Fail::Unavailable; return r; }
    // Automatic proxy (Windows 8.1+), else the system default.
    HINTERNET s = h.Open(req.agent.c_str(), 4 /* WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY */, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = h.Open(req.agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { r.fail = Fail::NoConnection; return r; }
    h.SetTimeouts(s, 10000, 10000, int(req.timeoutMs), int(req.timeoutMs));
    HINTERNET c = h.Connect(s, req.host.c_str(), INTERNET_PORT(req.port), 0);
    HINTERNET q = c ? h.OpenRequest(c, req.method.c_str(), req.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    req.secure ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    // Certificate checks stay on (WinHTTP's default): a server without a valid certificate gets nothing.
    void* data = req.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(req.body.data());
    if (q && (req.headers.empty() || h.AddRequestHeaders(q, req.headers.c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD)) &&
        h.SendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, DWORD(req.body.size()), DWORD(req.body.size()), 0) &&
        (r.sent = true, h.ReceiveResponse(q, nullptr))) {
        DWORD status = 0, len = sizeof status;
        h.QueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = status;
        char buf[4096];
        DWORD got = 0;
        while (r.body.size() < req.maxBody && h.ReadData(q, buf, sizeof buf, &got) && got) r.body.append(buf, got);
        if (r.body.size() > req.maxBody) r.body.resize(req.maxBody);
    } else {
        DWORD e = GetLastError();
        r.fail = e == ERROR_WINHTTP_TIMEOUT ? Fail::Timeout
               : e == ERROR_WINHTTP_SECURE_FAILURE ? Fail::Certificate
               : e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT ? Fail::Unreachable
               : Fail::Other;
        r.winError = e;
    }
    if (q) h.CloseHandle(q);
    if (c) h.CloseHandle(c);
    h.CloseHandle(s);
    return r;
}
}  // namespace hsm::net

// One blocking HTTPS request over WinHTTP, loaded from System32 on the first call so the DLL imports nothing new
// (tools/check_dll.py). Shared by the bug reports (report_client) and the update check (update_check); call it from a
// worker thread, never the game thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace hsm::net {
struct HttpRequest {
    std::wstring method = L"GET", host, path, headers;   // headers: "Name: value" lines joined by \r\n, or empty
    std::wstring agent;                                  // User-Agent
    std::string body;
    uint16_t port = 443;
    bool secure = true;                                  // false only for a local relay (127.0.0.1), for testing
    size_t maxBody = 65536;                              // the answer is read up to this many bytes, the rest dropped
    unsigned timeoutMs = 60000;                          // sending and receiving; resolving and connecting take 10 s
};

struct HttpResponse {
    enum class Fail : uint8_t { None, Unavailable, NoConnection, Timeout, Certificate, Unreachable, Other } fail = Fail::None;
    unsigned status = 0;      // HTTP status when fail == None
    unsigned winError = 0;    // Other: GetLastError()
    bool sent = false;        // the request and its body went out (WinHttpSendRequest returned), so the server may have acted on it
    std::string body;
};

HttpResponse Http(const HttpRequest& req);
std::wstring Wide(const std::string& utf8);
}  // namespace hsm::net

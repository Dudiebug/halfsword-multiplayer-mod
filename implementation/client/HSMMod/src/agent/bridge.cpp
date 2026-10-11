#include "agent/bridge.h"
#include "agent/input.h"
#include "agent/stack.h"
#include "ue/hook.h"
#include "log.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <thread>

namespace hsm::agent {
namespace {

struct Request { AgentCommand cmd; std::promise<std::string> reply; std::atomic<bool> abandoned{false}; };

std::mutex g_mu;
std::deque<std::shared_ptr<Request>> g_queue;
bool g_enabled = false;
constexpr auto kReplyTimeout = std::chrono::seconds(10);
constexpr size_t kMaxQueued = 32;

std::string Error(const std::string& why) { return "{\"ok\":false,\"error\":" + JsonString(why) + "}"; }

// Browsers can reach 127.0.0.1; their request line and headers would otherwise be parsed as commands.
bool LooksLikeHttp(const std::string& line) {
    for (const char* m : {"GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH ", "CONNECT "})
        if (line.rfind(m, 0) == 0) return true;
    return line.find(" HTTP/") != std::string::npos || line.find("Host:") == 0 || line.find("host:") == 0;
}

void SendLine(SOCKET s, const std::string& line) {
    std::string out = line + "\n";
    const char* p = out.data(); int left = int(out.size());
    while (left > 0) { int n = send(s, p, left, 0); if (n <= 0) return; p += n; left -= n; }
}

std::string Ask(const std::string& line) {
    AgentCommand cmd;
    if (!ParseCommand(line, cmd)) return Error("unparsable command");
    std::string direct;
    if (HandleInput(cmd, direct)) return direct;          // window input needs no game thread
    if (cmd.verb == "threads") return ThreadsJson();
    if (cmd.verb == "stack") {                             // sample the game thread (or a given thread id) where it is
        unsigned long tid = cmd.args.empty() ? ue::GameThreadId() : std::strtoul(cmd.args[0].c_str(), nullptr, 10);
        return SampleStackJson(tid, 32);
    }
    auto req = std::make_shared<Request>();
    req->cmd = std::move(cmd);
    std::future<std::string> f = req->reply.get_future();
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_queue.size() >= kMaxQueued) return Error("busy");
        g_queue.push_back(req);
    }
    if (f.wait_for(kReplyTimeout) != std::future_status::ready) {
        req->abandoned = true;                             // the caller got an error; never run it later
        return Error("game thread did not answer (not ticking?)");
    }
    try { return f.get(); } catch (const std::exception& e) { return Error(e.what()); }
}

void Serve(SOCKET client) {
    std::string buf;
    char chunk[512];
    for (;;) {
        int n = recv(client, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buf.append(chunk, size_t(n));
        if (buf.size() > 4 * kMaxCommandLine) { SendLine(client, Error("line too long")); break; }
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (LooksLikeHttp(line)) { closesocket(client); return; }   // a web page cannot drive the bridge
            SendLine(client, Ask(line));
        }
    }
    closesocket(client);
}

void Listen(SOCKET server) {
    for (;;) {
        SOCKET c = accept(server, nullptr, nullptr);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        Serve(c);
    }
}
}  // namespace

bool Enabled() { return g_enabled; }

bool Start(uint16_t port) {
    if (!port) return false;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never exposed off-machine
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(s, 1) != 0) {
        HSM_LOG("bridge: cannot listen on 127.0.0.1:%u", port); closesocket(s); return false;
    }
    g_enabled = true;
    std::thread(Listen, s).detach();
    HSM_LOG("bridge: listening on 127.0.0.1:%u", port);
    return true;
}

void Tick(const Handler& handler) {
    if (!g_enabled) return;
    std::deque<std::shared_ptr<Request>> work;
    { std::lock_guard<std::mutex> lock(g_mu); work.swap(g_queue); }
    for (auto& r : work) {
        if (r->abandoned) continue;
        try { r->reply.set_value(handler(r->cmd)); }
        catch (const std::exception& e) { r->reply.set_value(Error(std::string("handler threw: ") + e.what())); }
    }
}

}  // namespace hsm::agent

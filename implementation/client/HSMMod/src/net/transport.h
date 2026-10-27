// Transports. ITransport is what the session uses; ENetTransport (direct IP/LAN, own network thread) and
// SteamTransport (Steam relay via ISteamNetworkingMessages) implement it. Incoming payloads are validated
// before they are queued for the game thread.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hsm {

struct NetEvent {
    enum Kind { Connected, Disconnected, Data } kind = Data;
    uint32_t peer = 0;
    uint64_t steamId = 0;   // set by SteamTransport
    uint8_t channel = 0;
    std::vector<uint8_t> data;
    double rxMs = 0;        // Data: when it arrived, on the steady clock (NetSteadyMs; 0 = unknown, use the poll time)
};

// Milliseconds on std::chrono::steady_clock, the clock of game::SteadyNowMs (owner_sync.cpp), from any thread.
inline double NetSteadyMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class ITransport {
public:
    using Validator = std::function<bool(const uint8_t* data, size_t size, uint8_t channel)>;
    virtual ~ITransport() = default;
    virtual void Stop() = 0;
    virtual void Send(uint32_t peer, uint8_t channel, std::vector<uint8_t> data, bool reliable) = 0;
    virtual void Broadcast(uint8_t channel, std::vector<uint8_t> data, bool reliable) = 0;
    virtual void Disconnect(uint32_t peer) = 0;
    virtual bool Poll(NetEvent& out) = 0;
    virtual uint32_t RoundTripMs(uint32_t peer) = 0;
    // The peer's IPv4 address ("a.b.c.d"), for a direct-IP ban; empty where the transport has none (Steam).
    virtual std::string PeerAddress(uint32_t) { return {}; }
};

class ENetTransport final : public ITransport {
public:
    ~ENetTransport() override { Stop(); }
    bool StartHost(uint16_t port, size_t maxClients, Validator v);
    bool StartClient(const std::string& host, uint16_t port, Validator v);
    void Stop() override;
    void Send(uint32_t peer, uint8_t channel, std::vector<uint8_t> data, bool reliable) override;
    void Broadcast(uint8_t channel, std::vector<uint8_t> data, bool reliable) override;
    void Disconnect(uint32_t peer) override;
    bool Poll(NetEvent& out) override;
    uint32_t RoundTripMs(uint32_t peer) override;
    std::string PeerAddress(uint32_t peer) override;
    bool running() const { return running_; }

private:
    struct Outgoing { uint32_t peer; bool broadcast; bool disconnect; uint8_t channel; bool reliable; std::vector<uint8_t> data; };
    void Loop();
    bool StartCommon(bool host, const std::string& addr, uint16_t port, size_t peers, Validator v);

    void* host_ = nullptr;                 // ENetHost*
    std::vector<void*> peers_;             // index = peer id, ENetPeer* (network thread only)
    std::thread thread_;
    std::atomic<bool> running_{false};
    Validator validate_;
    std::mutex mu_;
    std::deque<NetEvent> inbox_;
    std::deque<Outgoing> outbox_;
    std::vector<uint32_t> rtt_;
    std::vector<std::string> addr_;        // index = peer id, the address it connected from (under mu_)
    bool isHost_ = false;
};

}  // namespace hsm

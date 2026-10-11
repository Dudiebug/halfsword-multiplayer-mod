#include "net/transport.h"
#include "log.h"
#include "hsm_protocol.h"
#include <enet/enet.h>

namespace hsm {
namespace {
std::once_flag g_enetInit;
bool g_enetOk = false;
}

bool ENetTransport::StartHost(uint16_t port, size_t maxClients, Validator v) { return StartCommon(true, "", port, maxClients, std::move(v)); }
bool ENetTransport::StartClient(const std::string& host, uint16_t port, Validator v) { return StartCommon(false, host, port, 1, std::move(v)); }

bool ENetTransport::StartCommon(bool host, const std::string& addr, uint16_t port, size_t peers, Validator v) {
    std::call_once(g_enetInit, [] { g_enetOk = enet_initialize() == 0; });
    if (!g_enetOk) { HSM_LOG("net: enet_initialize failed"); return false; }
    isHost_ = host; validate_ = std::move(v);
    ENetAddress a{};
    a.port = port;
    if (host) {
        a.host = ENET_HOST_ANY;
        host_ = enet_host_create(&a, peers, kChanCount, 0, 0);
    } else {
        host_ = enet_host_create(nullptr, 1, kChanCount, 0, 0);
        if (host_ && enet_address_set_host(&a, addr.c_str()) != 0) { HSM_LOG("net: cannot resolve %s", addr.c_str()); enet_host_destroy(static_cast<ENetHost*>(host_)); host_ = nullptr; return false; }
        if (host_ && !enet_host_connect(static_cast<ENetHost*>(host_), &a, kChanCount, kProtocolRevision)) { HSM_LOG("net: connect setup failed"); }
    }
    if (!host_) { HSM_LOG("net: enet_host_create failed (port %u)", port); return false; }
    peers_.assign(peers + 1, nullptr);
    rtt_.assign(peers + 1, 0);
    running_ = true;
    thread_ = std::thread([this] { Loop(); });
    HSM_LOG("net: %s on %s:%u", host ? "hosting" : "connecting", host ? "*" : addr.c_str(), port);
    return true;
}

void ENetTransport::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
    if (host_) { enet_host_flush(static_cast<ENetHost*>(host_)); enet_host_destroy(static_cast<ENetHost*>(host_)); host_ = nullptr; }
}

void ENetTransport::Send(uint32_t peer, uint8_t channel, std::vector<uint8_t> data, bool reliable) {
    std::lock_guard<std::mutex> lock(mu_);
    outbox_.push_back({peer, false, false, channel, reliable, std::move(data)});
}
void ENetTransport::Broadcast(uint8_t channel, std::vector<uint8_t> data, bool reliable) {
    std::lock_guard<std::mutex> lock(mu_);
    outbox_.push_back({0, true, false, channel, reliable, std::move(data)});
}
void ENetTransport::Disconnect(uint32_t peer) {
    std::lock_guard<std::mutex> lock(mu_);
    outbox_.push_back({peer, false, true, 0, true, {}});
}
bool ENetTransport::Poll(NetEvent& out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (inbox_.empty()) return false;
    out = std::move(inbox_.front()); inbox_.pop_front(); return true;
}
uint32_t ENetTransport::RoundTripMs(uint32_t peer) {
    std::lock_guard<std::mutex> lock(mu_);
    return peer < rtt_.size() ? rtt_[peer] : 0;
}

void ENetTransport::Loop() {
    auto* h = static_cast<ENetHost*>(host_);
    while (running_) {
        std::deque<Outgoing> out;
        { std::lock_guard<std::mutex> lock(mu_); out.swap(outbox_); }
        for (Outgoing& o : out) {
            ENetPeer* p = !o.broadcast && o.peer < peers_.size() ? static_cast<ENetPeer*>(peers_[o.peer]) : nullptr;
            if (o.disconnect) { if (p) enet_peer_disconnect(p, 0); continue; }
            uint32_t flags = o.reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
            ENetPacket* pkt = enet_packet_create(o.data.data(), o.data.size(), flags);
            if (o.broadcast) enet_host_broadcast(h, o.channel, pkt);
            else if (p) { if (enet_peer_send(p, o.channel, pkt) != 0) enet_packet_destroy(pkt); }   // not connected: ENet keeps no reference
            else enet_packet_destroy(pkt);
        }
        ENetEvent ev;
        while (running_ && enet_host_service(h, &ev, 2) > 0) {
            NetEvent ne;
            if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                uint32_t id = 0;
                for (uint32_t i = 1; i < peers_.size(); ++i) if (!peers_[i]) { id = i; break; }
                if (!id) { enet_peer_disconnect_now(ev.peer, 0); continue; }
                peers_[id] = ev.peer; ev.peer->data = reinterpret_cast<void*>(uintptr_t(id));
                ne.kind = NetEvent::Connected; ne.peer = id;
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                uint32_t id = uint32_t(reinterpret_cast<uintptr_t>(ev.peer->data));
                if (id && id < peers_.size()) peers_[id] = nullptr;
                ne.kind = NetEvent::Disconnected; ne.peer = id;
            } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                uint32_t id = uint32_t(reinterpret_cast<uintptr_t>(ev.peer->data));
                bool ok = ev.packet->dataLength <= kMaxPacket && validate_ && validate_(ev.packet->data, ev.packet->dataLength, ev.channelID);
                if (ok) { ne.kind = NetEvent::Data; ne.peer = id; ne.channel = ev.channelID; ne.data.assign(ev.packet->data, ev.packet->data + ev.packet->dataLength); }
                enet_packet_destroy(ev.packet);
                if (!ok) continue;   // malformed packets never reach the game thread
            } else continue;
            std::lock_guard<std::mutex> lock(mu_);
            inbox_.push_back(std::move(ne));
        }
        std::lock_guard<std::mutex> lock(mu_);
        for (uint32_t i = 1; i < peers_.size(); ++i) if (peers_[i]) rtt_[i] = static_cast<ENetPeer*>(peers_[i])->roundTripTime;
    }
}

}  // namespace hsm

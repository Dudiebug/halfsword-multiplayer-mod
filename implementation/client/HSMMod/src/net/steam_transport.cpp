#include "net/steam_transport.h"
#include "hsm_protocol.h"
#include "log.h"
#include "steam/steam_api.h"

namespace hsm {

uint32_t SteamTransport::AllowPeer(uint64_t steamId) {
    if (!steamId) return 0;
    auto it = byId_.find(steamId);
    if (it != byId_.end()) return it->second;
    uint32_t peer = nextPeer_++;
    byId_[steamId] = peer; bySlot_[peer] = steamId;
    steam::AcceptSession(steamId);
    NetEvent e; e.kind = NetEvent::Connected; e.peer = peer; e.steamId = steamId;
    queue_.push_back(std::move(e));
    HSM_LOG("steam-net: peer %u = %llu (%s)", peer, (unsigned long long)steamId, steam::PersonaName(steamId).c_str());
    return peer;
}

void SteamTransport::DropPeer(uint64_t steamId) {
    auto it = byId_.find(steamId);
    if (it == byId_.end()) return;
    NetEvent e; e.kind = NetEvent::Disconnected; e.peer = it->second; e.steamId = steamId;
    bySlot_.erase(it->second); byId_.erase(it);
    steam::CloseSession(steamId);
    queue_.push_back(std::move(e));
}

uint32_t SteamTransport::PeerOf(uint64_t steamId) const { auto it = byId_.find(steamId); return it == byId_.end() ? 0 : it->second; }
uint64_t SteamTransport::SteamOf(uint32_t peer) const { auto it = bySlot_.find(peer); return it == bySlot_.end() ? 0 : it->second; }

void SteamTransport::Stop() {
    for (auto& [id, peer] : byId_) steam::CloseSession(id);
    byId_.clear(); bySlot_.clear(); queue_.clear(); head_ = 0;
}

void SteamTransport::Send(uint32_t peer, uint8_t channel, std::vector<uint8_t> data, bool reliable) {
    uint64_t id = SteamOf(peer);
    if (!id) return;
    int flags = steam::kAutoRestart | (reliable ? steam::kReliable : steam::kNoNagle);
    if (!steam::Send(id, channel, data.data(), uint32_t(data.size()), flags) && !sendFailLogged_) {
        sendFailLogged_ = true;
        HSM_LOG("steam-net: SendMessageToUser to %llu failed (further failures not logged)", (unsigned long long)id);
    }
}

void SteamTransport::Broadcast(uint8_t channel, std::vector<uint8_t> data, bool reliable) {
    for (auto& [peer, id] : bySlot_) Send(peer, channel, data, reliable);
}

void SteamTransport::Disconnect(uint32_t peer) { if (uint64_t id = SteamOf(peer)) DropPeer(id); }

void SteamTransport::Pump() {
    std::vector<steam::Message> msgs;
    for (int ch = 0; ch < kChanCount; ++ch) steam::Receive(ch, msgs);
    for (steam::Message& m : msgs) {
        uint32_t peer = PeerOf(m.from);
        if (!peer) continue;                                                       // not a lobby member we accepted
        if (m.data.size() > kMaxPacket || !validate_ || !validate_(m.data.data(), m.data.size(), uint8_t(m.channel))) continue;
        NetEvent e; e.kind = NetEvent::Data; e.peer = peer; e.steamId = m.from; e.channel = uint8_t(m.channel); e.data = std::move(m.data);
        queue_.push_back(std::move(e));
    }
}

bool SteamTransport::Poll(NetEvent& out) {
    if (head_ >= queue_.size()) { queue_.clear(); head_ = 0; Pump(); }
    if (head_ >= queue_.size()) return false;
    out = std::move(queue_[head_++]);
    return true;
}

uint32_t SteamTransport::RoundTripMs(uint32_t peer) {
    uint64_t id = SteamOf(peer);
    int ping = id ? steam::PingMs(id) : -1;
    return ping > 0 ? uint32_t(ping) : 0;
}

}  // namespace hsm

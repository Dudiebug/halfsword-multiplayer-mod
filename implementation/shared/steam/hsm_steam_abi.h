// Opaque Windows x64 layouts checked against Valve Steamworks SDK 1.57.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hsm::steam::abi {
#pragma pack(push, 8)
struct NetIdentity { int32_t type; int32_t size; union { uint64_t steamId; char raw[128]; }; };
struct NetMessage {
    void* data; int32_t size; uint32_t conn; NetIdentity peer; int64_t connUserData; int64_t usecReceived;
    int64_t messageNumber; void* freeData; void* release; int32_t channel; int32_t flags; int64_t userData; uint16_t lane; uint16_t pad;
};
struct NetConnectionInfo {
    NetIdentity remote; int64_t userData; uint32_t listenSocket; uint8_t remoteAddress[18]; uint16_t pad;
    uint32_t remotePop, relayPop; int32_t state, endReason;
    char endDebug[128], description[128]; int32_t flags; uint32_t reserved[63];
};
struct NetStatus {
    int32_t state, ping; float qualityLocal, qualityRemote, outPackets, outBytes, inPackets, inBytes;
    int32_t sendRate, pendingUnreliable, pendingReliable, unackedReliable; int64_t queueTime; uint32_t reserved[16];
};
struct LobbyCreated { int32_t result; uint64_t lobby; };
struct LobbyMatchList { uint32_t count; };
struct LobbyEnter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };
struct LobbyDataUpdate { uint64_t lobby; uint64_t member; uint8_t success; };
struct LobbyChatUpdate { uint64_t lobby; uint64_t changed; uint64_t changer; uint32_t state; };
struct LobbyChatMsg { uint64_t lobby; uint64_t user; uint8_t type; uint32_t chatId; };
struct GameLobbyJoinRequested { uint64_t lobby; uint64_t friendId; };
struct RichJoinRequested { uint64_t friendId; char connect[256]; };
struct SessionRequest { NetIdentity remote; };
struct SessionFailed { NetConnectionInfo info; };
struct RelayStatus { int32_t available, measuring, networkConfig, anyRelay; char debug[256]; };
struct ServerConnectFailure { int32_t result; bool retrying; };
struct ServersDisconnected { int32_t result; };
#pragma pack(pop)
static_assert(sizeof(NetIdentity) == 136 && sizeof(NetMessage) == 216);
static_assert(offsetof(NetMessage, peer) == 16 && offsetof(NetMessage, usecReceived) == 160 && offsetof(NetMessage, channel) == 192);
static_assert(sizeof(NetConnectionInfo) == 696 && offsetof(NetConnectionInfo, state) == 176 && offsetof(NetConnectionInfo, endReason) == 180);
static_assert(sizeof(NetStatus) == 120 && offsetof(NetStatus, ping) == 4);
static_assert(sizeof(LobbyCreated) == 16 && sizeof(LobbyMatchList) == 4 && sizeof(LobbyEnter) == 24);
static_assert(sizeof(LobbyDataUpdate) == 24 && sizeof(LobbyChatUpdate) == 32 && sizeof(LobbyChatMsg) == 24);
static_assert(sizeof(GameLobbyJoinRequested) == 16 && sizeof(RichJoinRequested) == 264);
static_assert(sizeof(SessionRequest) == 136 && sizeof(SessionFailed) == 696 && sizeof(RelayStatus) == 272);
}  // namespace hsm::steam::abi

// Steamworks SDK 1.57 struct layouts on Windows x64 (pack 8), as opaque mirrors for the flat API.
// From CSLewin's PR #1 (MIT), checked against the SDK headers; the static_asserts pin every size the mod relies on.
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
struct NetConnectionInfo {   // SteamNetConnectionInfo_t
    NetIdentity remote; int64_t userData; uint32_t listenSocket; uint8_t remoteAddress[18]; uint16_t pad;
    uint32_t remotePop, relayPop; int32_t state, endReason;
    char endDebug[128], description[128]; int32_t flags; uint32_t reserved[63];
};
struct NetStatus {           // SteamNetConnectionRealTimeStatus_t
    int32_t state, ping; float qualityLocal, qualityRemote, outPackets, outBytes, inPackets, inBytes;
    int32_t sendRate, pendingUnreliable, pendingReliable, unackedReliable; int64_t queueTime; uint32_t reserved[16];
};
struct LobbyCreated { int32_t result; uint64_t lobby; };                                       // 513 (call result)
struct LobbyMatchList { uint32_t count; };                                                    // 510 (call result)
struct LobbyEnter { uint64_t lobby; uint32_t permissions; bool locked; uint32_t response; };  // 504
struct LobbyDataUpdate { uint64_t lobby; uint64_t member; uint8_t success; };                 // 505
struct LobbyChatUpdate { uint64_t lobby; uint64_t changed; uint64_t changer; uint32_t state; };  // 506
struct LobbyChatMsg { uint64_t lobby; uint64_t user; uint8_t type; uint32_t chatId; };        // 507
struct GameLobbyJoinRequested { uint64_t lobby; uint64_t friendId; };                        // 333
struct RichJoinRequested { uint64_t friendId; char connect[256]; };                           // 337
struct SessionRequest { NetIdentity remote; };                                                // 1251
struct SessionFailed { NetConnectionInfo info; };                                             // 1252
struct RelayStatus { int32_t available, measuring, networkConfig, anyRelay; char debug[256]; };  // 1281
struct ServerConnectFailure { int32_t result; bool retrying; };                               // 102
struct ServersDisconnected { int32_t result; };                                               // 103
#pragma pack(pop)
static_assert(sizeof(NetIdentity) == 136 && sizeof(NetMessage) == 216, "SteamNetworkingIdentity / SteamNetworkingMessage_t");
static_assert(offsetof(NetMessage, peer) == 16 && offsetof(NetMessage, usecReceived) == 160 && offsetof(NetMessage, channel) == 192,
              "SteamNetworkingMessage_t");
static_assert(sizeof(NetConnectionInfo) == 696 && offsetof(NetConnectionInfo, state) == 176 && offsetof(NetConnectionInfo, endReason) == 180,
              "SteamNetConnectionInfo_t");
static_assert(sizeof(NetStatus) == 120 && offsetof(NetStatus, ping) == 4, "SteamNetConnectionRealTimeStatus_t");
static_assert(sizeof(LobbyCreated) == 16 && sizeof(LobbyMatchList) == 4 && sizeof(LobbyEnter) == 24, "lobby call results");
static_assert(sizeof(LobbyDataUpdate) == 24 && sizeof(LobbyChatUpdate) == 32 && sizeof(LobbyChatMsg) == 24, "lobby callbacks");
static_assert(sizeof(GameLobbyJoinRequested) == 16 && sizeof(RichJoinRequested) == 264, "join callbacks");
static_assert(sizeof(SessionRequest) == 136 && sizeof(SessionFailed) == 696 && sizeof(RelayStatus) == 272, "networking callbacks");
}  // namespace hsm::steam::abi

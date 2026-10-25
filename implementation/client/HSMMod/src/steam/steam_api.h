// Steamworks access through the game's own steam_api64.dll (SDK 1.57 flat API), resolved at runtime.
// The game already calls SteamAPI_Init; we only look up interfaces, register callbacks and poll call results.
// Callbacks may fire on any thread; they are copied into a queue that the game thread drains with Poll().
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hsm::steam {

using SteamId = uint64_t;
using ApiCall = uint64_t;

enum class LobbyType : int { Private = 0, FriendsOnly = 1, Public = 2, Invisible = 3 };

struct Event {
    enum Kind {
        LobbyCreated,         // ok, lobby
        LobbyEntered,         // ok, lobby
        LobbyMembersChanged,  // lobby, user, a = state change bits (1 entered, 2 left, 4 disconnected, 8 kicked, 16 banned)
        LobbyDataChanged,     // lobby, user (== lobby for lobby-wide data)
        LobbyChat,            // lobby, user, text
        JoinRequested,        // lobby (friend clicked Join / accepted invite)
        LobbyList,            // lobbies
        SessionRequest,       // user wants a networking session
        SessionFailed,        // user
    } kind;
    bool ok = true;
    SteamId lobby = 0, user = 0;
    uint32_t a = 0;
    std::string text;
    std::vector<SteamId> lobbies;
};

struct Message { SteamId from = 0; int channel = 0; std::vector<uint8_t> data; double ageMs = -1; };   // ageMs: since Steam received it (-1 unknown)

enum SendFlags : int { kUnreliable = 0, kNoNagle = 1, kReliable = 8, kAutoRestart = 32 };

// Lifecycle (game thread). Init() returns false until steam_api64.dll is loaded and Steam is initialised.
bool Init();
bool Ready();
void Tick();                      // polls call results; pumps callbacks if the game does not
bool Poll(Event& out);            // drain queued callbacks/results

SteamId MySteamId();
std::string PersonaName(SteamId user);   // empty for unknown
uint32_t AppId();
std::string LaunchConnectLobby();        // "+connect_lobby <id>" from the command line, if any

// Lobbies (results arrive as Events)
void CreateLobby(LobbyType type, int maxMembers);
void JoinLobby(SteamId lobby);
void LeaveLobby(SteamId lobby);
bool SetLobbyType(SteamId lobby, LobbyType type);
bool SetLobbyJoinable(SteamId lobby, bool joinable);
bool SetLobbyData(SteamId lobby, const std::string& key, const std::string& value);
std::string LobbyData(SteamId lobby, const std::string& key);
void SetMemberData(SteamId lobby, const std::string& key, const std::string& value);
std::string MemberData(SteamId lobby, SteamId user, const std::string& key);
std::vector<SteamId> LobbyMembers(SteamId lobby);
SteamId LobbyOwner(SteamId lobby);
bool SendLobbyChat(SteamId lobby, const std::string& text);
void RequestLobbyList(const std::string& filterKey, const std::string& filterValue);
void InviteOverlay(SteamId lobby);
void SetRichPresence(const std::string& key, const std::string& value);

// Avatars (optional exports). MediumAvatar: the 64 x 64 avatar's image handle; 0 none (no avatar, or no export), -1
// Steam is still fetching it (ask again later). AvatarRGBA: its pixels, top row first, four bytes each (R G B A).
int MediumAvatar(SteamId user);
bool AvatarRGBA(int image, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);

// Networking (ISteamNetworkingMessages; relayed, NAT-traversing)
void InitRelay();
bool Send(SteamId to, int channel, const void* data, uint32_t size, int flags);
size_t Receive(int channel, std::vector<Message>& out, int max = 64);
void AcceptSession(SteamId user);
void CloseSession(SteamId user);
int PingMs(SteamId user);                // -1 if unknown

}  // namespace hsm::steam

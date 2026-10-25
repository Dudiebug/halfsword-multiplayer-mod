// Steam lobby lifecycle for the in-game Multiplayer screen. The lobby is the source of truth for who is in the
// session; the session layer mirrors membership into the transport. Game thread only.
#pragma once
#include "match/hsm_match.h"
#include "steam/steam_api.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm::game {

constexpr int kMaxLobbyMembers = 6;
constexpr const char* kModVersion = "0.6.0";   // bump with every release tag

struct LobbyMember { steam::SteamId id = 0; std::string name; bool ready = false; int team = -1; int hostOverride = -1; bool host = false; };

struct LobbyListing {
    steam::SteamId id = 0;
    std::string name, map, mode;
    int players = 0;
    bool compatible = false;
    std::string reason;   // why it is not joinable, if !compatible
};

class Lobby {
public:
    enum class State { Offline, Idle, Creating, Joining, InLobby };

    // Membership callbacks for the session (fired on the game thread).
    std::function<void(steam::SteamId)> onMemberJoined, onMemberLeft;
    std::function<void()> onEntered, onLeft;

    void Tick();                                   // drives Steam init + callback processing

    void Host(steam::LobbyType type);
    void Join(steam::SteamId lobby);
    void Leave();
    void Invite();
    void SetType(steam::LobbyType type);
    void SetReady(bool ready);
    void SetPublicState(const std::string& state, const std::string& map, const std::string& mode);
    void RefreshList();
    // Match settings (lobby data, host writes) and teams (own pick in member data, host moves in lobby data).
    MatchConfig config() const;
    void SetConfig(const MatchConfig& c);
    std::string MapPick() const;                    // host's chosen level (package path), empty until picked
    void SetMapPick(const std::string& path);
    void SetTeamPick(int team);
    void SetOverride(steam::SteamId member, int team);
    void ClearOverrides();

    State state() const { return state_; }
    bool IsHost() const { return state_ == State::InLobby && owner_ == steam::MySteamId(); }
    bool InLobby() const { return state_ == State::InLobby; }
    steam::SteamId id() const { return lobby_; }
    steam::SteamId owner() const { return owner_; }
    steam::LobbyType type() const { return type_; }
    const std::vector<LobbyMember>& members() const { return members_; }
    const std::vector<LobbyListing>& listings() const { return listings_; }
    const std::string& status() const { return status_; }

private:
    void Enter(steam::SteamId lobby);
    void RefreshMembers();
    void SetStatus(const std::string& s);
    LobbyListing Describe(steam::SteamId lobby) const;
    State state_ = State::Offline;
    steam::SteamId lobby_ = 0, owner_ = 0;
    steam::LobbyType type_ = steam::LobbyType::FriendsOnly;
    std::vector<LobbyMember> members_;
    std::vector<LobbyListing> listings_;
    std::string status_ = "Steam not ready";
    bool launchChecked_ = false;
};

}  // namespace hsm::game

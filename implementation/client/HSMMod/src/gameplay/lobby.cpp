#include "gameplay/lobby.h"
#include "hsm_protocol.h"
#include "log.h"
#include "session/hsm_build.h"
#include "ue/engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace hsm::game {
namespace {
// Parses a decimal int from Steam key/value data; anything else (empty, garbage, overflow) gives the fallback.
int IntOr(const std::string& s, int fallback) {
    if (s.empty() || s.size() > 6) return fallback;
    char* end = nullptr; long v = std::strtol(s.c_str(), &end, 10);
    return end && *end == 0 ? int(v) : fallback;
}
}  // namespace
#ifndef HSM_GIT_DESCRIBE
#define HSM_GIT_DESCRIBE ""
#endif
std::string ModBuild() { return ModBuildString(kModVersion, HSM_GIT_DESCRIBE); }
namespace {
bool g_allowBuildMismatch = false;
}  // namespace
void SetAllowBuildMismatch(bool on) { g_allowBuildMismatch = on; }
bool AllowBuildMismatch() { return g_allowBuildMismatch; }

namespace {
std::string BuildKeyHex() { char b[32]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)ue::BuildKey()); return b; }
std::string ProtocolStr() { return std::to_string(int(kProtocolRevision)); }
}  // namespace

void Lobby::SetStatus(const std::string& s) {
    if (s == status_) return;
    status_ = s; HSM_LOG("lobby: %s", s.c_str());
}

void Lobby::Tick() {
    if (state_ == State::Offline) {
        if (!steam::Init()) return;
        steam::InitRelay();
        state_ = State::Idle;
        SetStatus("Not in a lobby");
    }
    steam::Tick();
    if (!launchChecked_) {          // launched through a friend's "Join Game"
        launchChecked_ = true;
        std::string id = steam::LaunchConnectLobby();
        if (!id.empty()) Join(std::strtoull(id.c_str(), nullptr, 10));
    }
    steam::Event e;
    while (steam::Poll(e)) {
        switch (e.kind) {
            case steam::Event::LobbyCreated:
                if (!e.ok) { state_ = State::Idle; SetStatus("Could not create lobby"); break; }
                steam::SetLobbyData(e.lobby, "hsm_proto", ProtocolStr());
                steam::SetLobbyData(e.lobby, "hsm_build", BuildKeyHex());
                steam::SetLobbyData(e.lobby, "hsm_ver", kModVersion);
                steam::SetLobbyData(e.lobby, "name", steam::PersonaName(steam::MySteamId()) + "'s fight");
                steam::SetLobbyData(e.lobby, "state", "menu");
                Enter(e.lobby);
                break;
            case steam::Event::LobbyEntered:
                if (state_ == State::Creating) break;    // our own new lobby: entered before LobbyCreated sets its data
                if (state_ == State::InLobby && lobby_ == e.lobby) break;
                // An enter for a lobby we are no longer joining (a late answer after Leave or a second invite) would
                // otherwise drop us into it, or reset a join in progress. Fix from CSLewin's PR #1.
                if (state_ != State::Joining || e.lobby != joining_) {
                    if (e.ok) steam::LeaveLobby(e.lobby);
                    HSM_LOG("lobby: ignored stale enter lobby=%llu", (unsigned long long)e.lobby);
                    break;
                }
                joining_ = 0;
                if (!e.ok) { state_ = State::Idle; SetStatus("Could not join lobby (full or closed)"); break; }
                if (state_ != State::InLobby || lobby_ != e.lobby) {
                    LobbyListing l = Describe(e.lobby);
                    if (!l.compatible) { steam::LeaveLobby(e.lobby); state_ = State::Idle; SetStatus("Cannot join: " + l.reason); break; }
                    Enter(e.lobby);
                }
                break;
            case steam::Event::LobbyMembersChanged:
            case steam::Event::LobbyDataChanged:
                if (e.lobby == lobby_ && state_ == State::InLobby) RefreshMembers();
                break;
            case steam::Event::JoinRequested:
                if (e.lobby && e.lobby != lobby_) { Leave(); Join(e.lobby); }   // Leave is a no-op unless in or joining a lobby
                break;
            case steam::Event::LobbyList: {
                listings_.clear();
                for (steam::SteamId id : e.lobbies) listings_.push_back(Describe(id));
                std::stable_sort(listings_.begin(), listings_.end(), [](const LobbyListing& a, const LobbyListing& b) { return a.compatible > b.compatible; });
                SetStatus(e.ok ? std::to_string(listings_.size()) + " public lobbies found" : "Could not list lobbies");
                break;
            }
            case steam::Event::SessionRequest: {   // a (re)opened session from a lobby member must be accepted again
                bool member = false;
                if (state_ == State::InLobby && e.user) for (steam::SteamId id : steam::LobbyMembers(lobby_)) member = member || id == e.user;
                if (member) steam::AcceptSession(e.user);
                HSM_LOG("lobby: networking session request from %s %s", steam::PersonaName(e.user).c_str(), member ? "accepted" : "ignored (not in our lobby)");
                break;
            }
            case steam::Event::SessionFailed:
                HSM_LOG("lobby: networking session with %s failed (reason %u: %s)", steam::PersonaName(e.user).c_str(), e.a, e.text.c_str());
                break;
            case steam::Event::LobbyChat:
                if (e.lobby == lobby_ && state_ == State::InLobby && onChat) onChat(e.user, e.text);
                break;
        }
    }
}

LobbyListing Lobby::Describe(steam::SteamId lobby) const {
    LobbyListing l; l.id = lobby;
    l.name = steam::LobbyData(lobby, "name"); l.map = steam::LobbyData(lobby, "map"); l.mode = steam::LobbyData(lobby, "mode");
    const std::vector<steam::SteamId> members = steam::LobbyMembers(lobby);
    l.players = int(members.size());
    std::string proto = steam::LobbyData(lobby, "hsm_proto"), build = steam::LobbyData(lobby, "hsm_build");
    if (proto.empty()) l.reason = "not a HalfSwordMP lobby";
    else if (proto != ProtocolStr()) l.reason = "different mod version (host " + steam::LobbyData(lobby, "hsm_ver") + ", you " + kModVersion + ")";
    else if (build != BuildKeyHex()) l.reason = "different game version";
    else if (l.players >= kMaxLobbyMembers && std::find(members.begin(), members.end(), steam::MySteamId()) == members.end())
        l.reason = "lobby full";   // a full lobby we are already in is not full to us
    l.compatible = l.reason.empty();
    return l;
}

void Lobby::Host(steam::LobbyType type) {
    if (state_ != State::Idle) return;
    type_ = type; state_ = State::Creating; SetStatus("Creating lobby...");
    steam::CreateLobby(type, kMaxLobbyMembers);
}

void Lobby::Join(steam::SteamId lobby) {
    if (state_ != State::Idle || !lobby) return;
    joining_ = lobby; state_ = State::Joining; SetStatus("Joining lobby...");
    steam::JoinLobby(lobby);
}

void Lobby::Enter(steam::SteamId lobby) {
    lobby_ = lobby; state_ = State::InLobby; owner_ = steam::LobbyOwner(lobby);
    steam::SetMemberData(lobby_, "hsm_ver", kModVersion);
    steam::SetMemberData(lobby_, "hsm_mod", ModBuild());   // the lobby names every other player on another build
    steam::SetRichPresence("connect", "+connect_lobby " + std::to_string(lobby_));
    steam::SetRichPresence("status", IsHost() ? "Hosting a Half Sword fight" : "In a Half Sword lobby");
    members_.clear();
    RefreshMembers();
    SetStatus(IsHost() ? "Hosting - invite friends, then start any fight" : "Waiting for host to start a fight...");
    if (onEntered) onEntered();
}

void Lobby::Leave() {
    if (state_ != State::InLobby && state_ != State::Joining) return;
    steam::LeaveLobby(state_ == State::Joining ? joining_ : lobby_);
    for (const LobbyMember& m : members_) if (m.id != steam::MySteamId() && onMemberLeft) onMemberLeft(m.id);
    members_.clear(); lobby_ = owner_ = joining_ = 0; state_ = State::Idle;
    steam::SetRichPresence("connect", ""); steam::SetRichPresence("status", "");
    SetStatus("Left lobby");
    if (onLeft) onLeft();
}

void Lobby::Invite() { if (InLobby()) steam::InviteOverlay(lobby_); }
bool Lobby::SendChat(const std::string& text) { return InLobby() && !text.empty() && steam::SendLobbyChat(lobby_, text); }
void Lobby::SetType(steam::LobbyType type) { if (IsHost() && steam::SetLobbyType(lobby_, type)) type_ = type; }
MatchConfig Lobby::config() const {
    MatchConfig c;
    if (!InLobby()) return c;
    c.format = Format(IntOr(steam::LobbyData(lobby_, "hsm_fmt"), 0));
    c.scoreLimit = uint16_t(IntOr(steam::LobbyData(lobby_, "hsm_lim"), DefaultScoreLimit(c.format)));
    c.timeLimitS = uint16_t(IntOr(steam::LobbyData(lobby_, "hsm_time"), 0));
    c.respawnDelayS = uint16_t(IntOr(steam::LobbyData(lobby_, "hsm_resp"), 5));
    c.friendlyFire = steam::LobbyData(lobby_, "hsm_ff") == "1";
    c.gear = Gear(IntOr(steam::LobbyData(lobby_, "hsm_gear"), 0));
    c.tier = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_tier"), 2), 0, 7));
    return Sanitize(c);
}

void Lobby::SetConfig(const MatchConfig& in) {
    if (!IsHost()) return;
    MatchConfig c = Sanitize(in);
    steam::SetLobbyData(lobby_, "hsm_fmt", std::to_string(int(c.format)));
    steam::SetLobbyData(lobby_, "hsm_lim", std::to_string(c.scoreLimit));
    steam::SetLobbyData(lobby_, "hsm_time", std::to_string(c.timeLimitS));
    steam::SetLobbyData(lobby_, "hsm_resp", std::to_string(c.respawnDelayS));
    steam::SetLobbyData(lobby_, "hsm_ff", c.friendlyFire ? "1" : "0");
    steam::SetLobbyData(lobby_, "hsm_gear", std::to_string(int(c.gear)));
    steam::SetLobbyData(lobby_, "hsm_tier", std::to_string(int(c.tier)));
}
std::string Lobby::MapPick() const { return InLobby() ? steam::LobbyData(lobby_, "hsm_map") : std::string(); }
void Lobby::SetMapPick(const std::string& path) { if (IsHost()) steam::SetLobbyData(lobby_, "hsm_map", path); }
bool Lobby::Progression() const { return InLobby() && steam::LobbyData(lobby_, "hsm_prog") == "1"; }
void Lobby::SetProgression(bool on) { if (IsHost()) steam::SetLobbyData(lobby_, "hsm_prog", on ? "1" : "0"); }

void Lobby::SetTeamPick(int team) { if (InLobby()) steam::SetMemberData(lobby_, "hsm_team", std::to_string(team)); }

void Lobby::SetOverride(steam::SteamId member, int team) {
    if (IsHost()) steam::SetLobbyData(lobby_, "hsm_ovr_" + std::to_string(member), std::to_string(team));
}

void Lobby::ClearOverrides() { for (const LobbyMember& m : members_) SetOverride(m.id, -1); }

void Lobby::SetReady(bool ready) { if (InLobby()) steam::SetMemberData(lobby_, "ready", ready ? "1" : "0"); }

void Lobby::SetPublicState(const std::string& state, const std::string& map, const std::string& mode) {
    if (!IsHost()) return;
    steam::SetLobbyData(lobby_, "state", state); steam::SetLobbyData(lobby_, "map", map); steam::SetLobbyData(lobby_, "mode", mode);
}

void Lobby::RefreshList() {
    if (state_ == State::Offline) return;
    SetStatus("Searching lobbies...");
    steam::RequestLobbyList("hsm_proto", ProtocolStr());
}

void Lobby::RefreshMembers() {
    steam::SteamId owner = steam::LobbyOwner(lobby_);
    if (owner && owner != owner_) {           // host left and Steam promoted someone: no host migration
        owner_ = owner;
        SetStatus("Host left the lobby");
        Leave();
        return;
    }
    std::vector<steam::SteamId> ids = steam::LobbyMembers(lobby_);
    const bool synced = !members_.empty();   // the first refresh after Enter lists whoever was here: not "joined" lines
    for (const LobbyMember& m : members_) {
        if (std::find(ids.begin(), ids.end(), m.id) != ids.end() || m.id == steam::MySteamId()) continue;
        if (onMemberLeft) onMemberLeft(m.id);
        if (onPresence) onPresence(m.name, false);
    }
    std::vector<LobbyMember> next;
    for (steam::SteamId id : ids) {
        if (!id) continue;
        LobbyMember m; m.id = id; m.name = steam::PersonaName(id); m.host = id == owner_;
        m.ready = steam::MemberData(lobby_, id, "ready") == "1";
        m.build = steam::MemberData(lobby_, id, "hsm_mod").substr(0, kMaxModBuild);   // peer-written
        auto team = [](int t) { return t >= -1 && t <= 3 ? t : -1; };   // peer-written; later narrowed to int8_t
        m.team = team(IntOr(steam::MemberData(lobby_, id, "hsm_team"), -1));
        m.hostOverride = team(IntOr(steam::LobbyData(lobby_, "hsm_ovr_" + std::to_string(id)), -1));
        bool known = std::any_of(members_.begin(), members_.end(), [&](const LobbyMember& o) { return o.id == id; });
        if (!known && id && id != steam::MySteamId()) {
            if (onMemberJoined) onMemberJoined(id);
            if (synced && onPresence) onPresence(m.name, true);
        }
        next.push_back(m);
    }
    members_ = std::move(next);
}

}  // namespace hsm::game

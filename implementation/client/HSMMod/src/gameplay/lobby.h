// Steam lobby lifecycle for the in-game Multiplayer screen. The lobby is the source of truth for who is in the
// session; the session layer mirrors membership into the transport. Game thread only.
#pragma once
#include "match/hsm_online_rules.h"
#include "match/hsm_match.h"
#include "session/hsm_mod_rules.h"
#include "session/hsm_mod_switch.h"
#include "session/hsm_modpack.h"
#include "steam/steam_api.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm::game {

constexpr int kMaxLobbyMembers = 20;  // players: == kMaxPlayers (session/hsm_roster.h; static_assert in session.cpp).
                                      // A role=server lobby has one member more: the server, which is not a player.
#ifdef HSM_TESTBENCH
constexpr const char* kModVersion = "0.8.0-pre50-test";
#else
constexpr const char* kModVersion = "0.8.0-pre50";   // bump with every release tag
#endif
// This mod's build: kModVersion, the git describe build.sh passed and the DLL file's hash (session/hsm_build.h). Players
// compare it.
std::string ModBuild();
// allow_build_mismatch in HSMMod.ini (config.h): the host may start a match with other builds in. Set once at start.
void SetAllowBuildMismatch(bool on);
// direct_p2p in HSMMod.ini (rev 42): Steam may share this machine's addresses with lobby members for a direct path. Set
// once at start, before Steam networking starts.
void SetDirectP2P(bool on);
bool AllowBuildMismatch();

struct LobbyMember { steam::SteamId id = 0; std::string card; std::string name; std::string steamName; bool ready = false; int team = -1; int hostOverride = -1; bool host = false; std::string build; };

struct LobbyListing {
    steam::SteamId id = 0;
    std::string name, map, mode;
    Region region = Region::Auto;   // the host's region (hsm_region); Auto: it did not say
    int players = 0;      // players: a role=server lobby's server is not counted
    int maxPlayers = 8;   // the host's player limit (hsm_max); 8 (kDefaultPlayerLimit) when the lobby has none
    bool locked = false;  // asks for a password (hsm_lock); the password is never listed
    LobbyPack pack;       // the modpack the host announced (hsm_pack/hsm_packv/hsm_packh); empty: none. Checked live, not here
    bool banned = false;  // the owner is on the global ban list (session/hsm_global_bans.h): left out of the list
    bool compatible = false;
    std::string reason;   // why it is not joinable, if !compatible
    std::string mods;     // the lobby's mod rule tag ("mods: Maps only"), empty for the default (session/hsm_mod_rules.h LobbyRuleTag)
    std::string modsText; // the mod rules' full sentence for this machine (what to remove), when they turn it away
    std::string ruleText; // the lobby's rule in words (RuleSummary), for the switch-off question
    SwitchPlan plan;      // what this machine would turn off for a restart to fit the rule (session/hsm_mod_switch.h); empty when it fits, or was tried
    BackPlan back;        // mods off for another lobby that this one takes: what a restart brings back (PlanBack); empty: nothing
    bool switchable = false;   // !compatible only because of the plan, which has no blockers: joinable after a restart
};

class Lobby {
public:
    enum class State { Offline, Idle, Creating, Joining, InLobby };

    // Membership callbacks for the session (fired on the game thread).
    std::function<void(steam::SteamId)> onMemberJoined, onMemberLeft;
    std::function<void()> onEntered, onLeft;
    // The host left and Steam made another member the owner. True: the session migrates to the new owner and the lobby
    // stays; false (or unset): leave the lobby, as before 0.7.0-pre5.
    std::function<bool(steam::SteamId oldOwner, steam::SteamId newOwner)> onOwnerChanged;
    std::function<void(steam::SteamId from, const std::string& text)> onChat;   // a lobby chat line (ours too); the text is unchecked
    bool BlockHse() const;                       // HS Enhancer is refused: this lobby's switch, from its host; this machine's choice outside one
    ModPreset hostPreset = ModChoice{}.preset;  // the rule of this machine's lobbies (mod_rule=)
    bool InClassesLobby() const;                 // the lobby this machine is in uses classes (mod_rule=classes or unrestricted)
    bool InUnrestrictedLobby() const;            // ...with no class power budget (mod_rule=unrestricted)
    std::function<std::string()> myRuleSheet;   // this machine's class settings (hsm_online_rules.h)
    std::function<std::string(ModPreset, bool blockHse, double powerLimit)> onlineBlock;   // why this machine may not play online in this kind of lobby; empty: it may
    std::function<void()> onEnterCard;   // a new lobby: the old lobby's cards go
    void RepublishMyCard();   // the card again, if it changed since it was sent
    std::function<std::string()> myCard;                                        // this machine's build card (hsm_build_card.h), asked for on entering a lobby
    std::function<void(const std::string& name, const std::string& card)> onBuildCard;   // a member's card arrived or changed; peer-written
    std::function<void(const std::string& name, bool joined)> onPresence;       // someone else came or went, after we entered

    void Tick();                                   // drives Steam init + callback processing

    void Host(steam::LobbyType type, bool server = false);   // server: role=server, one member more (hsm_srv=1)
    void Join(steam::SteamId lobby, const std::string& password = std::string());   // password: for a locked lobby (kept for this lobby only)
    void Leave();
    void Invite();
    bool SendChat(const std::string& text);        // to every member (Steam lobby chat); false: not in a lobby or Steam refused
    void SetType(steam::LobbyType type);
    void SetReady(bool ready);
    // The host's player limit, 2..20 (Steam's member limit and lobby data hsm_max). Never below the players already in:
    // nobody is kicked by it. Before the lobby exists it is what Host creates it with.
    int MaxPlayers() const;
    void SetMaxPlayers(int n);
    // Lobby password. Locked is the public flag (hsm_lock); the password itself is never in lobby data. The host sets it
    // (SetPassword, "" clears; nobody is kicked), a joiner has the one it joined with, and every member keeps the host's
    // latest (RememberPassword, from LobbySecret) so a new host after a migration keeps the lobby locked.
    bool Locked() const;
    const std::string& Password() const;            // this lobby's; empty: none known
    void SetPassword(const std::string& password);  // host
    void RememberPassword(const std::string& password);   // member: the host's latest
    // A locked lobby was joined without (or with a wrong) password: the page asks for it, then Join(lobby, password).
    void AskPassword(steam::SteamId lobby, const std::string& note = std::string());
    steam::SteamId PasswordAsk() const { return askLobby_; }
    const std::string& PasswordNote() const { return askNote_; }
    void CancelPasswordAsk() { askLobby_ = 0; askNote_.clear(); }
    // The switch-off at join (session/hsm_mod_switch.h): a lobby's rule refuses mods this machine can turn off. The page
    // asks once (SwitchAskPending), then ConfirmSwitch turns them off for the next game, writes the rejoin file and
    // restarts the game, which joins the lobby on its own. rejoin: the lobby we are in changed its rule under us.
    // back: the other way, asked in a lobby that takes the mods this game has off for another (PlanBack): the restart
    // turns them all back on and rejoins; NOT NOW stays, and that lobby doesn't ask again this game.
    struct SwitchAsk { steam::SteamId lobby = 0; std::string name, ruleText; SwitchPlan plan; bool rejoin = false; bool back = false; BackPlan backPlan; };
    const SwitchAsk& SwitchAskPending() const { return switchAsk_; }   // lobby == 0: nothing asked
    void CancelSwitchAsk() { if (switchAsk_.back) backDeclined_ = switchAsk_.lobby; switchAsk_ = SwitchAsk{}; }
    bool ConfirmSwitch();
    std::function<bool()> restartGame;                             // starts a new copy of the game and quits this one (Session::RestartGame)
    std::function<void(const std::string& text)> onRuleChanged;    // the host changed this lobby's mod rule: a line for the player
    void SetPublicState(const std::string& state, const std::string& map, const std::string& mode);
    void RefreshList();
    // Names and region (session/hsm_player_names.h, session/hsm_region.h). Set from HSMMod.ini at start and again when the
    // multiplayer page changes them. Nothing new is on the wire: lobby data (name, hsm_region) and member data (hsm_nick).
    void SetNickname(const std::string& nickname);   // member data hsm_nick: what the other members show for us ("" clears)
    void SetHideNames(bool on);                      // HIDE PLAYER NAMES: members() shows the other members as "Player N" on this screen
    void SetName(const std::string& lobbyName);      // the host's lobby name (key name); "" keeps "<you>'s fight". Stored before a lobby exists too
    void SetRegion(Region region);                   // the host's region (key hsm_region); Auto: none. Stored before a lobby exists too
    void SetListDistance(LobbyDistance d);           // how far the next RefreshList asks Steam to look
    // Match settings (lobby data, host writes) and teams (own pick in member data, host moves in lobby data).
    MatchConfig config() const;
    void SetConfig(const MatchConfig& c);
    std::string MapPick() const;                    // host's chosen level (package path), empty until picked
    void SetMapPick(const std::string& path);
    LobbyPack Pack() const;                         // the host's announced modpack (session/hsm_modpack.h); empty: none
    void SetPack(const LobbyPack& pack);            // host: announce it (empty: none); gameplay/mod_pack.cpp
    // Host: announce the lobby's mod rule (gameplay/mod_rules.cpp LobbyTick); only changed keys are written.
    void SetModRule(const ModRule& rule);
    // This lobby's rule as its lobby data names it (what a member is held to; the MODS tab). `named`: the host wrote one
    // (a lobby without hsm_mrule reads as NoRule).
    ModRule RoomRule(bool* named = nullptr) const;
    // Any member: this DLL's official-build verdict in member data (kMemberOfficialKey); written when it changes, since
    // the update check usually answers after the lobby is entered.
    void SetOfficial(Official o);
    bool Progression() const;                       // the host chose the Progression card (hsm_prog)
    void SetProgression(bool on);
    void SetTeamPick(int team);
    void SetOverride(steam::SteamId member, int team);
    void ClearOverrides();
    void ShuffleTeamsNow();                         // host: SHUFFLE NOW: deal everyone to a team at random, balanced (match/hsm_match.h ShuffleTeams)

    State state() const { return state_; }
    bool IsHost() const { return state_ == State::InLobby && owner_ == steam::MySteamId(); }
    bool InLobby() const { return state_ == State::InLobby; }
    bool Joining() const { return state_ == State::Joining; }   // Join asked Steam, the lobby not entered yet
    bool ServerLobby() const;                       // a role=server lobby: the owner is the server, not a player
    int PlayerCount() const;                        // members, minus the server in a role=server lobby
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
    std::string password_;                // the lobby's password as this machine knows it, for passwordLobby_
    steam::SteamId passwordLobby_ = 0;
    steam::SteamId askLobby_ = 0;         // the lobby the page should ask a password for
    std::string askNote_;                 // why it asks again ("Wrong password")
    void RuleTick();                      // lobby data changed: a new mod rule is told, and the switch-off offered
    SwitchAsk switchAsk_;
    steam::SteamId rejoinLobby_ = 0;      // joined again after a restart with mods off: no second restart for it
    steam::SteamId backDeclined_ = 0;     // NOT NOW to the mods-back restart here: not asked again this game
    void AskBack(steam::SteamId lobby, const std::string& name, const std::string& ruleText, const BackPlan& b);
    ModRule lastRule_;                    // this lobby's rule as last seen (RuleTick)
    bool lastRuleNamed_ = false;
    int maxPlayers_ = 8;                  // the host's choice (kDefaultPlayerLimit until it changes it, up to 20); hsm_max is what everyone reads
    std::string officialSent_;   // the kMemberOfficialKey value last written (SetOfficial)
    std::string nickname_, lobbyName_;    // this machine's nickname and lobby name (HSMMod.ini), both already cleaned
    bool hideNames_ = false;
    Region region_ = Region::Auto;
    LobbyDistance listDistance_ = LobbyDistance::Default;
    std::string LobbyTitleNow() const;    // the Steam lobby key "name": test prefix, then the lobby name or "<you>'s fight"
    bool server_ = false;   // Host(type, true): this machine runs role=server
    State state_ = State::Offline;
    steam::SteamId lobby_ = 0, owner_ = 0, joining_ = 0;   // joining_: the lobby Join asked for, until its enter
    steam::LobbyType type_ = steam::LobbyType::FriendsOnly;
    std::vector<LobbyMember> members_;
    std::vector<LobbyListing> listings_;
    std::string status_ = "Steam not ready";
    bool launchChecked_ = false;
};

}  // namespace hsm::game

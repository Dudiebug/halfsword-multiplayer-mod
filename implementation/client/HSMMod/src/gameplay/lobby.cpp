#include "gameplay/lobby.h"
#include "file_hash.h"
#include "gameplay/mod_pack.h"
#include "gameplay/mod_rules.h"
#include "hsm_protocol.h"
#include "log.h"
#include "match/hsm_build_card.h"
#include "session/hsm_build.h"
#include "session/hsm_global_bans.h"
#include "session/hsm_password.h"
#include "session/hsm_version_refusal.h"
#include "session/hsm_player_limit.h"
#include "session/hsm_player_names.h"
#include "ue/engine.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace hsm::game {
namespace {
#ifdef HSM_TESTBENCH
constexpr const char* kLobbyNamePrefix = "[TEST] ";
#else
constexpr const char* kLobbyNamePrefix = "";
#endif
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
// The version, the commit build.sh saw and the SHA-256 of this DLL's own file (session/hsm_build.h). Worked out once: the
// first call reads the DLL (a few MB; mod.cpp makes it on the worker thread at startup, before any session).
std::string ModBuild() {
    static const std::string build = ModBuildString(kModVersion, HSM_GIT_DESCRIBE, OwnBinaryHash());
    return build;
}
namespace {
bool g_allowBuildMismatch = false;
}  // namespace
void SetAllowBuildMismatch(bool on) { g_allowBuildMismatch = on; }
bool g_directP2P = false;
void SetDirectP2P(bool on) { g_directP2P = on; }
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
        if (g_directP2P) HSM_LOG("lobby: direct_p2p=1: %s", steam::AllowDirectP2P(true) ? "direct paths allowed with every lobby member (addresses shared)"
                                                                                  : "could not be set (no SetGlobalConfigValueInt32); the Steam default stays");
        steam::InitRelay();
        state_ = State::Idle;
        SetStatus("Not in a lobby");
    }
    steam::Tick();
    if (!launchChecked_) {          // launched through a friend's "Join Game", or restarted with some mods off for a lobby
        launchChecked_ = true;
        std::string id = steam::LaunchConnectLobby();
        RejoinFile rejoin;
        const bool rejoinWanted = modrules::TakeRejoin(rejoin);   // read and deleted either way
        if (!id.empty()) Join(std::strtoull(id.c_str(), nullptr, 10));
        else if (rejoinWanted) {
            rejoinLobby_ = steam::SteamId(rejoin.lobby);
            HSM_LOG("lobby: back after the restart: joining lobby %llu again", (unsigned long long)rejoin.lobby);
            Join(rejoinLobby_, rejoin.password);
        }
    }
    steam::Event e;
    while (steam::Poll(e)) {
        switch (e.kind) {
            case steam::Event::LobbyCreated:
                if (!e.ok) { state_ = State::Idle; SetStatus("Could not create lobby"); break; }
                steam::SetLobbyData(e.lobby, "hsm_proto", ProtocolStr());
                steam::SetLobbyData(e.lobby, "hsm_build", BuildKeyHex());
                steam::SetLobbyData(e.lobby, "hsm_ver", kModVersion);
                steam::SetLobbyData(e.lobby, "name", LobbyTitleNow());
                if (RegionKnown(region_)) steam::SetLobbyData(e.lobby, "hsm_region", RegionKey(region_));
                HSM_LOG("lobby: created as \"%s\" region=%s", LobbyTitleNow().c_str(), RegionKey(region_));
                steam::SetLobbyData(e.lobby, "state", "menu");
                steam::SetLobbyData(e.lobby, "hsm_srv", server_ ? "1" : "0");
                steam::SetLobbyData(e.lobby, "hsm_max", std::to_string(maxPlayers_));
                steam::SetLobbyData(e.lobby, "hsm_lock", "0");
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
                    if (l.switchable && (!l.pack.any() || modpack::Check(l.pack).verdict == PackVerdict::Ok)) {   // the rule refuses mods a restart can turn off: ask
                        steam::LeaveLobby(e.lobby); state_ = State::Idle;
                        SetStatus("This lobby needs some of your mods off: restart?");
                        switchAsk_ = SwitchAsk{e.lobby, l.name, l.ruleText, l.plan, false, false, {}};
                        break;
                    }
                    if (!l.compatible) { steam::LeaveLobby(e.lobby); state_ = State::Idle; SetStatus(l.modsText.empty() ? "Cannot join: " + l.reason : l.modsText); break; }
                    if (l.pack.any()) {   // the host's modpack: this machine must have exactly it (nothing is downloaded)
                        const PackCheck pc = modpack::Check(l.pack);
                        modpack::LogJoinCheck(l.pack, pc);
                        if (pc.verdict != PackVerdict::Ok) { steam::LeaveLobby(e.lobby); state_ = State::Idle; SetStatus(PackJoinLine(l.pack, pc)); break; }
                    }
                    if (l.locked && passwordLobby_ != e.lobby) {   // an invite or a friend's Join Game: no password was typed yet
                        steam::LeaveLobby(e.lobby); state_ = State::Idle;
                        SetStatus("This lobby needs a password");
                        AskPassword(e.lobby);
                        break;
                    }
                    Enter(e.lobby);
                    AskBack(e.lobby, l.name, l.ruleText, l.back);   // mods off for another lobby that this one takes
                }
                break;
            case steam::Event::LobbyMembersChanged:
            case steam::Event::LobbyDataChanged:
                if (e.lobby == lobby_ && state_ == State::InLobby) { RefreshMembers(); if (e.kind == steam::Event::LobbyDataChanged) RuleTick(); }
                break;
            case steam::Event::JoinRequested:
                if (e.lobby && e.lobby != lobby_) { Leave(); Join(e.lobby); }   // Leave is a no-op unless in or joining a lobby
                break;
            case steam::Event::LobbyList: {
                listings_.clear();
                for (steam::SteamId id : e.lobbies) if (LobbyListing l = Describe(id); !l.banned) listings_.push_back(std::move(l));
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
    l.region = LobbyRegion(steam::LobbyData(lobby, "hsm_region"));   // peer-written: only a region this build lists counts
    const std::vector<steam::SteamId> members = steam::LobbyMembers(lobby);
    const bool server = steam::LobbyData(lobby, "hsm_srv") == "1";
    l.players = int(members.size()) - (server && !members.empty() ? 1 : 0);
    l.maxPlayers = ParsePlayerLimit(steam::LobbyData(lobby, "hsm_max"));   // peer-written; an old lobby has none: 8
    std::string proto = steam::LobbyData(lobby, "hsm_proto"), build = steam::LobbyData(lobby, "hsm_build");
    l.banned = GloballyBanned(steam::LobbyOwner(lobby));   // Steam names the owner once the lobby is joined, and often in the list
    if (proto.empty()) l.reason = "not a HalfSwordMP lobby";
    else if (l.banned) l.reason = "the host is banned from HalfSwordMP";
    else if (proto != ProtocolStr()) l.reason = VersionLobbyReason(steam::LobbyData(lobby, "hsm_ver"), kModVersion);   // who is behind (#3073)
    else if (build != BuildKeyHex()) l.reason = "different game version";
    else if (LobbyIsFull(l.players, l.maxPlayers) && std::find(members.begin(), members.end(), steam::MySteamId()) == members.end())
        l.reason = "lobby full";   // a full lobby we are already in is not full to us
    l.locked = steam::LobbyData(lobby, "hsm_lock") == "1";
    l.pack = ReadLobbyPack(steam::LobbyData(lobby, "hsm_pack"), steam::LobbyData(lobby, "hsm_packv"), steam::LobbyData(lobby, "hsm_packh"));   // peer-written: validated
    if (!proto.empty()) {   // mod rules: the lobby's rule against this machine's own report (the host checks again after the join)
        const ModRule rule = DecodeLobbyRule({steam::LobbyData(lobby, kLobbyModRuleKey), steam::LobbyData(lobby, kLobbyBlockHseKey),
                                              steam::LobbyData(lobby, kLobbyModOfficialKey), steam::LobbyData(lobby, kLobbyModListKey)});   // peer-written: DecodeLobbyRule reads it strictly
        const LobbyModView v = ViewLobbyRule(rule, modrules::OwnAsGuest(), modrules::OwnOfficial());
        l.mods = v.tag;   // names "HS Enhancer ok" when the host allows it, so nobody joins one by accident
        l.ruleText = RuleSummary(rule);
        if (modrules::Enabled() && !v.reason.empty()) {   // mod_rules=0: no pre-check here, the host still decides
            l.modsText = v.text;
            const bool onlyMods = l.reason.empty();
            if (onlyMods) l.reason = v.reason;
            // What a restart with some mods off would fix (session/hsm_mod_switch.h). Not for the lobby this game restarted
            // for already, and not where an unofficial build is the trouble: no restart helps there.
            if (onlyMods && lobby != rejoinLobby_ && !(rule.officialOnly && modrules::OwnOfficial() == Official::No)) l.plan = PlanSwitch(rule, modrules::OwnAsGuest());
            l.switchable = onlyMods && l.plan.Switchable();
        }
        // Mods off for another lobby, and this one takes them all: a restart brings them back (session/hsm_mod_switch.h).
        if (modrules::Enabled() && l.reason.empty() && !(rule.officialOnly && modrules::OwnOfficial() == Official::No))
            if (const ModReport* full = modrules::OwnBeforeSwitch())
                l.back = PlanBack(rule, *full, modrules::Own(), modrules::HseLockedForLobby(), modrules::HseKeysHoldable());
    }
    l.compatible = l.reason.empty();
    return l;
}

void Lobby::Host(steam::LobbyType type, bool server) {
    if (!server && onlineBlock) if (const std::string why = onlineBlock(hostPreset, modrules::HostRule(*this).blockHse, ClassPowerPoints(kClassPowerDefault)); !why.empty()) { SetStatus(why); return; }
    if (state_ != State::Idle) return;
    password_.clear(); passwordLobby_ = 0; askLobby_ = 0; askNote_.clear();
    type_ = type; server_ = server; state_ = State::Creating; SetStatus("Creating lobby...");
    steam::CreateLobby(type, maxPlayers_ + (server ? 1 : 0));
}

ModRule Lobby::RoomRule(bool* named) const {
    const std::string rule = InLobby() ? steam::LobbyData(lobby_, kLobbyModRuleKey) : std::string();
    if (named) *named = !rule.empty();
    if (rule.empty()) return NoRule();
    return DecodeLobbyRule({rule, steam::LobbyData(lobby_, kLobbyBlockHseKey), steam::LobbyData(lobby_, kLobbyModOfficialKey),
                            steam::LobbyData(lobby_, kLobbyModListKey)});   // peer-written: DecodeLobbyRule reads it strictly
}

bool Lobby::ServerLobby() const { return InLobby() && steam::LobbyData(lobby_, "hsm_srv") == "1"; }

int Lobby::PlayerCount() const { return int(members_.size()) - (ServerLobby() && !members_.empty() ? 1 : 0); }

bool Lobby::Locked() const { return InLobby() && steam::LobbyData(lobby_, "hsm_lock") == "1"; }

const std::string& Lobby::Password() const {
    static const std::string none;
    return passwordLobby_ && passwordLobby_ == lobby_ && InLobby() ? password_ : none;
}

void Lobby::SetPassword(const std::string& password) {
    if (!IsHost()) return;
    password_ = CleanPassword(password); passwordLobby_ = password_.empty() ? 0 : lobby_;
    steam::SetLobbyData(lobby_, "hsm_lock", password_.empty() ? "0" : "1");   // only whether, never the password
}

void Lobby::RememberPassword(const std::string& password) {
    if (!InLobby() || IsHost()) return;
    password_ = CleanPassword(password); passwordLobby_ = password_.empty() ? 0 : lobby_;
}

void Lobby::AskPassword(steam::SteamId lobby, const std::string& note) { askLobby_ = lobby; askNote_ = note; }

void Lobby::AskBack(steam::SteamId lobby, const std::string& name, const std::string& ruleText, const BackPlan& b) {
    if (!b.Any() || lobby == backDeclined_ || switchAsk_.lobby) return;
    HSM_LOG("lobby: this lobby takes the mods this game has off: offering a restart with %s", BackWords(b).c_str());
    SwitchAsk ask; ask.lobby = lobby; ask.name = name; ask.ruleText = ruleText; ask.rejoin = true; ask.back = true; ask.backPlan = b;
    switchAsk_ = ask;
}

bool Lobby::ConfirmSwitch() {
    const SwitchAsk ask = switchAsk_;
    switchAsk_ = SwitchAsk{};
    if (!ask.lobby) return false;
    const std::string words = ask.back ? BackWords(ask.backPlan) : SwitchWords(ask.plan);
    HSM_LOG("lobby: restarting with %s for %s (%s)", words.c_str(), ask.name.empty() ? "a lobby" : ask.name.c_str(), ask.ruleText.c_str());
    if (ask.back) {
        if (!modrules::ModsBackForNextGame(ask.backPlan.lockHse)) { SetStatus("Could not set up the restart (see HSMMod.log)"); return false; }
    } else if (!modrules::TurnOffForNextGame(ask.plan.modsTxt, ask.plan.exeDlls, ask.plan.paks, ask.plan.lockHse)) { SetStatus("Could not turn the mods off (see HSMMod.log)"); return false; }
    RejoinFile r;
    r.lobby = ask.lobby;
    r.until = uint64_t(std::time(nullptr)) + kRejoinWindowS;
    if (passwordLobby_ == ask.lobby) r.password = password_;   // typed for this lobby, or remembered from its host
    if (!modrules::WriteRejoin(r)) HSM_LOG("lobby: could not write the rejoin file: the lobby has to be joined by hand after the restart");
    if (!restartGame || !restartGame()) { SetStatus("Could not restart the game: start it again yourself, then join"); return false; }
    SetStatus("Restarting with " + words + "...");
    return true;
}

// The host changed this lobby's mod rule while we are in it (lobby data): everyone hears, and a member whose mods no longer
// fit is offered the restart, inside the host's grace (kModRuleGraceMs). The first rule named after Enter is the one we
// joined under, not a change.
void Lobby::RuleTick() {
    bool named = false;
    const ModRule rule = RoomRule(&named);
    if (!named) return;
    if (!lastRuleNamed_) { lastRuleNamed_ = true; lastRule_ = rule; return; }
    if (rule == lastRule_) return;
    lastRule_ = rule;
    HSM_LOG("lobby: the host changed the mod rules: %s", RuleSummary(rule).c_str());
    if (IsHost()) return;   // the host's own change: its chat line says so
    if (onRuleChanged) onRuleChanged("Host changed mods: " + RuleSummary(rule));
    if (!modrules::Enabled() || switchAsk_.lobby) return;
    if (lobby_ != rejoinLobby_) {
        const SwitchPlan plan = PlanSwitch(rule, modrules::OwnAsGuest());
        if (plan.Switchable()) { switchAsk_ = SwitchAsk{lobby_, steam::LobbyData(lobby_, "name"), RuleSummary(rule), plan, true, false, {}}; return; }
    }
    // Looser now: the mods this game has off for another lobby may all fit (Describe's back, for a rule change).
    if (rule.officialOnly && modrules::OwnOfficial() == Official::No) return;
    if (const ModReport* full = modrules::OwnBeforeSwitch())
        AskBack(lobby_, steam::LobbyData(lobby_, "name"), RuleSummary(rule), PlanBack(rule, *full, modrules::Own(), modrules::HseLockedForLobby(), modrules::HseKeysHoldable()));
}

int Lobby::MaxPlayers() const { return InLobby() ? ParsePlayerLimit(steam::LobbyData(lobby_, "hsm_max")) : maxPlayers_; }

void Lobby::SetMaxPlayers(int n) {
    if (!IsHost()) return;
    maxPlayers_ = ClampPlayerLimit(n, PlayerCount());
    steam::SetLobbyMemberLimit(lobby_, maxPlayers_ + (ServerLobby() ? 1 : 0));   // an old steam_api64 lacks it: the host's Hello check still holds
    steam::SetLobbyData(lobby_, "hsm_max", std::to_string(maxPlayers_));
}

void Lobby::Join(steam::SteamId lobby, const std::string& password) {
    if (state_ != State::Idle || !lobby) return;
    if (onlineBlock) {
        const ModRule r = DecodeLobbyRule({steam::LobbyData(lobby, kLobbyModRuleKey), steam::LobbyData(lobby, kLobbyBlockHseKey), "", ""});   // peer-written: read strictly
        // A lobby that blocks HS Enhancer: once in, Describe offers the restart that locks it (LobbyEntered). Refused
        // here only where none comes: the lobby this game restarted for already, or no pre-check (mod_rules=0).
        const bool lockLater = modrules::Enabled() && lobby != rejoinLobby_;
        const double power = ClassPowerPoints(IntOr(steam::LobbyData(lobby, "hsm_cpow"), kClassPowerDefault));   // the host's class power limit
        if (const std::string why = onlineBlock(r.preset, r.blockHse && !lockLater, power); !why.empty()) { SetStatus(why); return; }
    }
    password_ = CleanPassword(password); passwordLobby_ = password_.empty() ? 0 : lobby;
    askLobby_ = 0; askNote_.clear();
    joining_ = lobby; state_ = State::Joining; SetStatus("Joining lobby...");
    steam::JoinLobby(lobby);
}

void Lobby::Enter(steam::SteamId lobby) {
    lobby_ = lobby; state_ = State::InLobby; owner_ = steam::LobbyOwner(lobby);
    steam::SetMemberData(lobby_, "hsm_ver", kModVersion);
    steam::SetMemberData(lobby_, "hsm_mod", ModBuild());   // the lobby names every other player on another build
    steam::SetMemberData(lobby_, "hsm_nick", nickname_);   // "": no nickname, the Steam name shows
    if (onEnterCard) onEnterCard();
    if (myCard) steam::SetMemberData(lobby_, "hsm_card", myCard());
    if (myRuleSheet) steam::SetMemberData(lobby_, "hsm_sheet", myRuleSheet());
    officialSent_.clear();
    SetOfficial(modrules::OwnOfficial());   // mod rules: the update check's verdict on this DLL (LobbyTick updates it)
    lastRule_ = RoomRule(&lastRuleNamed_);   // the rule we joined under; RuleTick tells of changes from here
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
    password_.clear(); passwordLobby_ = 0;
    lastRuleNamed_ = false;
    if (switchAsk_.rejoin) switchAsk_ = SwitchAsk{};   // the question was about this lobby
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
    c.teams = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_teams"), 2), 0, 255));   // none: two; hsm_fmt=2, the old "3 teams", is Teams with 3 (Sanitize)
    // the limit's key is the format's own (LimitKeyFor): hsm_trnr rounds per fight (Tournament), hsm_pbwin wins to take the match (Pit Bridge Duels)
    c.scoreLimit = uint16_t(std::clamp(LimitFromKey(c.format, IntOr(steam::LobbyData(lobby_, LimitKeyFor(c.format)), LimitToKey(c.format, DefaultScoreLimit(c.format)))), 0, 1000));
    c.timeLimitS = uint16_t(IntOr(steam::LobbyData(lobby_, "hsm_time"), 0));
    c.respawnDelayS = uint16_t(IntOr(steam::LobbyData(lobby_, "hsm_resp"), 5));
    c.friendlyFire = steam::LobbyData(lobby_, "hsm_ff") == "1";
    c.gear = Gear(IntOr(steam::LobbyData(lobby_, "hsm_gear"), 0));
    c.tier = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_tier"), 2), 0, 7));
    {
        // hsm_wpm is the ticked weapon types; an older host wrote hsm_wpn, one of five filters (WeaponMaskFromKeys). None: Any.
        const std::string wpm = steam::LobbyData(lobby_, "hsm_wpm");
        c.weapons = WeaponMaskFromKeys(!wpm.empty(), IntOr(wpm, 0), IntOr(steam::LobbyData(lobby_, "hsm_wpn"), 0));
    }
    c.teamKit = TeamKit(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_kit"), int(TeamKit::Full)), 0, int(kTeamKitCount) - 1));   // none: Full kit
    c.crossbows = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_xbow"), 0), 0, int(kCrossbowModeCount) - 1));   // none: Off
    c.fuseS = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_fuse"), kFuseDefaultS), int(kFuseMinS), int(kFuseMaxS)));   // none: 30 s (Tag)
    c.taggerSpeedPct = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_tspd"), kTaggerSpeedDefault), 0, 255));   // none: 110% (Sanitize snaps it to a stop)
    c.shuffle = TeamShuffle(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_shuf"), 0), 0, 255));   // none: Off (Sanitize)
    c.timeOfDay = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_tod"), 0), 0, 255));          // none: the map's own (Sanitize)
    c.winBy = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_winby"), 0), 0, 2));                     // none: Death
    c.propsOff = steam::LobbyData(lobby_, "hsm_props") == "0";                                              // none: On
    c.classPower = SnapClassPower(IntOr(steam::LobbyData(lobby_, "hsm_cpow"), kClassPowerDefault));   // none: +15
    c.stripOff = steam::LobbyData(lobby_, "hsm_strip") == "0";                                              // none: On
    c.ownBodies = steam::LobbyData(lobby_, "hsm_body") == "1";                                             // none: Same for all
    c.keepKit = steam::LobbyData(lobby_, "hsm_keep") == "1";                                               // none: New each fight
    c.scenario = ScenarioFromKey(steam::LobbyData(lobby_, "hsm_scen"));                              // none: Standard
    c.damagePct = uint16_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_dmg"), kDamageStock), 0, int(kDamageStock)));   // none: stock
    c.deadWeightPct = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_dwt"), kWeightDefaultPct), 0, int(kWeightMaxPct)));     // none: 50%
    c.weaponWeightPct = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_wwt"), kWeightDefaultPct), 0, int(kWeightMaxPct)));   // none: 50%
    c.waveBreakS = uint16_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_wbrk"), 0), 0, 600));      // 0: the format's own
    c.clearGroundMin = uint8_t(std::clamp(IntOr(steam::LobbyData(lobby_, "hsm_clr"), 0), 0, 255));    // 0: off
    c.freeCam = steam::LobbyData(lobby_, "hsm_fcam") != "0";                                       // none: on
    c.hostPhysics = steam::LobbyData(lobby_, "hsm_sync") == "host";                                  // none: per player
    return Sanitize(c);
}

void Lobby::SetConfig(const MatchConfig& in) {
    if (!IsHost()) return;
    MatchConfig c = Sanitize(in);
    steam::SetLobbyData(lobby_, "hsm_fmt", std::to_string(int(c.format)));
    steam::SetLobbyData(lobby_, "hsm_teams", std::to_string(c.teams));
    steam::SetLobbyData(lobby_, LimitKeyFor(c.format), std::to_string(LimitToKey(c.format, c.scoreLimit)));
    steam::SetLobbyData(lobby_, "hsm_time", std::to_string(c.timeLimitS));
    steam::SetLobbyData(lobby_, "hsm_resp", std::to_string(c.respawnDelayS));
    steam::SetLobbyData(lobby_, "hsm_ff", c.friendlyFire ? "1" : "0");
    steam::SetLobbyData(lobby_, "hsm_gear", std::to_string(int(c.gear)));
    steam::SetLobbyData(lobby_, "hsm_tier", std::to_string(int(c.tier)));
    steam::SetLobbyData(lobby_, "hsm_wpm", std::to_string(int(c.weapons)));
    steam::SetLobbyData(lobby_, "hsm_kit", std::to_string(int(c.teamKit)));
    steam::SetLobbyData(lobby_, "hsm_xbow", std::to_string(c.crossbows));
    steam::SetLobbyData(lobby_, "hsm_fuse", std::to_string(c.fuseS));
    steam::SetLobbyData(lobby_, "hsm_tspd", std::to_string(c.taggerSpeedPct));
    steam::SetLobbyData(lobby_, "hsm_shuf", std::to_string(int(c.shuffle)));
    steam::SetLobbyData(lobby_, "hsm_cpow", std::to_string(c.classPower));
    steam::SetLobbyData(lobby_, "hsm_strip", c.stripOff ? "0" : "1");
    steam::SetLobbyData(lobby_, "hsm_keep", c.keepKit ? "1" : "0");
    steam::SetLobbyData(lobby_, "hsm_body", c.ownBodies ? "1" : "0");
    steam::SetLobbyData(lobby_, "hsm_scen", std::to_string(c.scenario));
    steam::SetLobbyData(lobby_, "hsm_tod", std::to_string(c.timeOfDay));
    steam::SetLobbyData(lobby_, "hsm_winby", std::to_string(c.winBy));
    steam::SetLobbyData(lobby_, "hsm_props", c.propsOff ? "0" : "1");
    steam::SetLobbyData(lobby_, "hsm_dmg", std::to_string(c.damagePct));
    steam::SetLobbyData(lobby_, "hsm_dwt", std::to_string(c.deadWeightPct));
    steam::SetLobbyData(lobby_, "hsm_wwt", std::to_string(c.weaponWeightPct));
    steam::SetLobbyData(lobby_, "hsm_wbrk", std::to_string(c.waveBreakS));
    steam::SetLobbyData(lobby_, "hsm_clr", std::to_string(c.clearGroundMin));
    steam::SetLobbyData(lobby_, "hsm_fcam", c.freeCam ? "1" : "0");
    steam::SetLobbyData(lobby_, "hsm_sync", c.hostPhysics ? "host" : "owner");
}
std::string Lobby::MapPick() const { return InLobby() ? steam::LobbyData(lobby_, "hsm_map") : std::string(); }
void Lobby::SetMapPick(const std::string& path) { if (IsHost()) steam::SetLobbyData(lobby_, "hsm_map", path); }
LobbyPack Lobby::Pack() const {
    return InLobby() ? ReadLobbyPack(steam::LobbyData(lobby_, "hsm_pack"), steam::LobbyData(lobby_, "hsm_packv"), steam::LobbyData(lobby_, "hsm_packh")) : LobbyPack{};
}
void Lobby::SetPack(const LobbyPack& p) {
    if (!IsHost() || (!p.any() && !Pack().any())) return;   // nothing announced, nothing to clear
    steam::SetLobbyData(lobby_, "hsm_pack", p.name);
    steam::SetLobbyData(lobby_, "hsm_packv", p.version);
    steam::SetLobbyData(lobby_, "hsm_packh", p.hash);
}
void Lobby::SetModRule(const ModRule& rule) {
    if (!IsHost()) return;
    const LobbyModData d = EncodeLobbyRule(rule);
    bool changed = false;
    const auto put = [&](const char* key, const std::string& v) {
        if (steam::LobbyData(lobby_, key) == v) return;
        steam::SetLobbyData(lobby_, key, v);
        changed = true;
    };
    put(kLobbyModRuleKey, d.rule); put(kLobbyBlockHseKey, d.hse); put(kLobbyModOfficialKey, d.official); put(kLobbyModListKey, d.list);
    if (changed) HSM_LOG("lobby: mod rule announced: %s", RuleSummary(rule).c_str());
}

void Lobby::SetOfficial(Official o) {
    if (!InLobby()) return;
    const char* name = OfficialName(o);
    if (officialSent_ == name) return;
    steam::SetMemberData(lobby_, kMemberOfficialKey, name);
    officialSent_ = name;
}

bool Lobby::Progression() const { return InLobby() && steam::LobbyData(lobby_, "hsm_prog") == "1"; }
void Lobby::SetProgression(bool on) { if (IsHost()) steam::SetLobbyData(lobby_, "hsm_prog", on ? "1" : "0"); }

void Lobby::SetTeamPick(int team) { if (InLobby()) steam::SetMemberData(lobby_, "hsm_team", std::to_string(team)); }

void Lobby::SetOverride(steam::SteamId member, int team) {
    if (IsHost()) steam::SetLobbyData(lobby_, "hsm_ovr_" + std::to_string(member), std::to_string(team));
}

void Lobby::ClearOverrides() { for (const LobbyMember& m : members_) SetOverride(m.id, -1); }

// SHUFFLE NOW: deal everyone in the room to a team at random, balanced, and show it as the host's moves. They hold for the next
// START MATCH (StartMatch honours the host's moves) and a later shuffle clears them. Nobody's own pick counts.
void Lobby::ShuffleTeamsNow() {
    const MatchConfig c = config();
    if (!IsHost() || !PicksTeams(c.format)) return;
    std::vector<TeamRequest> req;
    std::vector<steam::SteamId> who;
    for (const LobbyMember& m : members_) {
        if (who.size() >= 255 || (m.host && ServerLobby())) continue;   // a dedicated server's owner plays no team
        TeamRequest r; r.slot = uint8_t(who.size());
        req.push_back(r); who.push_back(m.id);
    }
    const auto deal = ShuffleTeams(req, c.format, c.teams, uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    for (size_t i = 0; i < who.size(); ++i) SetOverride(who[i], deal.at(uint8_t(i)));
    HSM_LOG("lobby: shuffled %zu players into %d teams", who.size(), TeamCount(c.format, c.teams));
}

bool Lobby::BlockHse() const { return InLobby() ? RoomRule().blockHse : modrules::Chosen().blockHse; }
bool Lobby::InUnrestrictedLobby() const { return InLobby() && steam::LobbyData(lobby_, kLobbyModRuleKey) == std::to_string(int(ModPreset::Unrestricted)); }
bool Lobby::InClassesLobby() const { return InLobby() && (InUnrestrictedLobby() || steam::LobbyData(lobby_, kLobbyModRuleKey) == std::to_string(int(ModPreset::Classes))); }

void Lobby::RepublishMyCard() {
    if (!InLobby() || !myCard) return;
    const std::string card = myCard();
    if (steam::MemberData(lobby_, steam::MySteamId(), "hsm_card") != card) steam::SetMemberData(lobby_, "hsm_card", card);
    if (myRuleSheet) {
        const std::string sheet = myRuleSheet();
        if (steam::MemberData(lobby_, steam::MySteamId(), "hsm_sheet") != sheet) steam::SetMemberData(lobby_, "hsm_sheet", sheet);
    }
}

void Lobby::SetReady(bool ready) { if (InLobby()) steam::SetMemberData(lobby_, "ready", ready ? "1" : "0"); }

void Lobby::SetPublicState(const std::string& state, const std::string& map, const std::string& mode) {
    if (!IsHost()) return;
    steam::SetLobbyData(lobby_, "state", state); steam::SetLobbyData(lobby_, "map", map); steam::SetLobbyData(lobby_, "mode", mode);
}

void Lobby::RefreshList() {
    if (state_ == State::Offline) return;
    SetStatus("Searching lobbies...");
    steam::RequestLobbyList("hsm_proto", ProtocolStr(), listDistance_ == LobbyDistance::Default ? -1 : int(listDistance_));   // Default: Steam's own
}

std::string Lobby::LobbyTitleNow() const {
    return kLobbyNamePrefix + LobbyTitle(lobbyName_, OwnName(nickname_, steam::PersonaName(steam::MySteamId())));
}
void Lobby::SetName(const std::string& lobbyName) {
    lobbyName_ = UsableLobbyName(lobbyName);
    if (!IsHost()) return;
    steam::SetLobbyData(lobby_, "name", LobbyTitleNow());
    HSM_LOG("lobby: name is now \"%s\"", LobbyTitleNow().c_str());
}
void Lobby::SetRegion(Region region) {
    if (region == region_) return;
    region_ = region;
    if (!IsHost()) return;
    steam::SetLobbyData(lobby_, "hsm_region", RegionKnown(region_) ? RegionKey(region_) : "");
    HSM_LOG("lobby: region is now %s", RegionKey(region_));
}
void Lobby::SetListDistance(LobbyDistance d) { listDistance_ = d; }
void Lobby::SetNickname(const std::string& nickname) {
    nickname_ = UsableNickname(nickname);
    if (!InLobby()) return;
    steam::SetMemberData(lobby_, "hsm_nick", nickname_);
    if (IsHost()) steam::SetLobbyData(lobby_, "name", LobbyTitleNow());   // "<you>'s fight" follows the name others see
    RefreshMembers();
}
void Lobby::SetHideNames(bool on) {
    if (on == hideNames_) return;
    hideNames_ = on;
    HSM_LOG("lobby: hide_names=%d", on ? 1 : 0);
    if (!InLobby()) return;
    if (onEnterCard) onEnterCard();                 // the cards already queued carry the old names
    for (LobbyMember& m : members_) m.card.clear();   // so every card counts as new in the refresh and is queued again
    RefreshMembers();
}

void Lobby::RefreshMembers() {
    steam::SteamId owner = steam::LobbyOwner(lobby_);
    if (owner && owner != owner_) {           // host left and Steam promoted someone
        const steam::SteamId old = owner_;
        owner_ = owner;
        if (!onOwnerChanged || !onOwnerChanged(old, owner)) {
            SetStatus("Host left the lobby");
            Leave();
            return;
        }
        SetStatus(owner == steam::MySteamId() ? "The host left. You are the new host." : "The host left. " + (steam::PersonaName(owner).empty() ? std::string("Another player") : steam::PersonaName(owner)) + " is the new host.");
    }
    std::vector<steam::SteamId> ids = steam::LobbyMembers(lobby_);
    const bool synced = !members_.empty();   // the first refresh after Enter lists whoever was here: not "joined" lines
    for (const LobbyMember& m : members_) {
        if (std::find(ids.begin(), ids.end(), m.id) != ids.end() || m.id == steam::MySteamId()) continue;
        if (onMemberLeft) onMemberLeft(m.id);
        if (onPresence) onPresence(m.name, false);
    }
    std::vector<LobbyMember> next;
    const bool serverLobby = ServerLobby();
    int nextSlot = 1;   // slots as a match numbers them: the host's fighter is 0, the others follow (a role=server lobby has no slot 0)
    for (steam::SteamId id : ids) {
        if (!id) continue;
        LobbyMember m; m.id = id; m.steamName = steam::PersonaName(id); m.host = id == owner_;
        // Shown name: their nickname (peer-written, cleaned again here) or their Steam name; HIDE PLAYER NAMES makes everyone but us
        // (and a server, which is no player) "Player N".
        const std::string nick = id == steam::MySteamId() ? nickname_ : CleanPeerName(steam::MemberData(lobby_, id, "hsm_nick"));
        const bool isServer = serverLobby && m.host;
        const int slot = isServer ? -1 : m.host ? 0 : nextSlot++;
        m.name = DisplayName(hideNames_ && !isServer, id == steam::MySteamId(), slot, nick.empty() ? SanitizeChat(m.steamName) : nick);
        m.steamName = SanitizeChat(m.steamName);   // no emoji or control characters; a Steam name is not cut at 32 bytes here
        m.ready = steam::MemberData(lobby_, id, "ready") == "1";
        m.build = steam::MemberData(lobby_, id, "hsm_mod").substr(0, kMaxModBuild);   // peer-written
        m.card = steam::MemberData(lobby_, id, "hsm_card").substr(0, kMaxCardValue);   // peer-written; decoded by hsm_build_card.h
        auto was = std::find_if(members_.begin(), members_.end(), [&](const LobbyMember& o) { return o.id == id; });
        if (!m.card.empty() && onBuildCard && (was == members_.end() || was->card != m.card || was->name != m.name)) onBuildCard(m.name, m.card);
        if (id != steam::MySteamId() && !nick.empty() && nick != m.steamName && (was == members_.end() || was->name != m.name))
            HSM_LOG("lobby: %s is %s on Steam%s", nick.c_str(), m.steamName.c_str(), (hideNames_ ? " (shown as " + m.name + ")" : std::string()).c_str());
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

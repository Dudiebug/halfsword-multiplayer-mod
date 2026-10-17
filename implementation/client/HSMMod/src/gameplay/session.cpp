#include "diagnostics/stall.h"
#include "session/hsm_stock_foes.h"
// Session orchestration on the game thread, pumped from BP_HalfSwordGameMode_C::ReceiveTick (menus included).
// Steam mode: the Steam lobby decides roles (owner hosts) and membership; the host's level changes pull every
//   member along (Travel), and clients may not leave the menu on their own while joined.
// Gameplay (Phase 2): the host is the authority for up to 6 fighters. Slot 0 is the host's stock fighter; every
//   client gets a mod-spawned Willie on the host wearing the client's own passport (relayed as a SaveGame blob),
//   driven by the client's input and aim. Clients fight as their own stock player 0, mirror host poses, and
//   show every other fighter as a brainless puppet with the right loadout. Decisions live in hsm_director.h.
// Matches (Phase 3): in fight levels (not the Tavern hub) with 2+ players, or players-vs-bots, the host runs
//   hsm::Match: team picks from the lobby, kill credit from weapon hit delegates, friendly-fire filtering,
//   respawns on team sides, bot waves, and a MatchInfo/Kill feed every client renders in the same HUD. Stock
//   NPCs that are not part of the match are removed so every machine sees the same fight.
// Owner sync (protocol rev 9, the default; session/hsm_authority.h): instead of the host simulating everyone, every
//   machine simulates its own fighter natively (the host also its bots) and streams it (game::OwnerStreams); every
//   fighter owned elsewhere is a neutralised stand-in servoed to that stream (game::StandIns). Blows on a stand-in
//   become hit claims replayed by the victim's owner through the game's own damage code (game::Claims). The host
//   relays the streams, routes the claims and referees the match. sync_mode=host (or a host without the script VM
//   hook) keeps the host-simulates-all flow above, unchanged.
#include "config.h"
#include "gameplay/adapter.h"
#include "gameplay/claims.h"
#include "gameplay/combat_sync.h"
#include "gameplay/equip_sync.h"
#include "gameplay/fx_sync.h"
#include "gameplay/gear.h"
#include "gameplay/grab_sync.h"
#include "gameplay/mirror.h"
#include "gameplay/owner_sync.h"
#include "gameplay/prop_sync.h"
#include "gameplay/severs.h"
#include "gameplay/standin_gore.h"
#include "gameplay/spectate.h"
#include "gameplay/lobby.h"
#include "gameplay/session.h"
#include "gameplay/travel.h"
#include "gameplay/world_sync.h"
#include "hsm_chat.h"
#include "hsm_input.h"
#include "hsm_jitter.h"
#include "hsm_owner.h"
#include "hsm_protocol.h"
#include "log.h"
#include "match/hsm_hud_text.h"
#include "match/hsm_match.h"
#include "match/hsm_spectate.h"
#include "net/steam_transport.h"
#include "net/transport.h"
#include "session/hsm_authority.h"
#include "session/hsm_blob.h"
#include "session/hsm_combat.h"
#include "session/hsm_director.h"
#include "session/hsm_level_gate.h"
#include "session/hsm_reconnect.h"
#include "session/hsm_roster.h"
#include "session/hsm_spawn.h"
#include "session/hsm_world.h"
#include "agent/bridge.h"
#include "agent/vmhook.h"
#include "paths.h"
#include "test/harness.h"
#include "ui/chat.h"
#include "ui/hud.h"
#include "ui/menu.h"
#include "ue/engine.h"
#include "ue/hook.h"
#include "ue/script_hook.h"
#include <windows.h>
#include <array>
#include <deque>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <optional>
#include <algorithm>

namespace hsm {
using namespace hsm::ue;
namespace {

constexpr const char* kGameModeClass = "BP_HalfSwordGameMode_C";
constexpr uint64_t kHostKey = 0;                 // roster key of the host itself (SteamIDs and ENet keys are never 0)
constexpr double kRingRadius = 120.0;            // cm between fighters sharing one spawn point
constexpr double kTeleportDistance = 300.0;
constexpr size_t kHitImpactPointOffset = 40;     // FHitResult: FaceIndex, Time, Distance, Location(24) then ImpactPoint           // off: first live test showed puppets missing on that path      // client: snap its own fighter on the first snapshot if farther
constexpr uint8_t kFirstBotEntity = kMaxFighters + 1, kLastBotEntity = uint8_t(kMaxWillies);
static_assert(Match::kMaxBotsAlive <= kLastBotEntity - kFirstBotEntity + 1, "every live bot needs an entity");
constexpr uint32_t kInfoIntervalMs = 1000, kScanIntervalMs = 1000, kFeedMs = 6000, kTintRetryMs = 500;
constexpr int kTintTries = 8;
constexpr uint32_t kKeepAliveMs = 1000;          // owner sync: the host's own traffic when it streams nothing (HostLossMonitor)
constexpr uint32_t kVitalsDeathMs = 1500;        // owner sync: a fighter its owner reports dead this long without a DeathReport
constexpr const char* kHubLevel = "/Game/Maps/Map_Hub_Tavern_Frank";
constexpr const char* kMenuLevel = "/Game/Maps/Menus/Map_Menu_Startup";

uint32_t NowMs() { return uint32_t(GetTickCount64()); }
bool IsMenuLevel(const std::string& level) { return level.find("/Maps/Menus/") != std::string::npos || level.find("Map_Menu_") != std::string::npos; }
double Dist(const FVector& a, const FVector& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
// When a message arrived (the transport's receive stamp, on game::SteadyNowMs's clock), not when this frame polled it:
// a long frame no longer counts as network jitter in a stand-in's pose buffer. No stamp, or one from the future: now.
double ArrivalMs(const NetEvent& ev) {
    const double now = game::SteadyNowMs();
    return ev.rxMs > 0 && ev.rxMs <= now ? ev.rxMs : now;
}

// A Hello from another protocol revision passes validation so the host can answer it with a Reject.
bool IsForeignHello(const uint8_t* d, size_t n) {
    return n >= 4 && uint16_t(d[0] | d[1] << 8) == kMagic && d[2] != kProtocolRevision && d[3] == uint8_t(MsgType::Hello);
}

bool ValidateForHost(const uint8_t* d, size_t n, uint8_t) {
    MsgType t; if (!PeekType(d, n, t)) return IsForeignHello(d, n);
    switch (t) {
        case MsgType::Hello: { Hello m; return Decode(d, n, m); }
        case MsgType::Input: { InputFrame m; return Decode(d, n, m); }
        case MsgType::Ready: { Ready m; return Decode(d, n, m); }
        case MsgType::BlobChunk: { BlobChunk m; return Decode(d, n, m); }
        case MsgType::Chat: { ChatLine m; return Decode(d, n, m); }
        case MsgType::Ping: case MsgType::Pong: { Ping m; bool p; return DecodePing(d, n, m, p); }
        case MsgType::Pose: { PoseMsg m; return Decode(d, n, m); }
        case MsgType::Vitals: { VitalsMsg m; return Decode(d, n, m); }
        case MsgType::DamageCredit: { DamageCredit m; return Decode(d, n, m); }
        case MsgType::DeathReport: { DeathReport m; return Decode(d, n, m); }
        case MsgType::HitClaim: { HitClaimMsg m; return Decode(d, n, m); }
        case MsgType::SeverClaim: { combat::SeverClaimMsg m; return Decode(d, n, m); }
        case MsgType::StuckBlade: { combat::StuckBladeMsg m; return Decode(d, n, m); }
        case MsgType::GrabState: { GrabStateMsg m; return Decode(d, n, m); }
        case MsgType::PropState: { PropStateMsg m; return Decode(d, n, m); }
        case MsgType::PropLease: { PropLeaseMsg m; return Decode(d, n, m) && m.action != LeaseAction::Grant; }
        case MsgType::PaintFx: { combat::PaintFxMsg m; return Decode(d, n, m); }
        case MsgType::Equip: { combat::EquipStateMsg m; return Decode(d, n, m); }
        default: return false;
    }
}
bool ValidateForClient(const uint8_t* d, size_t n, uint8_t) {
    MsgType t; if (!PeekType(d, n, t)) return false;
    switch (t) {
        case MsgType::Welcome: { Welcome m; return Decode(d, n, m); }
        case MsgType::Reject: { Reject m; return Decode(d, n, m); }
        case MsgType::Snapshot: { Snapshot m; return Decode(d, n, m); }
        case MsgType::Event: { Event m; return Decode(d, n, m); }
        case MsgType::Travel: { Travel m; return Decode(d, n, m); }
        case MsgType::Spawn: { Spawn m; return Decode(d, n, m); }
        case MsgType::Despawn: { Despawn m; return Decode(d, n, m); }
        case MsgType::BlobChunk: { BlobChunk m; return Decode(d, n, m); }
        case MsgType::MatchInfo: { MatchInfo m; return Decode(d, n, m); }
        case MsgType::Kill: { Kill m; return Decode(d, n, m); }
        case MsgType::Chat: { ChatLine m; return Decode(d, n, m); }
        case MsgType::CombatEvent: { CombatEvent m; return Decode(d, n, m); }
        case MsgType::WorldState: { WorldState m; return Decode(d, n, m); }
        case MsgType::Ping: case MsgType::Pong: { Ping m; bool p; return DecodePing(d, n, m, p); }
        case MsgType::Pose: { PoseMsg m; return Decode(d, n, m); }
        case MsgType::Vitals: { VitalsMsg m; return Decode(d, n, m); }
        case MsgType::HitClaim: { HitClaimMsg m; return Decode(d, n, m); }
        case MsgType::HitFx: { HitFxMsg m; return Decode(d, n, m); }
        case MsgType::SeverClaim: { combat::SeverClaimMsg m; return Decode(d, n, m); }
        case MsgType::StuckBlade: { combat::StuckBladeMsg m; return Decode(d, n, m); }
        case MsgType::GrabState: { GrabStateMsg m; return Decode(d, n, m); }
        case MsgType::PropState: { PropStateMsg m; return Decode(d, n, m); }
        case MsgType::PropLease: { PropLeaseMsg m; return Decode(d, n, m) && m.action == LeaseAction::Grant; }
        case MsgType::PaintFx: { combat::PaintFxMsg m; return Decode(d, n, m); }
        case MsgType::Equip: { combat::EquipStateMsg m; return Decode(d, n, m); }
        default: return false;
    }
}

// An actor the mod spawned, with its object-array slot so a stale pointer is never dereferenced.
struct ActorRef {
    UObject* obj = nullptr; int32_t index = -1;
    void Set(UObject* o) { obj = o; index = ObjectIndex(o); }
    void Clear() { obj = nullptr; index = -1; }
};

class SessionImpl final : public Session {
public:
    explicit SessionImpl(const Config& c) : cfg_(c) {
        lobby_.onEntered = [this] { OnLobbyEntered(); };
        lobby_.onLeft = [this] {
            bool strandedInFight = welcomed_ && InFightLevel();   // a client whose host left: back to the menu
            ResetNetwork("left lobby");
            if (strandedInFight) { HSM_LOG("client: lobby gone; returning to the main menu"); game::OpenLevel(lastGameMode_, kMenuLevel, true, ""); }
        };
        lobby_.onMemberJoined = [this](steam::SteamId id) { if (steamNet_ && lobby_.IsHost()) steamNet_->AllowPeer(id); };
        lobby_.onMemberLeft = [this](steam::SteamId id) { if (steamNet_) steamNet_->DropPeer(id); };
    }

    bool Start() {
        if (cfg_.role == Role::Steam) return true;   // transport is created when a lobby is entered
        auto enet = std::make_unique<ENetTransport>(cfg_.impairment);
        bool ok = cfg_.role == Role::Host ? enet->StartHost(cfg_.port, kMaxFighters - 1, ValidateForHost)
                                          : enet->StartClient(cfg_.hostAddress, cfg_.port, ValidateForClient);
        if (ok) net_ = std::move(enet);
        if (ok && cfg_.role == Role::Host) hostPending_ = true;
        return ok;
    }

    game::Lobby& lobby() override { return lobby_; }
    bool steamMode() const override { return cfg_.role == Role::Steam; }
    void StartOnMap(const std::string& path) override {
        if (!lastGameMode_ || (net_ && !IsHost())) return;
        HSM_LOG("session: starting on %s", path.c_str());
        // The level manager destroys every spawner whose "Required Combatants Amount" exceeds foes + 1 (decomp
        // BP_LevelManager.cpp:385), the player's included: keep at least 2 until our fighters exist (StopStockFoes).
        RememberFoes();
        adapter_.PrepareFreeModeFight(std::max(savedFoes_, 2));
        HostTravel(path);
    }

    // Before the Half Sword game mode ticks (splash/startup screens), any actor tick answers the bridge with the
    // commands that need no session state.
    void PumpAgentEarly() {
        uint32_t now = NowMs();
        if (now - lastModeTick_ < 1000 || now - lastEarlyPump_ < 50) return;
        lastEarlyPump_ = now;
        // Paused game (actors frozen, widgets still tick) in a level we already know: the full command set is safe.
        if (lastGameMode_ && ObjectAt(lastGameModeIndex_) == lastGameMode_ && adapter_.World() == lastGameMode_) {   // never reads a freed mode
            agent::Tick([this](const AgentCommand& c) { return HandleAgent(c); });
            return;
        }
        agent::Tick([this](const AgentCommand& c) {
            if (c.verb == "ping") return std::string("{\"ok\":true,\"early\":true}");
            if (c.verb == "widgets") return WidgetsJson();
            if (c.verb == "exec" || c.verb == "shot") { AgentCommand x = c; return HandleAgent(x); }
            return std::string("{\"ok\":false,\"error\":\"game mode not ticking yet; try widgets/key/click/exec\"}");
        });
    }

    std::string WidgetsJson() {
        std::string j = "{\"ok\":true,\"widgets\":[";
        bool first = true;
        UClass* uw = FindClass("UserWidget");
        for (int32_t i = 0, n = ObjectCount(); i < n && uw; ++i) {
            UObject* o = ObjectAt(i);
            if (!o || IsDefaultObject(o) || !IsA(o, uw)) continue;
            Call v(o, "IsInViewport");
            if (!v.invoke() || !v.get<bool>("ReturnValue")) continue;
            j += (first ? "" : ",") + JsonString(ObjectName(reinterpret_cast<UObject*>(ClassOf(o))));
            first = false;
        }
        return j + "]}";
    }

    void Tick(UObject* gameMode, float dt) {
        diagnostics::Heartbeat();
        diagnostics::Scope trace("Session::Tick");
        lastModeTick_ = NowMs();
        if (!net_ || !InFightLevel()) frameMs_.clear();                 // sampled only where the health line is logged
        else if (frameMs_.size() < kFrameSamples) frameMs_.push_back(double(dt) * 1000.0);
        if (!selfTested_) {
            selfTested_ = true;
            selfTestOk_ = SelfTest();
            test::Begin(selfTestOk_);
            if (!selfTestOk_) { HSM_LOG("session: self-test failed; disabling multiplayer for this run"); ResetNetwork("self-test failed"); return; }
            InstallClientSuppression();
            game::InstallTravelHook([this](const std::string& l, bool a, const std::string& o) { return OnOpenLevel(l, a, o); });
            // Blueprint-to-Blueprint calls (hits, damage) never reach ProcessEvent; this hooks the VM's executor.
            // Fails closed: on an unverified build the hooks stay inert and the log says why.
            HSM_LOG("session: script VM hook %s", InstallScriptHook() ? "installed" : "unavailable");
            GuardStockSpawns();
        }
        if (!selfTestOk_) return;
        if (steamMode()) lobby_.Tick();
        // Level change first: everything after this point must use the current world, never the old game mode.
        bool levelChanged = gameMode != lastGameMode_;
        if (!adapter_.Resolve(gameMode)) return;     // retried next tick, with the level change still pending
        if (levelChanged) { lastGameMode_ = gameMode; lastGameModeIndex_ = ObjectIndex(gameMode); OnLevelLoaded(); }
        inFight_ = InFightLevel();
        if (hostPending_) { hostPending_ = false; BecomeHost(); }
        agent::Tick([this](const AgentCommand& c) { return HandleAgent(c); });
        AutoRun();
        ui::Tick(*this, gameMode);
        EnsureLocalFighter();
        test::Tick(adapter_, InFightLevel(), dt);
        ApplyHeldInput();
        if (mirror_.Active() && net_) mirror_.Command({"off"}, adapter_, true, InFightLevel());   // offline bench only
        mirror_.Tick(adapter_, dt);
        bool joined = net_ && (steamMode() ? lobby_.InLobby() : true);
        adapter_.SetDestroyAware(net_ && OwnerMode());
        stockFlowGuard_ = joined && MatchLevel() && OwnerMode();
        adapter_.SuppressStockScreens(joined && MatchLevel(), OwnerMode());
        if (stockFlowGuard_ && adapter_.GamePaused()) {   // owner sync: a paused game stops this player's stream
            adapter_.SetGamePaused(false);
            if (++unpaused_ % 100 == 1) HSM_LOG("session: game was paused during an owner-sync match; unpaused (%u time(s))", unpaused_);
        }
        if (!net_) { ui::HudTick({}); return; }
        StockFoesTick(); // also while waiting for peers: cleanup must not wait for StartMatch
        if (IsHost()) HostTick(dt); else ClientTick(dt);
        TintTick();
        if (inputFixAt_ && NowMs() >= inputFixAt_) {   // twice: after the level's own setup, then once more
            adapter_.RestoreGameInput();
            HSM_LOG("session: game input mode restored");
            inputFixAt_ = inputFixRepeat_ ? 0 : NowMs() + 3000; inputFixRepeat_ = !inputFixRepeat_;
        }
        if (net_) SpectateTick();
        if (net_ && InFightLevel() && NowMs() >= nextHealth_) { nextHealth_ = NowMs() + (steamMode() ? 20000 : 5000); LogHealth(); }
        ui::HudModel hud = BuildHud();
        ui::HudTick(hud);
        ui::ChatTick(hud);
        if (std::optional<std::string> line = ui::TakeOutgoing(); line && net_) SendChat(*line);
    }

    // Health summary every 20 s on Steam (5 s on ENet); owner mode emits ten lines per summary.
    void LogHealth() {
        uint32_t now = NowMs();
        if (OwnerMode()) { LogOwnerHealth(); return; }
        if (IsHost()) {
            std::string line;
            for (const RosterMember& m : roster_.members()) {
                if (m.slot == 0) continue;
                const Fighter& f = fighters_[m.slot];
                line += " slot" + std::to_string(m.slot) + ": " + std::to_string(f.inputsRx) + " inputs, last " +
                        (f.inputsRx ? std::to_string(now - f.lastInputAt) + " ms ago" : "never") + (FighterActor(m.slot) ? ", fighter ok" : ", NO FIGHTER") + ";";
            }
            HSM_LOG("host: health%s bots %zu, match phase %d; blood sent %u applied %u, hits blocked %u", line.empty() ? " (no clients)" : line.c_str(),
                    bots_.size(), int(match_.phase()), bloodSent_, bloodApplied_, hitsBlocked_);
            bloodSent_ = bloodApplied_ = hitsBlocked_ = 0;
        } else {
            size_t alive = 0;
            for (auto& [e, ref] : puppets_) alive += adapter_.StillValid(ref.obj, ref.index);
            std::string desync;
            for (auto& [e, m] : desync_) {
                if (!m.n) continue;
                char b[96]; std::snprintf(b, sizeof b, " e%u%s avg %.0f max %.0f cm;", e, e == world_.myEntity() ? "(me)" : "", m.sum / m.n, m.max);
                desync += b;
            }
            desync_.clear();
            HSM_LOG("client: health %u snapshots, %u inputs sent, puppets %zu/%zu, own entity %u, own fighter %s; blood applied %u, hits blocked %u; desync%s",
                    snapsRx_, inputsTx_, alive, puppets_.size(), world_.myEntity(), adapter_.PlayerWillie(0) ? "ok" : "MISSING",
                    bloodApplied_, hitsBlocked_, desync.empty() ? " n/a" : desync.c_str());
            bloodSent_ = bloodApplied_ = hitsBlocked_ = 0;
        }
    }

    // Owner sync: what this machine streamed, how every stand-in tracks its owner, and every claim's fate.
    void LogOwnerHealth() {
        const char* who = IsHost() ? "host" : "client";
        HSM_LOG("%s: health (owner sync) streamed %llu poses %llu vitals; dropped %u foreign streams, rejected %u claims; stand-ins %d%s",
                who, static_cast<unsigned long long>(streams_.PosesSent()), static_cast<unsigned long long>(streams_.VitalsSent()),
                streamsDropped_, claimsRejected_, standIns_.Count(), IsHost() ? "" : (adapter_.PlayerWillie(0) ? ", own fighter ok" : ", own fighter MISSING"));
        streamsDropped_ = claimsRejected_ = 0;
        HSM_LOG("%s: %s", who, FrameAndPingLine().c_str());
        if (std::string line = standIns_.HealthLine(); !line.empty()) HSM_LOG("%s: %s", who, line.c_str());
        HSM_LOG("%s: %s", who, claims_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, fx_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, equip_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, severs_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, gore_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, grabs_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, props_.HealthLine().c_str());
    }

    // Frame time (the game mode's DeltaSeconds) since the previous call, and the round trip to every peer: a long frame
    // and a slow link both look like lag on the other screen; this line tells them apart.
    std::string FrameAndPingLine() {
        char b[96];
        std::string line;
        if (frameMs_.empty()) line = "frames none";
        else {
            std::vector<double> ms = frameMs_;
            double sum = 0;
            for (double v : ms) sum += v;
            const size_t k = std::min(ms.size() - 1, (ms.size() * 99) / 100);
            std::nth_element(ms.begin(), ms.begin() + std::ptrdiff_t(k), ms.end());
            std::snprintf(b, sizeof b, "frames %zu avg %.1f ms p99 %.1f ms", ms.size(), sum / double(ms.size()), ms[k]);
            line = b;
        }
        frameMs_.clear();
        line += " | rtt";
        size_t links = 0;
        auto add = [&](uint32_t peer, int slot) {
            std::snprintf(b, sizeof b, " slot%d %u ms", slot, net_->RoundTripMs(peer));
            line += b; ++links;
        };
        if (net_ && IsHost()) for (const auto& [peer, p] : peers_) add(peer, p.slot);
        else if (net_ && hostPeer_) add(hostPeer_, kHostSlot);
        if (!links) line += " none";
        return line;
    }

    // The host stamps the sender's real slot and relays every line; it shows its own lines directly.
    void SendChat(const std::string& text) {
        ChatLine c; c.fromSlot = 0; c.text = SanitizeChat(text);
        if (c.text.empty()) return;
        if (IsHost()) { net_->Broadcast(kChanReliable, Encode(c), true); ui::ShowLine(MyName(), c.text); }
        else if (hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(c), true);
    }

    // A stand-in (the mirror bench's or a session's) whose own Willie ReceiveTick must not run: it re-arms the muscles,
    // pain and damage the stand-in's neutralisation switched off.
    bool IsTickBlocked(const UObject* o) const { return o && (o == mirror_.TickBlocked() || standIns_.IsStandIn(o)); }

    std::string NameOfSlot(uint8_t slot) const {
        uint8_t e = Roster::EntityForSlot(slot);
        for (const FighterEntry& f : info_.fighters) if (f.entity == e && !f.name.empty()) return f.name;
        if (const RosterMember* m = roster_.BySlot(slot)) return m->name;
        return "Player " + std::to_string(slot + 1);
    }

private:
    bool IsHost() const { return steamMode() ? lobby_.IsHost() : cfg_.role == Role::Host; }
    bool OwnerMode() const { return syncMode_ == SyncMode::Owner; }
    int SnapshotHz() const { return std::max(cfg_.snapshotHz, 60); }   // older ini files say 30
    bool InFightLevel() const { return !currentLevel_.empty() && !IsMenuLevel(currentLevel_); }
    // Matches run in fight levels, not in the Tavern hub where players walk around between fights.
    bool MatchLevel() const { return InFightLevel() && currentLevel_.find("Hub") == std::string::npos; }
    std::string MyName() const { return steamMode() ? steam::PersonaName(steam::MySteamId()) : cfg_.playerName; }

    // ------------------------------------------------------------------ agent bridge (sensors/actuators)
    static constexpr const char* kAxisNames[kAxisCount] = {"MoveForward", "MoveRight", "TurnMouse", "LookMouse", "TurnGamepad",
                                                         "LookGamepad", "RightArm", "LeftArm", "RightGuard", "LeftGuard"};
    static constexpr const char* kActionNames[kActionCount] = {"GrabLeft", "GrabRight", "GuardLeft", "GuardRight", "Thrust", "ThrustGamepad",
                                                             "Jump", "CrouchKey", "CrouchHold", "Run", "SwapHands", "ToggleCameraLock",
                                                             "KeyForward", "KeyBack", "KeyLeft", "KeyRight", "Talk", "RightMouse", "MiddleMouse"};
    struct Held { float value = 0; uint32_t until = 0; };

    static std::string Vec3(const FVector& v) { char b[96]; std::snprintf(b, sizeof b, "[%.0f,%.0f,%.0f]", v.x, v.y, v.z); return b; }

    std::string FighterJson(uint8_t entity, UObject* w) {
        if (!w) return "{\"entity\":" + std::to_string(entity) + ",\"actor\":false}";
        double* hp = Field<double>(w, "Health");
        // Raw reflected vitals, not an invented aggregate HP score. Missing fields remain distinguishable from zero.
        auto number = [&](const char* name) {
            const double* value = Field<double>(w, name);
            return value && std::isfinite(*value) ? std::to_string(*value) : std::string("null");
        };
        std::string condition = "{\"consciousness\":" + number("Consciousness") +
            ",\"consciousness_legs\":" + number("Consciousness 2 (Legs)") +
            ",\"bleeding\":" + number("Bleeding") + ",\"blood_rate\":" + number("Blood Rate") +
            ",\"part_health\":{\"head\":" + number("Head Health") + ",\"neck\":" + number("Neck Health") +
            ",\"upper_body\":" + number("Body Upper Health") + ",\"lower_body\":" + number("Body Lower Health") +
            ",\"back\":" + number("Back Health") + ",\"right_arm\":" + number("Arm_R Health") +
            ",\"left_arm\":" + number("Arm_L Health") + ",\"right_leg\":" + number("Leg_R Health") +
            ",\"left_leg\":" + number("Leg_L Health") + "}}";
        return "{\"entity\":" + std::to_string(entity) + ",\"pos\":" + Vec3(adapter_.ActorLocation(w)) + ",\"dead\":" +
               (adapter_.IsDead(w) ? "true" : "false") + ",\"health\":" + std::to_string(hp ? int(*hp) : -1) +
               ",\"condition\":" + condition + "}";
    }

    std::string StateJson() {
        std::string j = "{\"ok\":true,\"role\":" + JsonString(net_ ? (IsHost() ? "host" : "client") : "solo") + ",\"level\":" + JsonString(currentLevel_) +
                        ",\"fight\":" + (InFightLevel() ? "true" : "false") + ",\"match_level\":" + (MatchLevel() ? "true" : "false") +
                        ",\"paused\":" + (adapter_.GamePaused() ? "true" : "false") + ",\"mode_ticking\":" + (NowMs() - lastModeTick_ < 1000 ? "true" : "false");
        j += ",\"match\":{\"phase\":" + std::to_string(info_.phase) + ",\"format\":" + std::to_string(info_.format) + ",\"status\":" +
             JsonString(StatusLine(info_, IsHost() ? 1 : world_.myEntity())) + "}";
        j += ",\"local\":" + FighterJson(IsHost() ? 1 : world_.myEntity(), InFightLevel() ? adapter_.PlayerWillie(0) : nullptr);
        j += ",\"fighters\":[";
        bool first = true;
        auto add = [&](uint8_t e, UObject* w) { j += (first ? "" : ",") + FighterJson(e, w); first = false; };
        if (IsHost()) {
            for (const RosterMember& m : roster_.members()) add(m.entity, EntityActor(m.entity));
            for (auto& [e, ref] : bots_) add(e, EntityActor(e));
        } else {
            for (auto& [e, ref] : puppets_) add(e, adapter_.StillValid(ref.obj, ref.index) ? ref.obj : nullptr);
        }
        j += "],\"peers\":" + std::to_string(peers_.size()) + ",\"welcomed\":" + (welcomed_ ? "true" : "false") +
             ",\"roster\":" + std::to_string(roster_.size()) + ",\"willies_in_level\":" + std::to_string(InFightLevel() ? adapter_.AllWillies().size() : 0) + "}";
        return j;
    }

    std::string HandleAgent(const AgentCommand& c) {
        auto ok = [](const std::string& extra = "") { return "{\"ok\":true" + extra + "}"; };
        auto err = [](const std::string& why) { return "{\"ok\":false,\"error\":" + JsonString(why) + "}"; };
        if (c.verb == "ping") return ok();
        if (c.verb == "state") return StateJson();
        if (c.verb == "open" && c.args.size() == 1) {
            if (!IsHost() && net_) return err("clients follow the host");
            adapter_.PrepareFreeModeFight();
            HostTravel(c.args[0]);
            return ok();
        }
        if (c.verb == "axis" && c.args.size() == 3) {
            for (size_t i = 0; i < kAxisCount; ++i) if (c.args[0] == kAxisNames[i]) {
                float v = ParseArgFloat(c.args[1], -kAxisLimit, kAxisLimit), ms = ParseArgFloat(c.args[2], 0, 60000);
                if (std::isnan(v) || std::isnan(ms)) return err("bad value or duration");
                held_[i] = {v, NowMs() + uint32_t(ms)};
                return ok();
            }
            return err("unknown axis");
        }
        if (c.verb == "action" && c.args.size() == 2) {
            UObject* me = InFightLevel() ? adapter_.PlayerWillie(0) : nullptr;
            if (!me || !adapter_.BindInputs(me)) return err("no local fighter");
            for (size_t i = 0; i < kActionCount; ++i) if (c.args[0] == kActionNames[i]) {
                bool tap = c.args[1] == "tap";
                if (c.args[1] == "down" || tap) adapter_.FireAction(me, Action(i), true);
                if (c.args[1] == "up" || tap) adapter_.FireAction(me, Action(i), false);
                return ok();
            }
            return err("unknown action");
        }
        if (c.verb == "quit") { quitRequested_ = true; return ok(); }
        if (c.verb == "widgets") return WidgetsJson();
        if (c.verb == "pause" && c.args.size() == 1) { adapter_.SetGamePaused(c.args[0] == "on"); return ok(); }
        if (c.verb == "vmhook") return agent::VmHookCommand(c.args, [this] { return InFightLevel() ? adapter_.PlayerWillie(0) : nullptr; });
        if (c.verb == "mirror") return mirror_.Command(c.args, adapter_, net_ != nullptr, InFightLevel());
        if (c.verb == "shot") { adapter_.ConsoleCommand("HighResShot 1"); return ok(",\"note\":\"saved under Saved/Screenshots\""); }
        if (c.verb == "exec" && !c.args.empty()) {
            std::string cmd;
            for (const std::string& a : c.args) cmd += (cmd.empty() ? "" : " ") + a;
            adapter_.ConsoleCommand(cmd);
            return ok();
        }
        return err("unknown command (ping, state, open <level>, axis <name> <value> <ms>, action <name> down|up|tap, shot, exec <console command>, vmhook stats|hits|log|block|off, mirror on <delayMs> <jitterMs> <lossPct> [offsetUu] [keeptick]|stats|off, quit)");
    }

    // Held axes are re-applied every frame like a real device, through Willie's own input events.
    void ApplyHeldInput() {
        UObject* me = InFightLevel() ? adapter_.PlayerWillie(0) : nullptr;
        if (!me || !adapter_.BindInputs(me)) return;
        uint32_t now = NowMs();
        for (size_t i = 0; i < kAxisCount; ++i)
            if (held_[i].until) {
                bool live = int32_t(held_[i].until - now) > 0;
                adapter_.ApplyAxis(me, Axis(i), live ? held_[i].value : 0.f);
                if (!live) held_[i] = Held{};
            }
    }

    // In a session (or an automated -hsmauto/-hsmtest run) the mod does not wait forever for the stock level flow to
    // produce a player: a fight level that still has no local pawn after the grace period gets one from the mod (own
    // loadout, possessed by player 0). Clients take the spot the host announced; host/solo use the level's spawn
    // points. Plain single-player play is never touched (no pawn is normal there: death screens, transitions).
    void EnsureLocalFighter() {
        if (!(net_ || automated_) || !InFightLevel() || adapter_.PlayerWillie(0)) { noPawnSince_ = 0; return; }
        uint32_t now = NowMs();
        if (!noPawnSince_) { noPawnSince_ = now; return; }
        if (now - noPawnSince_ < kLocalFighterGraceMs) return;
        noPawnSince_ = now;                                   // retry spacing if the spawn fails
        game::Adapter::SpawnOptions o; o.teamInt = 1;
        std::vector<uint8_t> own;
        bool client = net_ && !IsHost();
        if (client) {
            const Spawn* sp = world_.Find(world_.myEntity());
            if (!sp) return;                                  // wait for the host's announcement
            o.at = {sp->x, sp->y, sp->z}; o.yawDeg = sp->yaw;
            o.blob = sp->blobId ? clientBlobs_.AtLeast(sp->blobId) : nullptr;
            if (!o.blob && !lastUpload_.empty()) o.blob = &lastUpload_;
        } else {
            std::vector<FVector> pts = adapter_.SpawnCandidates();
            o.at = pts.empty() ? FVector{} : pts.front();
            if (adapter_.ExportPassport(own)) o.blob = &own;
        }
        HSM_LOG("session: no player pawn in %s after %u ms; spawning the local fighter", currentLevel_.c_str(), kLocalFighterGraceMs);
        adapter_.RespawnLocalPlayer(o);
    }

    // -hsmauto=<level>: a host/solo instance loads that Free Mode level by itself; -hsmquit=<s>: quit after s seconds.
    void AutoRun() {
        uint32_t now = NowMs();
        if (!startedAt_) {
            startedAt_ = now;
            std::string cl = CommandLineUtf8();
            autoLevel_ = CmdArg(cl, "hsmauto").value_or("");
            automated_ = !autoLevel_.empty() || CmdArg(cl, "hsmtest").has_value();
            quitAfterMs_ = uint32_t(CmdArgInt(cl, "hsmquit", 0, 10, 86400)) * 1000u;
        }
        if (quitRequested_ || (quitAfterMs_ && now - startedAt_ >= quitAfterMs_)) {
            HSM_LOG("session: quitting (%s)", quitRequested_ ? "agent request" : "-hsmquit timer");
            quitRequested_ = false; quitAfterMs_ = 0;
            adapter_.QuitGame();
            return;
        }
        bool mayTravel = !net_ || IsHost();
        if (!autoLevel_.empty() && mayTravel && lastGameMode_ && !InFightLevel() && now - startedAt_ > 8000 && now >= nextAuto_) {
            nextAuto_ = now + 15000;
            HSM_LOG("session: -hsmauto opening %s", autoLevel_.c_str());
            adapter_.PrepareFreeModeFight();
            HostTravel(autoLevel_);
        }
    }

    // ------------------------------------------------------------------ lobby / transport lifecycle
    void OnLobbyEntered() {
        HSM_LOG("session: Steam start lobby=%llu owner=%llu members=%zu role=%s", (unsigned long long)lobby_.id(),
                (unsigned long long)lobby_.owner(), lobby_.members().size(), lobby_.IsHost() ? "host" : "client");
        auto st = std::make_unique<SteamTransport>(lobby_.IsHost() ? ValidateForHost : ValidateForClient);
        steamNet_ = st.get();
        net_ = std::move(st);
        if (lobby_.IsHost()) {
            for (const game::LobbyMember& m : lobby_.members()) if (m.id != steam::MySteamId()) steamNet_->AllowPeer(m.id);
            hostPending_ = true;
        } else steamNet_->AllowPeer(lobby_.owner());
    }

    void ResetNetwork(const char* why) {
        if (net_) { HSM_LOG("session: network reset (%s)", why); net_->Stop(); }
        steamNet_ = nullptr; net_.reset();
        claims_.Stop();                              // the hooks go before any fighter does
        fx_.Stop();                                  // with its paint boxes on the stand-ins
        equip_.Stop();
        ClearScriptHooks(kStockFlowOwner);
        grabs_.Stop();                               // unmarks the fighters it held, so before they go too
        props_.Stop();
        severs_.Stop();
        gore_.Stop();
        // host
        for (uint8_t slot = 1; slot < kMaxFighters; ++slot) DespawnFighter(slot);
        roster_.Clear(); director_ = HostDirector{}; hostBlobs_.Clear(); peers_.clear(); blobVersion_.fill(0); hostExport_.clear();
        picksValid_ = false; hostPending_ = false; travelSeq_ = 0;
        for (auto& [e, ref] : bots_) if (adapter_.StillValid(ref.obj, ref.index)) adapter_.DestroyActor(ref.obj);
        bots_.clear(); match_ = Match{}; kills_ = KillLog{}; info_ = MatchInfo{}; feed_.clear(); tints_.clear(); ownDeadPending_ = false;
        // client
        for (auto& [e, ref] : puppets_) {
            if (OwnerMode()) adapter_.DestroyFighter(ref.obj, ref.index);   // a stand-in: its weapons go too
            else if (adapter_.StillValid(ref.obj, ref.index)) adapter_.DestroyActor(ref.obj);
        }
        puppets_.clear(); world_ = ClientWorld{}; clientBlobs_.Clear(); clientAssembler_ = BlobAssembler{}; lastUpload_.clear();
        hostPeer_ = 0; welcomed_ = false; haveSnapshot_ = false; snappedOwn_ = false; hellosSent_ = 0;
        reservations_.Clear(); jitter_.clear(); placed_.clear();
        RestoreFoes();
        myWillie_.store(nullptr);                    // the permanent input hooks must stop swallowing this pawn's look
        held_.fill(Held{});
        hostLoss_ = HostLossMonitor(NowMs());
        // owner sync
        standIns_.Clear(); grabs_.Clear(); streams_.Clear(); lives_.clear();
        syncMode_ = SyncMode::Host; ownDeathReported_ = false; nextKeepAlive_ = 0; streamsDropped_ = claimsRejected_ = 0;
    }

    // ------------------------------------------------------------------ travel / levels
    // Our own OpenLevel bypasses the travel hook, so a level the host opens itself (Start match, -hsmauto, the bridge,
    // Progression back to the hub) must be announced to the lobby here, after the GI settings for it are set.
    void HostTravel(const std::string& level) {
        OnOpenLevel(level, true, "");
        game::OpenLevel(lastGameMode_, level, true, "");
    }

    bool OnOpenLevel(const std::string& level, bool absolute, const std::string& options) {
        bool joined = steamMode() ? lobby_.InLobby() : net_ != nullptr;
        if (!joined || IsHost()) {                     // remembered even before hosting, for late joiners
            Travel t; t.level = level; t.absolute = absolute; t.options = options; t.settings = game::CaptureGameSettings();
            lastTravel_ = t;
        }
        if (!joined) return true;
        if (IsHost()) {
            Travel& t = *lastTravel_; t.seq = ++travelSeq_;
            if (net_) net_->Broadcast(kChanReliable, Encode(t), true);
            lobby_.SetPublicState(IsMenuLevel(level) ? "menu" : "playing", level.substr(level.find_last_of('/') + 1), "");
            HSM_LOG("host: travel #%u to %s (%zu settings) sent to lobby", t.seq, level.c_str(), t.settings.size());
            return true;
        }
        if (IsMenuLevel(level)) return true;           // clients can always go back to the menu
        HSM_LOG("client: in a lobby - waiting for the host to start the fight (leave the lobby to play solo)");
        return false;
    }

    void OnLevelLoaded() {
        currentLevel_ = ObjectName(OuterOf(OuterOf(lastGameMode_)));   // GameMode -> PersistentLevel -> World
        HSM_LOG("session: level loaded (%s)", currentLevel_.c_str());
        agent::VmHookFlush();
        mirror_.OnLevel();
        if (ScriptHookActive()) ResolveScriptHooks();   // the level's Blueprint classes and overrides are loaded now
        haveSnapshot_ = false; snappedOwn_ = false; picksValid_ = false;
        held_.fill(Held{});                          // agent-held axes do not carry into the next level
        groundValid_ = false; foesStopped_ = false;
        if (!InFightLevel()) RestoreFoes();
        inputFixAt_ = net_ && InFightLevel() ? NowMs() + 1500 : 0;
        severedSent_.clear(); worldTracker_ = WorldTracker{}; worldView_ = WorldView{}; jitter_.clear(); placed_.clear();
        // Host side: every actor of the old level is gone.
        for (Fighter& f : fighters_) { f.actor.Clear(); f.input.ReleaseAll(); f.wasDead = false; }
        director_.OnLevel(InFightLevel(), NowMs(), travelSeq_);
        bots_.clear(); match_ = Match{}; kills_ = KillLog{}; sidesValid_ = false; wave_ = 0; info_ = MatchInfo{}; feed_.clear(); tints_.clear();
        ownDeadPending_ = false; ownDiedAt_ = 0; nextOwnRespawn_ = 0;
        // Client side: puppets died with the level; the host re-announces them after Ready.
        world_.OnLevel(); puppets_.clear();
        levelGate_.OnLevel(game::SteadyNowMs()); nextGateProbe_ = 0; recreate_.Clear(); lostSlots_.clear();
        // Owner sync, client: the stock pawn the level gives player 0 is replaced by the dressed fighter the host
        // announces, once the level's own start-up cleanup is over (ClientGateOpen).
        if (net_ && !IsHost() && OwnerMode() && InFightLevel()) ownDeadPending_ = true;
        // Owner sync: every stand-in died with the level (the feeds stay: their owners keep streaming).
        for (uint8_t e = 1; e <= kMaxWillies; ++e) { standIns_.Detach(e); severs_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); }
        gore_.Clear();
        grabs_.Clear();
        props_.Clear();                              // every prop actor died with the level
        lives_.clear(); ownDeathReported_ = false;
        if (!net_) return;
        if (IsHost()) { ShareHostLoadout(); return; }
        if (welcomed_) UploadLoadout();             // before Ready, so the host never spawns us in a stale loadout
        if (travelSeq_ && hostPeer_) { Ready r; r.travelSeq = travelSeq_; r.level = pendingLevel_; net_->Send(hostPeer_, kChanReliable, Encode(r), true); }
    }

    // ------------------------------------------------------------------ host
    struct Fighter { ActorRef actor; InputApplier input; bool wasDead = false; uint32_t inputsRx = 0, lastInputAt = 0; };
    struct Peer { uint8_t slot = 0; uint64_t key = 0; BlobAssembler blobs; uint32_t helloAt = 0; ChatRateLimiter chat; };
    static constexpr uint32_t kHelloRepeatMs = 5000;

    void BecomeHost() {
        roster_.Clear(); roster_.Add(kHostKey, MyName());
        director_ = HostDirector{}; director_.OnJoin(0, NowMs()); director_.OnLevel(InFightLevel(), NowMs(), 0);
        hostExport_.clear(); ShareHostLoadout();
        // Owner sync needs the script VM hook (claims); without it the host falls back to simulating everyone.
        syncMode_ = cfg_.ownerSync && ScriptHookActive() ? SyncMode::Owner : SyncMode::Host;
        if (cfg_.ownerSync && !OwnerMode()) HSM_LOG("host: owner sync needs the script VM hook, which is unavailable; using host sync");
        HSM_LOG("host: hosting as slot 0 (up to %d fighters), %s sync", kMaxFighters, OwnerMode() ? "owner" : "host");
        if (OwnerMode()) StartOwner();
    }

    bool CombatEnabled() const {
        return !MatchLevel() || CombatPhase(IsHost() ? match_.phase() : MatchPhase(info_.phase));
    }

    // Owner sync on this machine: a fresh stream clock, and the claims hooks with what they need to know of the session.
    void StartOwner() {
        streams_.Start(game::SteadyNowMs());
        GuardStockFlow();
        game::Claims::Env env;
        env.combatEnabled = [this] { return CombatEnabled(); };
        env.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        env.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        env.send = [this](const combat::HitClaim& c) {
            if (IsHost()) RouteHostClaim(kHostSlot, c);
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(HitClaimMsg{c}), true);
        };
        env.allowed = [this](uint8_t attacker, uint8_t victim) {
            return HitAllowed(true, KnownTeam(attacker), KnownTeam(victim), IsHost() ? match_.config().friendlyFire : info_.friendlyFire);
        };
        env.opposing = [this](uint8_t attacker, uint8_t victim) { return Opposing(attacker, victim); };
        env.damaged = [this](uint8_t victim, uint8_t attacker) {
            if (IsHost()) kills_.OnDamage(victim, attacker, NowMs(), 1, Opposing(attacker, victim));
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(DamageCredit{victim, attacker}), true);
        };
        if (!claims_.Start(adapter_, standIns_, streams_, std::move(env)))
            HSM_LOG("session: WARNING owner sync without the script VM hook: blows between this machine and stand-ins do nothing");
        // Blood and wounds on fighters simulated here, shown on every other machine's stand-in of them.
        game::FxSync::Env fenv;
        fenv.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        fenv.send = [this](const combat::PaintFxMsg& m) {
            if (!net_) return;
            if (IsHost()) net_->Broadcast(kChanReliable, Encode(m), true);
            else if (hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(m), true);
        };
        fx_.Start(adapter_, standIns_, std::move(fenv));
        // Weapons drawn and sheathed by fighters simulated here, matched by every other machine's stand-in of them.
        game::EquipSync::Env eenv;
        eenv.send = [this](const combat::EquipStateMsg& m) {
            if (!net_) return;
            if (IsHost()) net_->Broadcast(kChanReliable, Encode(m), true);
            else if (hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(m), true);
        };
        equip_.Start(adapter_, standIns_, std::move(eenv));
        game::Severs::Env sev;
        sev.combatEnabled = [this] { return CombatEnabled(); };
        sev.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        sev.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        sev.sendSever = [this](const combat::SeverClaim& c) {
            if (IsHost()) RouteHostSever(kHostSlot, c);
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(combat::SeverClaimMsg{c}), true);
        };
        sev.sendStuck = [this](const combat::StuckBladeMsg& m) {
            if (IsHost()) RouteHostStuck(kHostSlot, m);
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(m), true);
        };
        sev.allowed = [this](uint8_t attacker, uint8_t victim) {
            return HitAllowed(true, KnownTeam(attacker), KnownTeam(victim), IsHost() ? match_.config().friendlyFire : info_.friendlyFire);
        };
        sev.nativeShown = [this](uint8_t e, uint8_t part) { gore_.NoteNativeSever(e, part); };
        severs_.Start(adapter_, standIns_, streams_, std::move(sev));
        game::StandInGore::Env gore;
        gore.standInActor = [this](uint8_t e) { return standIns_.ActorOf(e); };
        gore_.Start(adapter_, std::move(gore));
        standIns_.SetDeadBones([this](uint8_t e) { return gore_.DeadBones(e); });
        standIns_.SetHeldWeapons([this](uint8_t e) { return severs_.HeldWeapons(e); });   // its blades held in my fighter
        game::GrabSync::Env genv;
        genv.standInEntity = [this](const UObject* w) { return standIns_.EntityOf(w); };
        genv.standInActor = [this](uint8_t e) { return standIns_.ActorOf(e); };
        genv.dead = [this](uint8_t e) { const VitalsMsg* v = standIns_.Vitals(e); return v && (v->flags & 1); };
        genv.send = [this](const std::vector<uint8_t>& b, bool reliable) {
            const uint8_t chan = reliable ? kChanReliable : kChanUnreliable;
            if (!net_) return;
            if (IsHost()) net_->Broadcast(chan, b, reliable);
            else if (hostPeer_) net_->Send(hostPeer_, chan, b, reliable);
        };
        grabs_.Start(adapter_, std::move(genv));
        game::PropSync::Env penv;
        penv.host = IsHost();
        penv.mySlot = [this] { return IsHost() ? kHostSlot : mySlot_; };
        penv.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        penv.send = [this](const std::vector<uint8_t>& b, bool reliable) {
            if (!net_) return;
            const auto chan = reliable ? kChanReliable : kChanUnreliable;
            if (IsHost()) net_->Broadcast(chan, b, reliable);
            else if (hostPeer_) net_->Send(hostPeer_, chan, b, reliable);
        };
        props_.Start(adapter_, standIns_, std::move(penv));
    }

    // Owner sync: the game's single-player win/lose flow must not run in a session match. "Event Lose Match" on a
    // player fighter (ours, every stand-in and every mod-spawned fighter have Player set) opens UI_Lose, which pauses
    // the game and leads to "return to tavern / another fight"; "Force Finish Match" opens UI_NextFIght; "Call Minus
    // Enemy" counts a dead non-player down to the stock win screen. The session's match referee decides instead.
    // Non-player fighters (host bots) still run Event Lose Match for its "Give Up" AI state; its Call Minus Enemy is
    // blocked. Active only while stockFlowGuard_ (owner sync, in a match level, joined); host sync never registers it.
    static constexpr const char* kStockFlowOwner = "stockflow";
    void GuardStockFlow() {
        ClearScriptHooks(kStockFlowOwner);
        auto block = [this](const char* what) {
            return [this, what](ScriptCall& c) {
                if (!stockFlowGuard_) return false;
                if (std::strcmp(what, "Event Lose Match") == 0) {
                    bool player = false;
                    if (!GetBool(c.self(), "Player", player) || !player) return false;
                }
                if (++stockFlowBlocked_ % 50 == 1) HSM_LOG("session: blocked stock \"%s\" during an owner-sync match (%u so far)", what, stockFlowBlocked_);
                return true;
            };
        };
        // Literal names, one call each: tools/check_reflection.py verifies HookScript names against the game.
        int n = HookScript(kStockFlowOwner, "Willie_BP_C", "Event Lose Match", block("Event Lose Match"));
        n += HookScript(kStockFlowOwner, "Willie_BP_C", "Force Finish Match", block("Force Finish Match"));
        n += HookScript(kStockFlowOwner, "Willie_BP_C", "Call Minus Enemy", block("Call Minus Enemy"));
        HSM_LOG("session: stock win/lose flow guard %s (%d function(s) hooked now)", ScriptHookActive() ? "on" : "NOT ACTIVE (no script VM hook)", n);
    }

    // The entity of a fighter this machine simulates (owner sync): its own fighter, and on the host its bots.
    uint8_t OwnedEntityOf(const UObject* w) {
        if (!w) return 0;
        if (w == myWillie_.load()) return IsHost() ? uint8_t(1) : world_.myEntity();
        if (IsHost())
            for (auto& [e, ref] : bots_) if (ref.obj == w && adapter_.StillValid(ref.obj, ref.index)) return e;
        return 0;
    }
    UObject* OwnedActorOf(uint8_t e) {
        if (!e) return nullptr;
        if (e == (IsHost() ? uint8_t(1) : world_.myEntity())) return myWillie_.load();
        return IsHost() && e >= kFirstBotEntity ? EntityActor(e) : nullptr;
    }

    void HostTick(float dt) {
        myWillie_.store(inFight_ ? adapter_.PlayerWillie(0) : nullptr);   // owner-authority checks read this, not the engine
        NetEvent ev;
        while (net_->Poll(ev)) {
            if (ev.kind == NetEvent::Connected) HSM_LOG("host: peer %u connected", ev.peer);
            else if (ev.kind == NetEvent::Disconnected) OnClientLeft(ev.peer);
            else HostReceive(ev);
        }
        for (uint8_t slot : reservations_.Expire(NowMs())) {
            bool back = std::any_of(peers_.begin(), peers_.end(), [&](const auto& kv) { return kv.second.slot == slot; });
            if (const RosterMember* m = roster_.BySlot(slot); m && !back) { HSM_LOG("host: slot %u did not rejoin in time", slot); RemoveMember(slot, m->key); }
        }
        if (director_.fight()) SpawnDue();
        if (!OwnerMode()) for (uint8_t slot = 1; slot < kMaxFighters; ++slot) DriveRemoteFighter(slot, dt);
        HostMatchTick();
        if (OwnerMode()) { OwnerHostTick(dt); return; }
        sendAccum_ += dt;
        if (peers_.empty() || sendAccum_ < 1.f / float(SnapshotHz())) return;
        sendAccum_ = 0;
        Snapshot snap; snap.tick = ++tick_;
        for (uint8_t slot = 0; slot < kMaxFighters; ++slot) {
            UObject* w = FighterActor(slot);
            WillieState st;
            if (!w || !adapter_.ReadState(w, Roster::EntityForSlot(slot), st)) continue;
            bool dead = st.flags & 1;
            if (dead && !fighters_[slot].wasDead) SendEvent(EventKind::Died, st.entity, 0);
            fighters_[slot].wasDead = dead;
            snap.willies.push_back(std::move(st));
        }
        for (auto& [e, ref] : bots_) {
            WillieState st;
            if (adapter_.StillValid(ref.obj, ref.index) && adapter_.ReadState(ref.obj, e, st)) snap.willies.push_back(std::move(st));
        }
        if (!snap.willies.empty()) net_->Broadcast(kChanUnreliable, Encode(snap), false);
        SeveredMasks now;
        for (const WillieState& st : snap.willies)
            if (UObject* w = EntityActor(st.entity)) if (uint32_t m = game::ReadSeveredMask(w)) now[st.entity] = m;
        for (CombatEvent& e : DismemberDiff(severedSent_, now)) net_->Broadcast(kChanReliable, Encode(e), true);
        if (++worldEvery_ % 3 == 0) {                // loose weapons at a third of the snapshot rate: the scan walks every actor
            std::vector<WorldPos> players;
            for (const WillieState& st : snap.willies) if (!st.bodies.empty()) players.push_back({st.bodies[0].px, st.bodies[0].py, st.bodies[0].pz});
            WorldState ws = worldTracker_.Update(tick_, game::CollectLooseWeapons(adapter_, [this](UObject* w) { return EntityOf(w); }), players);
            if (!ws.objects.empty()) net_->Broadcast(kChanUnreliable, Encode(ws), false);
        }
    }

    // Owner sync, host side: stream its own fighter and its bots, drive every client's stand-in, keep the line warm.
    // Severs, stuck blades and gore (phase 4a) ride on claims and vitals; grabs and dropped weapons (phase 4b) ride on
    // GrabState and PropSync's leases; level props stay local.
    void OwnerHostTick(float dt) {
        const double now = game::SteadyNowMs();
        std::vector<std::pair<uint8_t, UObject*>> owned;
        if (UObject* me = myWillie_.load()) owned.push_back({uint8_t(1), me});
        for (auto& [e, ref] : bots_) if (adapter_.StillValid(ref.obj, ref.index)) owned.push_back({e, ref.obj});
        streams_.Tick(adapter_, now, dt, owned, [this](const std::vector<uint8_t>& b) { net_->Broadcast(kChanUnreliable, b, false); });
        standIns_.Tick(adapter_, now, dt, OwnedPelvises(owned));
        RecoverLostStandIns(now);
        claims_.Tick();
        severs_.Tick(now, owned);
        gore_.Tick(now);
        grabs_.Tick(now, dt, owned);
        props_.Tick(now, owned);
        equip_.Tick(now, owned);
        if (!peers_.empty() && NowMs() >= nextKeepAlive_) {
            nextKeepAlive_ = NowMs() + kKeepAliveMs;
            net_->Broadcast(kChanUnreliable, EncodePing(NowMs(), false), false);
        }
    }

    // Owner sync, host: a client's stand-in the game destroyed while the client is still live is spawned again where it
    // was announced (at most every RecreateLimiter::kRetryMs); the servo then snaps it onto the client's stream.
    void RecoverLostStandIns(double now) {
        if (!InFightLevel()) return;
        for (uint8_t slot = 1; slot < kMaxFighters; ++slot) if (fighters_[slot].actor.obj) FighterActor(slot);   // records losses
        for (auto it = lostSlots_.begin(); it != lostSlots_.end();) {
            const uint8_t slot = *it, e = Roster::EntityForSlot(slot);
            const Spawn* sp = nullptr;
            std::vector<Spawn> live = director_.LiveSpawns();
            for (const Spawn& s : live) if (s.entity == e) sp = &s;
            if (!roster_.BySlot(slot) || !sp || fighters_[slot].actor.obj) { it = lostSlots_.erase(it); continue; }
            if (!recreate_.Allow(e, now)) { ++it; continue; }
            game::Adapter::SpawnOptions o; o.blob = sp->blobId ? hostBlobs_.Get(sp->blobId) : nullptr;
            o.at = {sp->x, sp->y, sp->z}; o.yawDeg = sp->yaw; o.teamInt = TeamIntFor(sp->team, e); o.brain = false;
            UObject* w = adapter_.SpawnFighter(o);
            if (!w) { HSM_LOG("host: re-creating entity %u's stand-in failed; retrying", e); ++it; continue; }
            fighters_[slot].actor.Set(w); fighters_[slot].wasDead = false;
            standIns_.Detach(e); standIns_.Attach(e, w);
            QueueTint(w, sp->team);
            HSM_LOG("host: entity %u's stand-in was destroyed by the game; re-created", e);
            it = lostSlots_.erase(it);
        }
    }

    // Where the fighters simulated here stand: a stand-in is never snapped onto one of them.
    std::vector<sync::V3> OwnedPelvises(const std::vector<std::pair<uint8_t, UObject*>>& owned) const {
        std::vector<sync::V3> out;
        for (const auto& [e, w] : owned) if (sync::V3 p; streams_.PelvisOf(e, p)) out.push_back(p);
        return out;
    }

    std::vector<uint8_t> ConnectedSlots() const {
        std::vector<uint8_t> out{kHostSlot};
        for (const auto& [peer, p] : peers_) out.push_back(p.slot);
        return out;
    }
    std::optional<uint32_t> PeerOfSlot(uint8_t slot) const {
        for (const auto& [peer, p] : peers_) if (p.slot == slot) return peer;
        return std::nullopt;
    }

    // A Pose or Vitals from a client: kept only for an entity that client owns, applied to its stand-in here and relayed
    // as-is to every other client.
    void HostOwnerStream(const NetEvent& ev, const Peer& peer, MsgType t) {
        const uint8_t* d = ev.data.data(); size_t n = ev.data.size();
        if (t == MsgType::Pose) {
            PoseMsg m; sync::PoseFrame frame;
            if (!Decode(d, n, m, &frame) || !MayStream(peer.slot, m.entity)) { ++streamsDropped_; return; }
            standIns_.OnPose(m, frame, ArrivalMs(ev));
        } else {
            VitalsMsg v;
            if (!Decode(d, n, v) || !MayStream(peer.slot, v.entity)) { ++streamsDropped_; return; }
            if (!standIns_.OnVitals(v)) return;
            gore_.OnVitals(v.entity, v.severed, v.dislocated, game::SteadyNowMs());
            severs_.OnVitals(v, game::SteadyNowMs());
            NoteVitals(v);
        }
        for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
            if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanUnreliable, ev.data, false);
    }

    // The life of a client-owned fighter as its owner's vitals tell it: the host's fallback when a DeathReport is missing.
    void NoteVitals(const VitalsMsg& v) {
        OwnerLife& l = lives_[v.entity];
        if (!(v.flags & 1)) { l.aliveSeen = true; l.deadSince = 0; }
        else if (l.aliveSeen && !l.deadSince) l.deadSince = std::max(NowMs(), 1u);
    }

    // A claim made on the host (senderSlot 0) or received from a client: checked against who owns what and the
    // friendly-fire rule, then replayed by the victim's owner and shown to everyone else.
    void RouteHostClaim(uint8_t senderSlot, const combat::HitClaim& c) {
        const ClaimRoute r = RouteClaim(senderSlot, c.attacker, c.victim, ConnectedSlots());
        if (!r.accept || !HitAllowed(true, KnownTeam(c.attacker), KnownTeam(c.victim), match_.config().friendlyFire)) { ++claimsRejected_; return; }
        if (r.replaySlot == kHostSlot) claims_.Replay(c, game::SteadyNowMs());
        else if (std::optional<uint32_t> to = PeerOfSlot(r.replaySlot)) net_->Send(*to, kChanReliable, Encode(HitClaimMsg{c}), true);
        if (!r.fxSlots.empty()) {
            const std::vector<uint8_t> fx = Encode(HitFxMsg{c});
            for (uint8_t slot : r.fxSlots) if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, fx, true);
        }
        if (r.hostFx) claims_.Effect(c);
    }

    // A sever made on the host or received from a client, routed as its blow is: the victim's owner replays it on its
    // own fighter; every other machine keeps it until that owner's vitals confirm the part (the attacker kept its own).
    void RouteHostSever(uint8_t senderSlot, const combat::SeverClaim& c) {
        const ClaimRoute r = RouteClaim(senderSlot, c.attacker, c.victim, ConnectedSlots());
        if (!r.accept || !HitAllowed(true, KnownTeam(c.attacker), KnownTeam(c.victim), match_.config().friendlyFire)) { ++claimsRejected_; return; }
        const double now = game::SteadyNowMs();
        const std::vector<uint8_t> msg = Encode(combat::SeverClaimMsg{c});
        if (r.replaySlot == kHostSlot) severs_.Replay(c, now);
        else if (std::optional<uint32_t> to = PeerOfSlot(r.replaySlot)) net_->Send(*to, kChanReliable, msg, true);
        for (uint8_t slot : r.fxSlots) if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, msg, true);
        if (r.hostFx) severs_.Remember(c, now);
    }

    // A blade stuck in / freed from a stand-in: only the victim's owner holds it. A release always passes, so a blade
    // stuck before the friendly-fire rule changed is still let go.
    void RouteHostStuck(uint8_t senderSlot, const combat::StuckBladeMsg& m) {
        const ClaimRoute r = RouteClaim(senderSlot, m.attacker, m.victim, ConnectedSlots());
        if (!r.accept || (m.stuck && !HitAllowed(true, KnownTeam(m.attacker), KnownTeam(m.victim), match_.config().friendlyFire))) { ++claimsRejected_; return; }
        if (r.replaySlot == kHostSlot) severs_.OnStuck(m, game::SteadyNowMs());
        else if (std::optional<uint32_t> to = PeerOfSlot(r.replaySlot)) net_->Send(*to, kChanReliable, Encode(m), true);
    }

    UObject* FighterActor(uint8_t slot) {
        if (slot == 0) return InFightLevel() ? adapter_.PlayerWillie(0) : nullptr;
        ActorRef& a = fighters_[slot].actor;
        if (a.obj && !adapter_.StillValid(a.obj, a.index)) {
            HSM_LOG("host: fighter in slot %u was destroyed by the game", slot);
            a.Clear();
            if (OwnerMode()) lostSlots_.insert(slot);   // its stand-in is spawned again (RecoverLostStandIns)
        }
        return a.obj;
    }

    // Host authority: the host simulates every fighter; a remote fighter is driven by its player's input frames
    // (mouse deltas spread over host frames, see InputApplier::Tick).
    void ApplyInputs(UObject* w, InputApplier& in, float dtMs) {
        InputApplier::Step s = in.Tick(dtMs);
        for (Action a : s.released) adapter_.FireAction(w, a, false);
        for (Action a : s.pressed) adapter_.FireAction(w, a, true);
        for (size_t i = 0; i < kAxisCount; ++i) adapter_.ApplyAxis(w, Axis(i), s.axes[i]);
        adapter_.KeepRemoteFighter(w, in.latest().controlYaw, in.latest().controlPitch);
    }

    void DriveRemoteFighter(uint8_t slot, float dt) {
        UObject* w = FighterActor(slot);
        if (!w || !adapter_.BindInputs(w)) return;
        ApplyInputs(w, fighters_[slot].input, dt * 1000.f);
    }

    // The game's lock-on and "pal" checks compare Team Int (equal and non-zero = allies). Allies get the local
    // player's 1; enemies a distinct non-zero value per team, or per fighter when there are no teams.
    int TeamIntFor(int8_t team, uint8_t entity) {
        int8_t mine = kNoTeam;
        if (IsHost()) mine = TeamOfSlot(0);
        else if (const Spawn* own = world_.Find(world_.myEntity())) mine = own->team;
        if (team != kNoTeam && team == mine) return 1;
        return team != kNoTeam ? 10 + team : 2 + entity;
    }

    // Free Mode keeps respawning its foes onto the spawn points our fighters use; once the level's spawn has run
    // (our fighter exists) the foe count goes to 0 for the rest of the fight. The player's own value comes back on
    // the next non-fight level or when the session ends.
    void RememberFoes() { if (savedFoes_ < 0) savedFoes_ = adapter_.FreeModeFoes(); }
    // Before StartMatch the default Match config is FFA, even for a selected bot format. Consult the intended
    // config now; clients always remove local-only foes, preserving only the host's announced bot stand-ins.
    bool StockFoesAllowed() {
        if (!IsHost()) return false;
        if (match_.phase() != MatchPhase::Idle) return AllowStockFoes(true, IsBotFormat(match_.config().format), progression_);
        const MatchConfig planned = steamMode() ? lobby_.config() : cfg_.testMatch.Apply(MatchConfig{});
        const bool progression = !adapter_.FreeModeActive() && (steamMode() || !cfg_.testMatch.Enabled());
        return AllowStockFoes(true, IsBotFormat(planned.format), progression);
    }
    static std::string StockClassName(UObject* actor) {
        return ObjectName(reinterpret_cast<UObject*>(ClassOf(actor)));
    }
    void GuardStockSpawns() {
        // BP_LevelManager sets Spawn Player before calling Spawn Willies. That event queues a Delay(0): changing
        // Free Mode Foes Amount after the player appears cannot cancel an already queued foe.
        HookScript("stockspawns", "BP_SpawnerPoint_Willies_C", "Spawn Willies", [this](ScriptCall& c) {
            bool player = false;
            if (!net_ || !MatchLevel() || StockFoesAllowed()) return false;
            const bool known = GetBool(c.self(), "Spawn Player", player);
            if (!BlockStockSpawn(true, false, known, player)) return false;
            c.ZeroOutputs();
            HSM_LOG("session: blocked stock foe spawn class=Willie_BP_C via %s spawner_class=%s (Spawn Player=%s)",
                    ObjectName(c.self()).c_str(), StockClassName(c.self()).c_str(), known ? "false" : "missing");
            return true;
        });
        // A Spawn Willies request may already be inside Delay(0) when we join. Its continuation bypasses
        // the event wrapper. Sweep immediately after that graph returns, before the new foe gets a physics tick.
        ObserveScript("stockspawns", "BP_SpawnerPoint_Willies_C", "ExecuteUbergraph_BP_SpawnerPoint_Willies", {},
                      [this](ScriptCall&) {
                          if (!net_ || !MatchLevel() || StockFoesAllowed()) return;
                          lastFoeSweep_ = NowMs() - 250;
                          StockFoesTick();
                      });
        HookScript("stockspawns", "Abyss_Map_Open_EA_C", "Spawn NPC", [this](ScriptCall& c) {
            if (!BlockStockSpawn(net_ && MatchLevel(), StockFoesAllowed(), true, false)) return false;
            c.ZeroOutputs();
            HSM_LOG("session: blocked stock foe spawn via %s class=%s (Spawn NPC)", ObjectName(c.self()).c_str(), StockClassName(c.self()).c_str());
            return true;
        });
    }
    void StopStockFoes() {
        if (!net_ || !MatchLevel() || StockFoesAllowed()) return;
        RememberFoes();
        adapter_.SetFreeModeFoes(0); // keep the setting suppressed too; the VM hook is the actual request guard
        if (!foesStopped_) {
            foesStopped_ = true;
            HSM_LOG("session: stock foe suppression active (spawn guard + world cleanup; Free Mode foes requested=0)");
        }
    }
    void RemoveStockFoe(UObject* w) {
        diagnostics::Scope trace("RemoveStockFoe", reinterpret_cast<uintptr_t>(w));
        const std::string name = ObjectName(w), cls = StockClassName(w);
        // Stop collisions immediately even if destruction is deferred; remove carried blades as well as the body.
        Call(w, "SetActorEnableCollision").set<bool>("bNewActorEnableCollision", false).invoke();
        Call(w, "SetActorTickEnabled").set<bool>("bEnabled", false).invoke();
        const int32_t index = ObjectIndex(w);
        adapter_.DestroyFighter(w, index);
        HSM_LOG("session: stock foe removal %s class=%s result=%s", name.c_str(), cls.c_str(),
                adapter_.StillValid(w, index) ? "requested; retry if still present" : "destroyed");
    }
    void StockFoesTick() {
        diagnostics::Scope trace("StockFoesTick");
        if (!net_ || !MatchLevel() || StockFoesAllowed()) return;
        UObject* me = adapter_.PlayerWillie(0);
        if (!me) return; // preserve the level's player-spawner selection until its pawn exists
        const uint32_t now = NowMs();
        if (foesStopped_ && now - lastFoeSweep_ < 250) return;
        lastFoeSweep_ = now;
        StopStockFoes();
        for (UObject* w : adapter_.AllWillies())
            if (RemoveStockFighter(true, false, w == me, LocalEntityOf(w) != 0)) RemoveStockFoe(w);
    }
    void RestoreFoes() { if (savedFoes_ >= 0) { adapter_.SetFreeModeFoes(savedFoes_); savedFoes_ = -1; } }

    // Spawn height: the stock spawner put the host's body at the right height above its floor; every spawn point
    // gets that same height above the floor under it (spawner actors sit at varying heights, so players popped out of
    // the ground). Unchanged when a trace fails.
    FVector Grounded(FVector at) {
        UObject* host = adapter_.PlayerWillie(0);
        if (!groundValid_) {
            double fz;
            if (!host || !adapter_.FloorZ(adapter_.ActorLocation(host), host, fz)) return at;
            groundOffset_ = adapter_.ActorLocation(host).z - fz; groundValid_ = true;
            HSM_LOG("host: fighters stand %.0f cm above the floor in %s", groundOffset_, currentLevel_.c_str());
        }
        double fz;
        if (adapter_.FloorZ(at, host, fz)) at.z = fz + groundOffset_;
        return at;
    }

    // Spawn points: the host's own fighter is candidate 0, the level's Willie spawners follow; farthest-point
    // sampling then gives slot k its own spot, spreading out from the host.
    FVector SpawnPoint(uint8_t slot, double& yawDeg) {
        if (!picksValid_) {
            UObject* host = adapter_.PlayerWillie(0);
            spawnCenter_ = host ? adapter_.ActorLocation(host) : FVector{};
            std::vector<Point3> cands{{spawnCenter_.x, spawnCenter_.y, spawnCenter_.z}};
            for (const FVector& v : adapter_.SpawnCandidates())
                if (std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z) > 1.0) cands.push_back({v.x, v.y, v.z});   // (0,0,0) = unread spawner
            spawnCands_ = cands;
            picks_ = SelectSpread(cands, kMaxFighters);
            picksValid_ = true;
            HSM_LOG("host: %zu spawn candidates in %s", cands.size() - 1, currentLevel_.c_str());
        }
        const SpawnPick& pk = picks_[slot];
        const Point3& c = spawnCands_[pk.index];
        double angle = slot * (6.283185307179586 / kMaxFighters);
        FVector at{c.x + std::cos(angle) * kRingRadius * pk.ring, c.y + std::sin(angle) * kRingRadius * pk.ring, c.z};
        double dx = spawnCenter_.x - at.x, dy = spawnCenter_.y - at.y;
        yawDeg = (dx == 0 && dy == 0) ? 0 : std::atan2(dy, dx) * 57.29577951308232;   // face the host
        return Grounded(at);
    }

    void SpawnDue() {
        std::vector<uint8_t> due = director_.Due(NowMs());
        if (due.empty()) return;
        UObject* host = adapter_.PlayerWillie(0);
        if (!host) return;                              // the stock spawner has not placed the host yet
        for (uint8_t slot : due) {
            Spawn sp; sp.entity = Roster::EntityForSlot(slot); sp.ownerSlot = slot; sp.kind = EntityKind::Fighter;
            sp.team = TeamOfSlot(slot);
            FVector at; double yaw = 0;
            if (slot == 0) {
                at = adapter_.ActorLocation(host); yaw = adapter_.ActorYaw(host);
                sp.blobId = hostBlobs_.LatestFor(0);
                QueueTint(host, sp.team);
            } else {
                at = sp.team != kNoTeam ? SidePoint(sp.team, sp.entity, yaw) : SpawnPoint(slot, yaw);
                uint32_t id = director_.BlobFor(slot);
                const std::vector<uint8_t>* blob = hostBlobs_.Get(id);
                sp.blobId = blob ? id : 0;
                game::Adapter::SpawnOptions o; o.blob = blob; o.at = at; o.yawDeg = yaw; o.teamInt = TeamIntFor(sp.team, sp.entity);
                o.brain = !OwnerMode();                 // owner sync: a stand-in, moved by its owner's stream
                UObject* w = adapter_.SpawnFighter(o);
                if (!w) HSM_LOG("host: spawning slot %u failed; it stays empty until the next level", slot);
                fighters_[slot].actor.Set(w);
                if (w && OwnerMode()) standIns_.Attach(sp.entity, w);
                fighters_[slot].wasDead = false;
                if (w) QueueTint(w, sp.team);
            }
            if (match_.phase() != MatchPhase::Idle && !match_.Has(sp.entity)) { match_.AddFighter({sp.entity, sp.team, true}); infoDirty_ = true; }
            sp.x = float(at.x); sp.y = float(at.y); sp.z = float(at.z); sp.yaw = float(yaw);
            director_.MarkSpawned(sp);
            net_->Broadcast(kChanReliable, Encode(sp), true);
            HSM_LOG("host: entity %u (slot %u) live, loadout blob %08x", sp.entity, slot, sp.blobId);
            if (slot == 0) StopStockFoes();
        }
    }

    void DespawnFighter(uint8_t slot) {
        Fighter& f = fighters_[slot];
        if (OwnerMode()) {                              // a stand-in: unbound first, its weapons go with it
            standIns_.Detach(Roster::EntityForSlot(slot));
            props_.ForgetEntity(Roster::EntityForSlot(slot));
            adapter_.DestroyFighter(f.actor.obj, f.actor.index);
            f.actor.Clear(); f.wasDead = false;
            return;
        }
        UObject* w = f.actor.obj && adapter_.StillValid(f.actor.obj, f.actor.index) ? f.actor.obj : nullptr;
        InputApplier::Step s = f.input.ReleaseAll();
        if (w) { for (Action a : s.released) adapter_.FireAction(w, a, false); adapter_.DestroyActor(w); }
        f.actor.Clear(); f.wasDead = false;
    }

    // The host's own passport, shared like any client's (slot 0).
    void ShareHostLoadout() {
        if (roster_.size() == 0) return;
        std::vector<uint8_t> bytes;
        if (!adapter_.ExportPassport(bytes) || bytes == hostExport_) return;
        hostExport_ = bytes;
        StoreAndRelay(0, std::move(bytes), 0);
    }

    // Assigns a fresh host blob id for the slot, keeps it, and sends it to every peer except `skipPeer`.
    void StoreAndRelay(uint8_t slot, std::vector<uint8_t> bytes, uint32_t skipPeer) {
        uint32_t id = MakeBlobId(slot, ++blobVersion_[slot]);
        std::vector<BlobChunk> chunks = SplitBlob(id, bytes);
        if (chunks.empty() || !hostBlobs_.Put(id, std::move(bytes))) { HSM_LOG("host: loadout for slot %u not storable", slot); return; }
        director_.OnBlob(slot, id);
        if (!net_) return;
        for (const auto& [peer, p] : peers_) {
            if (peer == skipPeer) continue;
            for (const BlobChunk& c : chunks) net_->Send(peer, kChanReliable, Encode(c), true);
        }
    }

    void SendAllBlobs(uint32_t peer) {
        for (uint8_t slot = 0; slot < kMaxFighters; ++slot) {
            uint32_t id = hostBlobs_.LatestFor(slot);
            if (const std::vector<uint8_t>* b = id ? hostBlobs_.Get(id) : nullptr)
                for (const BlobChunk& c : SplitBlob(id, *b)) net_->Send(peer, kChanReliable, Encode(c), true);
        }
    }

    void HostReceive(const NetEvent& ev) {
        const uint8_t* d = ev.data.data(); size_t n = ev.data.size();
        MsgType t;
        if (!PeekType(d, n, t)) { if (IsForeignHello(d, n)) OnHello(ev); return; }
        if (t == MsgType::Hello) { OnHello(ev); return; }
        if (t == MsgType::Ping) {
            Ping p; bool pong; DecodePing(d, n, p, pong);
            net_->Send(ev.peer, kChanUnreliable, EncodePing(p.timeMs, true), false);
            return;
        }
        auto it = peers_.find(ev.peer);
        if (it == peers_.end()) return;                // nothing but Hello/Ping before Welcome
        Peer& peer = it->second;
        if (OwnerMode() && (t == MsgType::Pose || t == MsgType::Vitals)) {
            HostOwnerStream(ev, peer, t);
        } else if (OwnerMode() && t == MsgType::HitClaim) {
            HitClaimMsg m; Decode(d, n, m);
            RouteHostClaim(peer.slot, m.claim);
        } else if (OwnerMode() && t == MsgType::SeverClaim) {
            combat::SeverClaimMsg m; Decode(d, n, m);
            RouteHostSever(peer.slot, m.claim);
        } else if (OwnerMode() && t == MsgType::StuckBlade) {
            combat::StuckBladeMsg m; Decode(d, n, m);
            RouteHostStuck(peer.slot, m);
        } else if (OwnerMode() && t == MsgType::PaintFx) {
            combat::PaintFxMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.entity)) { ++streamsDropped_; return; }
            fx_.Apply(m);
            for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, ev.data, true);
        } else if (OwnerMode() && t == MsgType::Equip) {
            combat::EquipStateMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.entity)) { ++streamsDropped_; return; }
            equip_.Apply(m);
            for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, ev.data, true);
        } else if (OwnerMode() && t == MsgType::GrabState) {
            GrabStateMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.grabber)) { ++streamsDropped_; return; }
            grabs_.OnGrabState(m, game::SteadyNowMs());
            for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, ev.channel, ev.data, ev.channel == kChanReliable);
        } else if (OwnerMode() && t == MsgType::PropState) {
            if (props_.OnPropState(d, n, peer.slot))      // only the granted streamer's newer state is relayed
                for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                    if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanUnreliable, ev.data, false);
        } else if (OwnerMode() && t == MsgType::PropLease) {
            props_.OnPropLease(d, n, peer.slot);          // LeaseFromPeerOk inside; Grants go out reliable to everyone
        } else if (OwnerMode() && t == MsgType::DamageCredit) {
            DamageCredit c; Decode(d, n, c);
            if (!MayStream(peer.slot, c.victim) || !match_.Alive(c.victim) || !Opposing(c.attacker, c.victim)) return;
            kills_.OnDamage(c.victim, c.attacker, NowMs(), 1, true);
        } else if (OwnerMode() && t == MsgType::DeathReport) {
            DeathReport r; Decode(d, n, r);
            if (!MayStream(peer.slot, r.victim) || !match_.Alive(r.victim)) return;
            const uint32_t now = NowMs();
            // The owner's window wins, including 0. A delayed report must not revive expired credit.
            HandleDeath(r.victim, Opposing(r.killer, r.victim) ? r.killer : 0, now);
        } else if (t == MsgType::Input) {
            if (OwnerMode()) return;                    // every owner plays its own fighter
            InputFrame f; Decode(d, n, f);
            Fighter& fr = fighters_[peer.slot];
            fr.input.Receive(f); ++fr.inputsRx; fr.lastInputAt = NowMs();
        } else if (t == MsgType::Ready) {
            Ready r; Decode(d, n, r);
            director_.OnReady(peer.slot, r.travelSeq, NowMs());
            for (const Spawn& sp : director_.LiveSpawns()) net_->Send(ev.peer, kChanReliable, Encode(sp), true);
            // Every current lease, after the level load that cleared the joiner's view (props_.Clear).
            if (OwnerMode()) for (const std::vector<uint8_t>& g : props_.GrantsForJoiner()) net_->Send(ev.peer, kChanReliable, g, true);
            HSM_LOG("host: slot %u ready in %s (travel #%u)", peer.slot, r.level.c_str(), r.travelSeq);
        } else if (t == MsgType::Chat) {
            ChatLine c; Decode(d, n, c);
            if (!peer.chat.Allow(NowMs())) return;
            c.fromSlot = peer.slot;
            net_->Broadcast(kChanReliable, Encode(c), true);
            ui::ShowLine(NameOfSlot(peer.slot), c.text);
        } else if (t == MsgType::BlobChunk) {
            BlobChunk c; Decode(d, n, c);
            std::vector<uint8_t> blob;
            BlobAssembler::Result res = peer.blobs.Add(c, GetTickCount64(), blob);
            if (res == BlobAssembler::Result::Rejected) HSM_LOG("host: rejected loadout chunk from slot %u", peer.slot);
            if (res != BlobAssembler::Result::Complete) return;
            HSM_LOG("host: loadout from slot %u (%zu bytes)", peer.slot, blob.size());
            StoreAndRelay(peer.slot, std::move(blob), ev.peer);
        }
    }

    void OnHello(const NetEvent& ev) {
        Hello h;
        if (!Decode(ev.data.data(), ev.data.size(), h)) {        // another revision: only its header revision is readable
            h = Hello{}; h.revision = ev.data.size() > 2 ? ev.data[2] : 0;
            if (h.revision == kProtocolRevision) return;
        }
        // Each Hello resends every loadout (megabytes); a peer repeating it is ignored until it could be a rejoin.
        if (auto it = peers_.find(ev.peer); it != peers_.end() && NowMs() - it->second.helloAt < kHelloRepeatMs) {
            HSM_LOG("host: ignoring repeated Hello from peer %u", ev.peer);
            return;
        }
        Reject r;
        std::optional<uint8_t> slot;
        uint64_t key = ev.steamId ? ev.steamId : (uint64_t(1) << 63) | ev.peer;
        if (h.revision != kProtocolRevision) r.reason = RejectReason::BadRevision;
        else if (h.buildKey != BuildKey()) r.reason = RejectReason::BuildMismatch;
        else if (!(slot = roster_.Add(key, h.name))) r.reason = RejectReason::Full;
        if (slot && reservations_.Claim(key, NowMs())) {        // rejoin: score kept, fighter respawns fresh
            HSM_LOG("host: slot %u rejoined within its reservation", *slot);
            DespawnFighter(*slot);
            if (OwnerMode()) {   // a restarted machine: its stream, claim ids, gore and holds start afresh, here and everywhere
                const uint8_t e = Roster::EntityForSlot(*slot);
                standIns_.Forget(e); severs_.Forget(e); gore_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e); lives_.erase(e);
                director_.Forget(e);   // not live any more: SpawnDue spawns its stand-in and sends everyone a fresh Spawn
                Despawn dp; dp.entity = e;
                for (const auto& [peer, p] : peers_)
                    if (peer != ev.peer && p.key != key) net_->Send(peer, kChanReliable, Encode(dp), true);
            }
        }
        if (!slot) {
            HSM_LOG("host: rejecting peer %u (reason %d; its revision %u build %016llx, ours %u build %016llx)", ev.peer, int(r.reason),
                    h.revision, (unsigned long long)h.buildKey, kProtocolRevision, (unsigned long long)BuildKey());
            net_->Send(ev.peer, kChanReliable, Encode(r), true);
            net_->Disconnect(ev.peer);
            return;
        }
        // A reconnect can arrive before the old connection's Disconnected: forget the stale peer so its late
        // disconnect does not remove the member's fighter.
        std::erase_if(peers_, [&](const auto& kv) { return kv.first != ev.peer && kv.second.key == key; });
        Peer& p = peers_[ev.peer]; p = Peer{}; p.slot = *slot; p.key = key; p.helloAt = NowMs();
        fighters_[*slot].input.ReleaseAll();
        director_.OnJoin(*slot, NowMs());
        if (match_.phase() != MatchPhase::Idle) AssignLateTeam(*slot);
        Welcome w; w.sessionId = sessionId_; w.slot = *slot; w.entity = Roster::EntityForSlot(*slot);
        w.snapshotHz = uint16_t(SnapshotHz()); w.map = currentLevel_; w.syncMode = uint8_t(syncMode_);
        net_->Send(ev.peer, kChanReliable, Encode(w), true);
        SendAllBlobs(ev.peer);
        if (InFightLevel()) {                          // late joiner: follow the host into the running level
            Travel t;
            if (lastTravel_ && lastTravel_->level.find(currentLevel_) != std::string::npos) t = *lastTravel_;
            else { t.level = currentLevel_; t.settings = game::CaptureGameSettings(); }   // short map name resolves too
            if (!t.seq) t.seq = ++travelSeq_;
            net_->Send(ev.peer, kChanReliable, Encode(t), true);
        }
        HSM_LOG("host: '%s' joined as slot %u (%zu/%d fighters)", h.name.c_str(), *slot, roster_.size(), kMaxFighters);
    }

    // A Steam member who drops mid-match keeps slot, fighter and score for ReservationTable's window.
    void OnClientLeft(uint32_t peer) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        uint8_t slot = it->second.slot;
        uint64_t key = it->second.key;
        peers_.erase(it);
        if (OwnerMode()) props_.SlotLeft(slot);
        bool steamKey = !(key >> 63);
        if (match_.phase() != MatchPhase::Idle && steamKey && reservations_.Reserve(key, slot, NowMs())) {
            fighters_[slot].input.ReleaseAll();
            HSM_LOG("host: slot %u dropped mid-match; holding it for a rejoin", slot);
            return;
        }
        RemoveMember(slot, key);
    }

    void RemoveMember(uint8_t slot, uint64_t key) {
        roster_.Remove(key);
        DespawnFighter(slot);
        standIns_.Forget(Roster::EntityForSlot(slot)); severs_.Forget(Roster::EntityForSlot(slot)); gore_.Forget(Roster::EntityForSlot(slot)); lives_.erase(Roster::EntityForSlot(slot));
        claims_.Forget(Roster::EntityForSlot(slot)); fx_.Forget(Roster::EntityForSlot(slot)); equip_.Forget(Roster::EntityForSlot(slot));
        grabs_.Forget(Roster::EntityForSlot(slot));
        if (OwnerMode()) { props_.SlotLeft(slot); props_.ForgetEntity(Roster::EntityForSlot(slot)); }
        hostBlobs_.DropSlot(slot);
        if (uint8_t e = director_.OnLeave(slot)) { Despawn dp; dp.entity = e; net_->Broadcast(kChanReliable, Encode(dp), true); }
        match_.RemoveFighter(Roster::EntityForSlot(slot)); kills_.Forget(Roster::EntityForSlot(slot)); infoDirty_ = true;
        HSM_LOG("host: slot %u left", slot);
    }

    void SendEvent(EventKind k, uint32_t a, uint32_t b) {
        Event e; e.seq = ++eventSeq_; e.kind = k; e.a = a; e.b = b;
        net_->Broadcast(kChanReliable, Encode(e), true);
    }

    // ------------------------------------------------------------------ match (host)
    int8_t TeamOfSlot(uint8_t slot) const { auto it = slotTeam_.find(slot); return it == slotTeam_.end() ? kNoTeam : it->second; }

    uint8_t EntityOf(UObject* willie) {
        if (!willie) return 0;
        if (willie == hostWillie_) return 1;
        for (uint8_t s = 1; s < kMaxFighters; ++s) if (fighters_[s].actor.obj == willie) return Roster::EntityForSlot(s);
        for (auto& [e, ref] : bots_) if (ref.obj == willie) return e;
        return 0;
    }

    UObject* EntityActor(uint8_t e) {
        if (e >= 1 && e <= kMaxFighters) return FighterActor(uint8_t(e - 1));
        auto it = bots_.find(e);
        return it != bots_.end() && adapter_.StillValid(it->second.obj, it->second.index) ? it->second.obj : nullptr;
    }

    // Host authority for hits: clients never resolve a contact (their bodies follow the host); the host resolves
    // them all with the friendly-fire rule, records kill credit and sends the blood.
    // Owner sync: every machine resolves contacts between the fighters it simulates itself, natively, with the same
    // rule; a contact with a stand-in runs on to Deal Complex Damage, where game::Claims decides it. A clash of my
    // weapon with a stand-in's weapon is noted for the parry rule.
    bool HitHook(UObject* obj, UFunction* fn, void* params) {
        if (!net_ || !params || !inFight_) return false;
        const bool owner = OwnerMode();
        if (!IsHost() && !owner) { if (!welcomed_) return false; ++hitsBlocked_; return true; }
        int32_t off = OtherActorOffset(fn);
        if (off < 0) return false;
        UObject* other = *reinterpret_cast<UObject**>(static_cast<uint8_t*>(params) + off);
        if (owner) props_.NoteContact(obj, other);
        bool selfWeapon = adapter_.IsWeapon(obj), otherWeapon = other && adapter_.IsWeapon(other);
        if (selfWeapon && otherWeapon) { if (owner) NoteWeaponClash(obj, other); return false; }
        UObject* weapon = selfWeapon ? obj : otherWeapon ? other : nullptr;
        UObject* victim = selfWeapon ? adapter_.FighterOf(other) : obj;
        if (!victim || !adapter_.IsWillie(victim)) return false;
        UObject* attacker = weapon ? adapter_.FighterOf(weapon) : nullptr;
        uint8_t ve = owner ? OwnedEntityOf(victim) : LocalEntityOf(victim);
        uint8_t ae = !attacker ? 0 : owner ? OwnedEntityOf(attacker) : LocalEntityOf(attacker);
        if (!ve) return false;                        // not a session fighter (e.g. a stock foe being removed), or a stand-in
        if (!CombatEnabled()) { ++hitsBlocked_; return true; }
        bool ff = IsHost() ? match_.config().friendlyFire : info_.friendlyFire;
        if (!HitAllowed(true, ae ? KnownTeam(ae) : kNoTeam, KnownTeam(ve), ff)) { ++hitsBlocked_; return true; }
        if (ae && ae != ve) {
            if (IsHost() && !owner) kills_.OnHit(ve, ae, NowMs());
            if (!owner) SendBlood(ve, victim, fn, params);
        }
        return false;
    }

    void NoteWeaponClash(UObject* a, UObject* b) {
        UObject* fa = adapter_.FighterOf(a);
        UObject* fb = adapter_.FighterOf(b);
        const uint8_t sa = standIns_.EntityOf(fa), sb = standIns_.EntityOf(fb);
        if (sa && !sb && OwnedEntityOf(fb)) claims_.NoteClash(sa, game::SteadyNowMs());
        else if (sb && !sa && OwnedEntityOf(fa)) claims_.NoteClash(sb, game::SteadyNowMs());
    }

    // Hit events fire thousands of times a second: the parameter offset is looked up once per function.
    int32_t OtherActorOffset(UFunction* fn) {
        auto it = otherActorOffset_.find(fn);
        if (it != otherActorOffset_.end()) return it->second;
        FProperty* p = FindProperty(fn, "OtherActor");
        int32_t off = p ? PropertyOffset(p) : -1;
        otherActorOffset_[fn] = off;
        return off;
    }

    uint8_t LocalEntityOf(UObject* w) {
        if (!w) return 0;
        if (IsHost()) return EntityOf(w);
        if (w == myWillie_.load()) return world_.myEntity();
        for (auto& [e, ref] : puppets_) if (ref.obj == w) return e;
        return 0;
    }
    bool Opposing(uint8_t attacker, uint8_t victim) {
        if (!ValidEntity(attacker) || !ValidEntity(victim) || attacker == victim) return false;
        if (IsHost() && (!match_.Has(attacker) || !match_.Has(victim))) return false;
        int8_t a = KnownTeam(attacker), v = KnownTeam(victim);
        return a == kNoTeam || v == kNoTeam || a != v;
    }
    int8_t KnownTeam(uint8_t e) {
        if (IsHost()) return match_.TeamOf(e);
        for (const FighterEntry& f : info_.fighters) if (f.entity == e) return f.team;
        return kNoTeam;
    }

    void ApplyCombat(const CombatEvent& e) {
        UObject* w = IsHost() ? EntityActor(e.entity) : ClientActor(e.entity);
        if (!w) return;
        bool ok = e.kind == kCombatDismember ? game::ApplyDismember(w, e.partsMask) : game::ApplyBlood(w, e);
        if (ok) ++bloodApplied_;
    }

    // Blood for a hit on a body this machine owns, for every other screen: at the impact point, max one per victim
    // every 150 ms. Clients send it to the host, which relays it.
    void SendBlood(uint8_t victim, UObject* target, UFunction* fn, void* params) {
        if (!net_ || (IsHost() ? peers_.empty() : !hostPeer_)) return;
        uint32_t now = NowMs();
        auto last = lastBloodAt_.find(victim);
        if (last != lastBloodAt_.end() && now - last->second < 150) return;
        FProperty* hit = FindProperty(fn, "Hit");
        FProperty* imp = FindProperty(fn, "NormalImpulse");
        if (!hit) return;
        const uint8_t* h = static_cast<const uint8_t*>(params) + PropertyOffset(hit);
        FVector at; std::memcpy(&at, h + kHitImpactPointOffset, sizeof at);   // FHitResult::ImpactPoint
        UObject* body = adapter_.FighterOf(target) ? adapter_.FighterOf(target) : target;
        if (!std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || Dist(at, adapter_.ActorLocation(body)) > 400.0) return;
        double strength = 0;
        if (imp) { FVector n; std::memcpy(&n, static_cast<const uint8_t*>(params) + PropertyOffset(imp), sizeof n); strength = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z); }
        if (imp && strength < 50.0) return;            // resting contact, not a blow
        lastBloodAt_[victim] = now;
        CombatEvent e; e.kind = kCombatBlood; e.entity = victim; e.x = float(at.x); e.y = float(at.y); e.z = float(at.z);
        e.bone = game::NearestPartBone(body, at); e.size = uint8_t(std::min(255.0, strength / 100.0)); e.sharp = 1;
        if (IsHost()) net_->Broadcast(kChanReliable, Encode(e), true);
        else net_->Send(hostPeer_, kChanReliable, Encode(e), true);
        ++bloodSent_;
    }

    // A player joining a running match keeps everyone else's team and goes to their pick or the smallest team.
    void AssignLateTeam(uint8_t slot) {
        std::vector<TeamRequest> req;
        for (const auto& [s, t] : slotTeam_) req.push_back({s, kNoTeam, t});
        TeamRequest mine; mine.slot = slot;
        if (steamMode()) if (const RosterMember* m = roster_.BySlot(slot))
            for (const game::LobbyMember& lm : lobby_.members()) if (lm.id == m->key) { mine.pick = int8_t(lm.team); mine.hostOverride = int8_t(lm.hostOverride); }
        req.push_back(mine);
        slotTeam_[slot] = ResolveTeams(req, match_.config().format)[slot];
    }

    void StartMatch(uint32_t now) {
        MatchConfig cfg = steamMode() ? lobby_.config() : cfg_.testMatch.Apply(MatchConfig{});
        bool progression = !adapter_.FreeModeActive() && (steamMode() || !cfg_.testMatch.Enabled());
        if (progression) { cfg.format = Format::PlayersVsBots; cfg.scoreLimit = 100; }   // co-op against the event's opponents
        if (cfg.format == Format::Duel) cfg.friendlyFire = true;   // two fighters, no teams: every hit counts
        std::vector<TeamRequest> req;
        for (const RosterMember& m : roster_.members()) {
            TeamRequest r; r.slot = m.slot;
            if (steamMode()) for (const game::LobbyMember& lm : lobby_.members())
                if (lm.id == (m.key == kHostKey ? steam::MySteamId() : m.key)) { r.pick = int8_t(lm.team); r.hostOverride = int8_t(lm.hostOverride); }
            req.push_back(r);
        }
        slotTeam_ = ResolveTeams(req, cfg.format);
        std::vector<FighterInfo> list;
        for (const RosterMember& m : roster_.members()) list.push_back({m.entity, TeamOfSlot(m.slot), true});
        match_.Start(cfg, list, now);
        kills_ = KillLog{}; wave_ = 0; adoptStock_ = IsBotFormat(cfg.format); wavesEnabled_ = adoptStock_ && !progression;
        bool gearApplied = !progression && !IsBotFormat(cfg.format) && cfg.gear != Gear::Own;   // gear is a PvP option
        if (gearApplied) AssignMatchGear(cfg);
        progression_ = progression;
        sidesValid_ = false; infoDirty_ = true;
        HSM_LOG("host: match started (format %d, limit %u, time %us, friendly fire %s)%s", int(cfg.format), cfg.scoreLimit, cfg.timeLimitS,
                cfg.friendlyFire ? "on" : "off", progression ? " - Progression co-op" : "");
        HSM_LOG("host: match gear %d, tier %u", int(cfg.gear), cfg.tier);
        if (TeamCount(cfg.format) > 0 || gearApplied) RespawnAll();   // team sides / generated loadouts take effect
    }

    void HostMatchTick() {
        hostWillie_ = MatchLevel() ? adapter_.PlayerWillie(0) : nullptr;
        if (!MatchLevel()) return;
        uint32_t now = NowMs();
        bool wanted = roster_.size() >= 2 || (steamMode() && IsBotFormat(lobby_.config().format));
        if (match_.phase() == MatchPhase::Idle) {
            // Start once every member's fighter is in (the director's ready/loadout timeouts bound the wait).
            bool allIn = true;
            for (const RosterMember& m : roster_.members()) allIn = allIn && director_.Live(m.entity);
            if (wanted && hostWillie_ && allIn) StartMatch(now); else return;
        }
        if (now >= nextScan_) { nextScan_ = now + kScanIntervalMs; ScanStockWillies(); }
        DetectDeaths(now);
        Match::Events ev = match_.Tick(now);
        if (ev.live || ev.ended || ev.rematch || ev.waveStarted || ev.waveCleared || ev.roundReset || ev.countdownRestart) infoDirty_ = true;
        if (ev.waveStarted) HSM_LOG("host: wave %d starts (%d bots)", match_.wave() + 1, match_.botsLeft());
        if (ev.waveCleared) HSM_LOG("host: wave cleared");
        if (ev.countdownRestart) HSM_LOG("host: duel countdown restarted; recovering %zu fighter(s)", ev.respawn.size());
        if (ev.roundReset) HSM_LOG("host: duel round reset");
        if (ev.ended) HSM_LOG("host: match over, winner key %d", match_.winner());
        if (ev.rematch && progression_) {                // Progression fights end back in the Tavern, like the stock game
            HSM_LOG("host: Progression fight over; returning everyone to the hub");
            HostTravel(kHubLevel);
            return;
        }
        for (uint8_t e : ev.respawn) RespawnEntity(e);
        if (ev.rematch) RespawnAll();
        if (progression_ && !bots_.empty() && match_.phase() == MatchPhase::Live) {
            bool anyBot = false;
            for (auto& [e, ref] : bots_) anyBot = anyBot || match_.Alive(e);
            if (!anyBot) { match_.Finish(kHumanTeam, now); infoDirty_ = true; }
        }
        if (wavesEnabled_ && ev.spawnBots) SpawnWaveBots(ev.spawnBots, ev.botRank);
        if (wavesEnabled_ && now >= nextCorpseSweep_) {
            nextCorpseSweep_ = now + kScanIntervalMs;
            for (uint8_t e : match_.StaleBotCorpses(now, Match::kCorpseMs)) RemoveBot(e);
        }
        if (infoDirty_ || now >= nextInfo_) BroadcastInfo(now);
    }

    void DetectDeaths(uint32_t now) {
        std::vector<uint8_t> entities;
        for (const RosterMember& m : roster_.members()) entities.push_back(m.entity);
        for (auto& [e, ref] : bots_) entities.push_back(e);
        for (uint8_t e : entities) {
            if (!match_.Alive(e)) continue;
            if (OwnerMode() && e >= 2 && e <= kMaxFighters) {   // a client's fighter: its DeathReport, else its vitals
                auto it = lives_.find(e);
                if (it != lives_.end() && it->second.deadSince && now - it->second.deadSince >= kVitalsDeathMs) {
                    HSM_LOG("host: entity %u reported dead by its vitals only", e);
                    HandleDeath(e, kills_.KillerOf(e, now), now);
                }
                continue;
            }
            UObject* w = EntityActor(e);
            if (!w || !adapter_.IsDead(w)) continue;
            HandleDeath(e, kills_.KillerOf(e, now), now);
        }
    }

    void HandleDeath(uint8_t e, uint8_t killer, uint32_t now) {
        if (!match_.Alive(e)) return;
        if (OwnerMode() && !Opposing(killer, e)) killer = 0;
        const int before = killer ? match_.Score(match_.ScoreKeyOf(killer)) : 0;
        if (OwnerMode() && standIns_.ActorOf(e)) standIns_.OnDeath(e);
        {
            match_.OnDeath(e, killer, now);
            HSM_LOG("host: death e%u credited e%u, damage age %s (window %ums), score %d -> %d, phase %u winner %d",
                    e, killer, kills_.AgeText(e, now).c_str(), KillLog::kWindowMs, before,
                    killer ? match_.Score(match_.ScoreKeyOf(killer)) : 0, unsigned(match_.phase()), match_.winner());
            Kill k; k.killer = killer; k.victim = e;
            if (killer == 0) k.flags = 4;
            else if (killer == e) k.flags = 2;
            else if (match_.TeamOf(killer) != kNoTeam && match_.TeamOf(killer) == match_.TeamOf(e)) k.flags = 1;
            net_->Broadcast(kChanReliable, Encode(k), true);
            PushFeed(k);
            infoDirty_ = true;
        }
    }

    // Team side point for a (re)spawn, farthest from living enemies; small per-entity offset so batches never stack.
    FVector SidePoint(int8_t team, uint8_t entity, double& yawDeg) {
        if (!sidesValid_) {
            sidePoints_.clear();
            for (const FVector& v : adapter_.SpawnCandidates()) sidePoints_.push_back({v.x, v.y, v.z});
            if (sidePoints_.size() < 2) {                  // no spawners: a ring around where the host stands
                FVector c = hostWillie_ ? adapter_.ActorLocation(hostWillie_) : FVector{};
                sidePoints_.clear();
                for (int i = 0; i < 8; ++i) sidePoints_.push_back({c.x + std::cos(i * 0.785398) * 900, c.y + std::sin(i * 0.785398) * 900, c.z});
            }
            sides_ = PartitionSides(sidePoints_, std::max(TeamCount(match_.config().format), 1));
            sidesValid_ = true;
        }
        std::vector<Point3> enemies;
        for (const RosterMember& m : roster_.members()) AddEnemy(enemies, m.entity, team, entity);
        for (auto& [e, ref] : bots_) AddEnemy(enemies, e, team, entity);
        const Point3& p = sidePoints_[PickRespawn(sidePoints_, sides_, team, enemies)];
        double angle = entity * 1.0471975511965976;
        FVector at{p.x + std::cos(angle) * kRingRadius, p.y + std::sin(angle) * kRingRadius, p.z};
        at = Grounded(at);
        double cx = 0, cy = 0;
        for (const Point3& q : sidePoints_) { cx += q.x; cy += q.y; }
        cx /= double(sidePoints_.size()); cy /= double(sidePoints_.size());
        yawDeg = std::atan2(cy - at.y, cx - at.x) * 57.29577951308232;   // face the middle
        return at;
    }

    void AddEnemy(std::vector<Point3>& out, uint8_t e, int8_t team, uint8_t self) {
        if (e == self || !match_.Alive(e) || (team != kNoTeam && match_.TeamOf(e) == team)) return;
        if (UObject* w = EntityActor(e)) { FVector v = adapter_.ActorLocation(w); out.push_back({v.x, v.y, v.z}); }
    }

    void RespawnEntity(uint8_t e) {
        severedSent_.erase(e);                       // a fresh body: its cuts are reported again
        if (e < 1 || e > kMaxFighters) return;            // bots come back as waves, not respawns
        uint8_t slot = uint8_t(e - 1);
        if (!roster_.BySlot(slot)) { match_.MarkRespawned(e); return; }
        int8_t team = match_.TeamOf(e);
        double yaw = 0;
        FVector at = SidePoint(team, e, yaw);
        game::Adapter::SpawnOptions o; o.at = at; o.yawDeg = yaw; o.teamInt = slot == 0 ? 1 : TeamIntFor(team, e);
        o.brain = slot == 0 || !OwnerMode();
        uint32_t id = slot == 0 ? hostBlobs_.LatestFor(0) : director_.BlobFor(slot);
        o.blob = hostBlobs_.Get(id);
        UObject* w = nullptr;
        severs_.Forget(e);                           // a new body: its stuck blades, holds and severs start afresh
        claims_.Forget(e); fx_.Forget(e); equip_.Forget(e);
        if (slot == 0) { w = adapter_.RespawnLocalPlayer(o); hostWillie_ = w; }
        else {
            DespawnFighter(slot);
            if (OwnerMode()) {                           // the new body's stream starts afresh
                standIns_.Forget(e); gore_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e); lives_.erase(e);
            }
            w = adapter_.SpawnFighter(o); fighters_[slot].actor.Set(w);
            if (w && OwnerMode()) standIns_.Attach(e, w);
        }
        if (!w) {
            match_.RespawnFailed(e, NowMs()); infoDirty_ = true;
            HSM_LOG("host: respawn of entity %u failed; will retry", e);
            return;
        }
        if (slot == 0) myWillie_.store(w);
        Spawn sp; sp.entity = e; sp.ownerSlot = slot; sp.kind = EntityKind::Fighter; sp.team = team; sp.blobId = o.blob ? id : 0;
        sp.x = float(at.x); sp.y = float(at.y); sp.z = float(at.z); sp.yaw = float(yaw);
        director_.MarkSpawned(sp);
        Despawn dp; dp.entity = e;
        net_->Broadcast(kChanReliable, Encode(dp), true);
        net_->Broadcast(kChanReliable, Encode(sp), true);
        match_.MarkRespawned(e); kills_.Forget(e);
        QueueTint(w, team);
    }

    void RespawnAll() {
        for (const RosterMember& m : roster_.members()) RespawnEntity(m.entity);
        ClearBots();
        wave_ = 0;
    }

    void ClearBots() {
        for (auto& [e, ref] : bots_) {
            if (adapter_.StillValid(ref.obj, ref.index)) adapter_.DestroyActor(ref.obj);
            Despawn dp; dp.entity = e; net_->Broadcast(kChanReliable, Encode(dp), true);
            director_.Forget(e); match_.RemoveFighter(e); severs_.Forget(e); gore_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e);
        }
        bots_.clear();
    }

    uint8_t FreeBotEntity() const {
        for (uint8_t e = kFirstBotEntity; e <= kLastBotEntity; ++e) if (!bots_.count(e)) return e;
        return 0;
    }

    void AnnounceBot(uint8_t e, UObject* w) {
        bots_[e].Set(w);
        std::vector<uint8_t> bytes;
        if (adapter_.ExportPassportOf(w, bytes)) StoreAndRelay(uint8_t(e - 1), std::move(bytes), 0);
        FVector at = adapter_.ActorLocation(w);
        Spawn sp; sp.entity = e; sp.ownerSlot = 0xFF; sp.kind = EntityKind::Bot; sp.team = kBotTeam; sp.blobId = hostBlobs_.LatestFor(uint8_t(e - 1));
        sp.x = float(at.x); sp.y = float(at.y); sp.z = float(at.z); sp.yaw = float(adapter_.ActorYaw(w));
        director_.MarkSpawned(sp);
        net_->Broadcast(kChanReliable, Encode(sp), true);
        match_.AddFighter({e, kBotTeam, false});
        infoDirty_ = true;
    }

    // Stock NPCs: adopted as synced bots in players-vs-bots / Progression, removed otherwise.
    void ScanStockWillies() {
        int purged = 0;
        for (UObject* w : adapter_.AllWillies()) {
            if (EntityOf(w)) continue;
            uint8_t e = adoptStock_ && !adapter_.IsDead(w) ? FreeBotEntity() : 0;
            if (e) { AnnounceBot(e, w); HSM_LOG("host: stock fighter adopted as bot entity %u class=%s", e, StockClassName(w).c_str()); }
            else if (!StockFoesAllowed()) { RemoveStockFoe(w); ++purged; }
        }
        if (purged) HSM_LOG("host: removed %d unsynced stock fighter(s)", purged);
    }

    // The match decides how many bots and of which rank; entities of dead bots are reclaimed when none are free.
    void SpawnWaveBots(int count, int rank) {
        int spawned = 0;
        for (int i = 0; i < count; ++i) {
            uint8_t e = FreeBotEntity();
            if (!e) for (auto& [b, ref] : bots_) if (!match_.Alive(b)) { e = b; break; }
            if (!e) break;
            if (bots_.count(e)) RemoveBot(e);
            double yaw = 0;
            FVector at = SidePoint(kBotTeam, e, yaw);
            UObject* bot = match_.config().format == Format::Zombies ? adapter_.SpawnAbyssNpc(at) : nullptr;
            if (!bot) bot = adapter_.SpawnBot(rank, at, yaw, 2);
            if (!bot) break;
            AnnounceBot(e, bot); ++spawned;
        }
        if (spawned < count) { match_.SpawnFailed(count - spawned); HSM_LOG("host: spawned %d of %d bots", spawned, count); }
    }

    void RemoveBot(uint8_t e) {
        auto it = bots_.find(e);
        if (it == bots_.end()) return;
        if (adapter_.StillValid(it->second.obj, it->second.index)) adapter_.DestroyActor(it->second.obj);
        Despawn dp; dp.entity = e; net_->Broadcast(kChanReliable, Encode(dp), true);
        director_.Forget(e); match_.RemoveFighter(e); kills_.Forget(e); severedSent_.erase(e); severs_.Forget(e); gore_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e);
        bots_.erase(it);
        infoDirty_ = true;
    }

    // Random-by-tier gear: a fresh generated loadout per player; Mirror: one generated loadout for everyone.
    void AssignMatchGear(const MatchConfig& cfg) {
        std::vector<uint8_t> shared;
        if (cfg.gear == Gear::Mirror) shared = game::RandomPassportBlob(adapter_, cfg.tier);
        for (const RosterMember& m : roster_.members()) {
            std::vector<uint8_t> blob = cfg.gear == Gear::Mirror ? shared : game::RandomPassportBlob(adapter_, cfg.tier);
            if (blob.empty()) { HSM_LOG("host: no generated gear for slot %u; it keeps its own loadout", m.slot); continue; }
            StoreAndRelay(m.slot, std::move(blob), 0);
        }
    }

    void BroadcastInfo(uint32_t now) {
        infoDirty_ = false; nextInfo_ = now + kInfoIntervalMs;
        MatchInfo m;
        const MatchConfig& c = match_.config();
        m.format = uint8_t(c.format); m.scoreLimit = c.scoreLimit; m.timeLimitS = c.timeLimitS; m.respawnDelayS = c.respawnDelayS;
        m.friendlyFire = c.friendlyFire; m.phase = uint8_t(match_.phase()); m.timeLeftMs = match_.TimeLeftMs(now);
        m.winner = int16_t(match_.winner());
        for (const auto& [key, score] : match_.scores()) if (m.scores.size() < kMaxWillies) m.scores.push_back({int16_t(key), int16_t(score)});
        for (const RosterMember& r : roster_.members())
            m.fighters.push_back({r.entity, match_.TeamOf(r.entity), int16_t(match_.Kills(r.entity)), int16_t(match_.Deaths(r.entity)), r.name});
        for (auto& [e, ref] : bots_)
            if (m.fighters.size() < kMaxWillies) m.fighters.push_back({e, kBotTeam, int16_t(match_.Kills(e)), int16_t(match_.Deaths(e)), "Bot"});
        net_->Broadcast(kChanReliable, Encode(m), true);
        info_ = std::move(m);
    }

    // ------------------------------------------------------------------ match (both)
    void PushFeed(const Kill& k) {
        feed_.push_back({NowMs(), KillLine(info_, k)});
        while (feed_.size() > 4) feed_.pop_front();
    }

    void QueueTint(UObject* w, int8_t team) {
        if (team == kNoTeam || !w) return;
        Tint t; t.actor.Set(w); t.team = team; t.next = NowMs() + kTintRetryMs; t.tries = kTintTries;
        tints_.push_back(t);
    }

    // Armor pieces attach over the first moments after a spawn; re-apply a few times, then stop.
    void TintTick() {
        uint32_t now = NowMs();
        for (Tint& t : tints_) {
            if (t.tries <= 0 || now < t.next) continue;
            t.next = now + kTintRetryMs; --t.tries;
            if (!adapter_.StillValid(t.actor.obj, t.actor.index)) { t.tries = 0; continue; }
            Rgba c = TeamColor(t.team);
            adapter_.TintArmor(t.actor.obj, c.r, c.g, c.b);
        }
        std::erase_if(tints_, [](const Tint& t) { return t.tries <= 0; });
    }

    ui::HudModel BuildHud() {
        ui::HudModel h;
        h.visible = MatchLevel() && info_.phase != uint8_t(MatchPhase::Idle);
        if (!h.visible) return h;
        uint8_t me = IsHost() ? uint8_t(1) : world_.myEntity();
        h.status = StatusLine(info_, me);
        uint32_t now = NowMs();
        UObject* own = adapter_.PlayerWillie(0);
        bool dead = own && adapter_.IsDead(own);
        if (dead && !ownDiedAt_) ownDiedAt_ = now;
        if (!dead && !ownDeadPending_) ownDiedAt_ = 0;
        if (ownDiedAt_ && info_.phase != uint8_t(MatchPhase::Results)) {
            uint32_t delay = Format(info_.format) == Format::Duel ? Match::kRoundGapMs : uint32_t(info_.respawnDelayS) * 1000u, since = now - ownDiedAt_;
            h.respawn = since < delay ? "Respawning in " + Seconds(delay - since) : "Respawning...";
        }
        for (const auto& [at, line] : feed_) if (now - at < kFeedMs) h.feed += line + "\n";
        h.scoreboard = Scoreboard(info_);
        return h;
    }

    // Dead and waiting: watch living fighters (teammates first); Left/Right arrows cycle.
    void SpectateTick() {
        UObject* own = adapter_.PlayerWillie(0);
        bool watching = MatchLevel() && info_.phase != uint8_t(MatchPhase::Idle) && info_.phase != uint8_t(MatchPhase::Results)
                        && (!own || adapter_.IsDead(own) || ownDeadPending_);
        if (!watching) {
            if (spectating_) { game::ResetViewToPawn(adapter_); spectating_ = 0; spectateActor_ = nullptr; }
            return;
        }
        uint8_t me = IsHost() ? uint8_t(1) : world_.myEntity();
        int8_t myTeam = kNoTeam;
        std::vector<SpectateCycler::Candidate> cands;
        for (const FighterEntry& f : info_.fighters) {
            if (f.entity == me) { myTeam = f.team; continue; }
            cands.push_back({f.entity, f.team, EntityAlive(f.entity)});
        }
        spectate_.Refresh(me, myTeam, cands);
        bool fg = false;
        if (HWND w = GetForegroundWindow()) { DWORD pid = 0; GetWindowThreadProcessId(w, &pid); fg = pid == GetCurrentProcessId(); }
        bool right = fg && (GetAsyncKeyState(VK_RIGHT) & 0x8000), left = fg && (GetAsyncKeyState(VK_LEFT) & 0x8000);
        if (right && !spectateKeyDown_) spectate_.Next();
        if (left && !spectateKeyDown_) spectate_.Prev();
        spectateKeyDown_ = right || left;
        std::optional<uint8_t> target = spectate_.Target();
        UObject* actor = target ? (IsHost() ? EntityActor(*target) : ClientActor(*target)) : nullptr;
        if (actor && actor != spectateActor_ && game::SetViewTarget(adapter_, actor, 0.4f)) {
            spectating_ = *target; spectateActor_ = actor;
        }
    }

    bool EntityAlive(uint8_t e) {
        if (IsHost()) return match_.Alive(e);
        if (OwnerMode()) { const VitalsMsg* v = standIns_.Vitals(e); return !v || !(v->flags & 1); }
        for (const WillieState& s : latest_.willies) if (s.entity == e) return !(s.flags & 1);
        return false;
    }

    // ------------------------------------------------------------------ client
    void OnOwnSpawn(const Spawn& sp) {
        if (OwnerMode() && !levelGate_.Open()) return;   // the level would wipe it; ClientGateOpen applies it later
        if (!ownDeadPending_) { if (UObject* me = adapter_.PlayerWillie(0)) QueueTint(me, sp.team); return; }
        if (NowMs() < nextOwnRespawn_) return;
        nextOwnRespawn_ = NowMs() + 1000;
        placed_.erase(world_.myEntity());
        const std::vector<uint8_t>* assigned = sp.blobId ? clientBlobs_.Get(sp.blobId) : nullptr;   // e.g. generated gear
        game::Adapter::SpawnOptions o; o.blob = assigned ? assigned : lastUpload_.empty() ? nullptr : &lastUpload_;
        o.at = {sp.x, sp.y, sp.z}; o.yawDeg = sp.yaw; o.teamInt = 1;
        severs_.Forget(world_.myEntity());          // a new body: its stuck blades and holds start afresh
        claims_.Forget(world_.myEntity()); fx_.Forget(world_.myEntity()); equip_.Forget(world_.myEntity());
        UObject* w = adapter_.RespawnLocalPlayer(o);
        snappedOwn_ = false;                         // input hooks are per function and follow myWillie_
        if (w) { ownDeadPending_ = false; nextOwnRespawn_ = 0; myWillie_.store(w); QueueTint(w, sp.team); }
        else HSM_LOG("client: local respawn failed; will retry in 1s");
    }

    void SendHello() {
        Hello h; h.buildKey = BuildKey(); h.name = MyName();
        net_->Send(hostPeer_, kChanReliable, Encode(h), true);
        nextHello_ = NowMs() + 2000;
        HSM_LOG("client: hello sent to the host%s", hellosSent_++ ? " (no welcome yet; retrying)" : "");
    }

    void ReturnToMenuAfterHostLoss() {
        HSM_LOG("client: host lost; returning to the main menu");
        if (steamMode()) lobby_.Leave();
        ResetNetwork("host lost");
        game::OpenLevel(lastGameMode_, kMenuLevel, true, "");
    }

    void ClientTick(float dt) {
        bool hostGone = false;
        NetEvent ev;
        while (net_->Poll(ev)) {
            if (ev.kind == NetEvent::Connected) {
                hostPeer_ = ev.peer;
                SendHello();
            } else if (ev.kind == NetEvent::Disconnected) {
                HSM_LOG("client: disconnected from host");
                if (welcomed_ && InFightLevel()) hostGone = hostLoss_.OnDisconnect(NowMs()) == HostLossAction::ReturnToMenu;
                welcomed_ = false; hostPeer_ = 0;
            } else { hostLoss_.OnPacket(NowMs()); ClientReceive(ev); }
        }
        // Only fights have steady host traffic (snapshots); menus and loading screens never count as silence.
        if (welcomed_ && InFightLevel()) {
            HostLossAction a = hostLoss_.Tick(NowMs());
            if (a == HostLossAction::ShowReconnecting) HSM_LOG("client: host silent; waiting for it to come back");
            hostGone = hostGone || a == HostLossAction::ReturnToMenu;
        } else hostLoss_.OnPacket(NowMs());
        if (hostGone) { ReturnToMenuAfterHostLoss(); return; }
        if (!welcomed_ && hostPeer_ && NowMs() >= nextHello_) SendHello();   // the first message of a Steam session can be lost
        if (!welcomed_ || !InFightLevel()) { myWillie_.store(nullptr); return; }
        if (OwnerMode() && !ClientGateOpen()) { myWillie_.store(adapter_.PlayerWillie(0)); return; }
        if (ownDeadPending_) if (const Spawn* sp = world_.Find(world_.myEntity())) OnOwnSpawn(*sp);
        if (OwnerMode()) RecoverLostPuppets();
        CreatePuppets();

        UObject* me = adapter_.PlayerWillie(0);
        if (OwnerMode()) {
            myWillie_.store(me);
            if (me) StopStockFoes();
            ClientOwnerTick(me, dt);
            return;
        }
        if (me && !hooksBound_ && adapter_.BindInputs(me)) BindClientInputHooks();
        myWillie_.store(me);
        if (me) StopStockFoes();
        SendInputs(dt);
        if (!haveSnapshot_) return;
        bool fresh = latest_.tick != measuredTick_;
        measuredTick_ = latest_.tick;
        for (const WillieState& s : latest_.willies) {
            UObject* w = ClientActor(s.entity);
            if (!w) continue;
            PlacePuppetOnce(w, s);
            if (fresh) MeasureDesync(w, s);
            adapter_.DriveTowards(w, s, dt);
            adapter_.ApplyVitals(w, s);
        }
    }

    // Owner sync, client: nothing is spawned before the level's own start-up is over (BP_LevelManager's Clean Up Map
    // destroys every fighter, weapon and armour piece it did not spawn). Probed every 100 ms until it opens; then my
    // dressed fighter replaces the stock pawn and the stand-ins are created.
    bool ClientGateOpen() {
        if (levelGate_.Open()) return true;
        if (NowMs() < nextGateProbe_) return false;
        nextGateProbe_ = NowMs() + 100;
        LevelProbe p;
        std::vector<UObject*> managers = FindInstances("BP_LevelManager_C");
        p.manager = !managers.empty();
        if (p.manager) GetBool(managers.front(), "Player Spawned", p.playerSpawned);
        p.pawn = adapter_.PlayerWillie(0) != nullptr;
        const double now = game::SteadyNowMs();
        if (!levelGate_.Update(now, p)) return false;
        const char* why = levelGate_.why() == LevelSpawnGate::Why::LevelSpawned ? "the level spawned its player"
                        : levelGate_.why() == LevelSpawnGate::Why::NoManager ? "no level manager" : "timed out waiting for the level";
        HSM_LOG("client: level start-up over (%s); spawning fighters", why);
        if (ownDeadPending_) if (const Spawn* sp = world_.Find(world_.myEntity())) OnOwnSpawn(*sp);
        return true;
    }

    // Owner sync, client: a stand-in the game destroyed while its owner is still announced is spawned again
    // (at most every RecreateLimiter::kRetryMs per entity); the servo then snaps it onto its owner's stream.
    void RecoverLostPuppets() {
        const double now = game::SteadyNowMs();
        for (auto it = puppets_.begin(); it != puppets_.end();) {
            const uint8_t e = it->first;
            if (adapter_.StillValid(it->second.obj, it->second.index) || !recreate_.Allow(e, now)) { ++it; continue; }
            standIns_.Detach(e);
            it = puppets_.erase(it);
            if (world_.MarkLost(e)) HSM_LOG("client: entity %u's stand-in was destroyed by the game; re-creating it", e);
        }
    }

    // Owner sync, client side: my fighter is mine to play. Moved once onto the spot the host announced for it (every
    // machine's stock spawner uses the same start), then streamed; every other fighter is a stand-in driven here.
    void ClientOwnerTick(UObject* me, float dt) {
        const double now = game::SteadyNowMs();
        const uint8_t mine = world_.myEntity();
        if (me && !snappedOwn_)
            if (const Spawn* sp = world_.Find(mine)) {
                snappedOwn_ = true;
                const FVector want{sp->x, sp->y, sp->z};
                if (Dist(adapter_.ActorLocation(me), want) > kTeleportDistance) {
                    adapter_.Teleport(me, want);
                    HSM_LOG("client: my fighter moved to its announced spawn");
                }
            }
        std::vector<std::pair<uint8_t, UObject*>> owned;
        if (me && mine && snappedOwn_) owned.push_back({mine, me});
        streams_.Tick(adapter_, now, dt, owned, [this](const std::vector<uint8_t>& b) { if (hostPeer_) net_->Send(hostPeer_, kChanUnreliable, b, false); });
        ReportOwnDeath(me, now);
        standIns_.Tick(adapter_, now, dt, OwnedPelvises(owned));
        claims_.Tick();
        severs_.Tick(now, owned);
        gore_.Tick(now);
        grabs_.Tick(now, dt, owned);
        props_.Tick(now, owned);
        equip_.Tick(now, owned);
    }

    // Once per life: the owner reports its last positive opposing damage within the credit window.
    void ReportOwnDeath(UObject* me, double now) {
        if (!me || !adapter_.IsDead(me)) { if (me) ownDeathReported_ = false; return; }   // alive: a new life
        if (ownDeathReported_ || !hostPeer_) return;
        ownDeathReported_ = true;
        DeathReport r; r.victim = world_.myEntity(); r.killer = claims_.RecentAttacker(r.victim, now);
        net_->Send(hostPeer_, kChanReliable, Encode(r), true);
        HSM_LOG("client: my fighter died; reported to the host (last opposing damage from entity %u, window 60s)", r.killer);
    }

    void SendInputs(float dt) {
        sendAccum_ += dt;
        if (sendAccum_ < 1.f / float(cfg_.inputHz)) return;
        sendAccum_ = 0;
        InputFrame f = capture_.Take(NowMs());
        adapter_.ControlRotation(0, f.controlYaw, f.controlPitch);
        net_->Send(hostPeer_, kChanUnreliable, Encode(f), false);
        ++inputsTx_;
    }

    // How far each body here is from the host's latest pose (hip bone), per fighter, reported every 5 s.
    void MeasureDesync(UObject* w, const WillieState& s) {
        FVector here;
        if (s.bodies.empty() || !adapter_.FirstBodyPosition(w, here)) return;
        const BodyState& b = s.bodies[0];
        double d = Dist(here, {b.px, b.py, b.pz});
        Desync& m = desync_[s.entity];
        m.sum += d; m.max = std::max(m.max, d); ++m.n;
    }

    // Movement and actions go to the host instead of the local body (it follows the host's simulation of it);
    // looking around stays local, and so does the camera lock.
    void BindClientInputHooks() {
        hooksBound_ = true;
        for (const auto& [fn, b] : adapter_.Bindings()) {
            game::InputBinding bind = b;
            HookFunction(fn, [this, bind](UObject* obj, UFunction* f, void* params) {
                if (OwnerMode() || obj != myWillie_.load()) return false;   // owner sync: my input drives my fighter here
                if (bind.isAxis) {
                    Axis a = Axis(bind.index);
                    capture_.OnAxis(a, game::Adapter::ReadAxisParam(f, params));
                    return !(a == Axis::TurnMouse || a == Axis::LookMouse || a == Axis::TurnGamepad || a == Axis::LookGamepad);
                }
                capture_.OnAction(Action(bind.index), bind.pressed);
                return Action(bind.index) != Action::ToggleCameraLock;
            });
        }
        HSM_LOG("client: input capture hooks installed");
    }

    // A puppet's first snapshot can be metres from where it was created: move it there once, hip to hip.
    void PlacePuppetOnce(UObject* w, const WillieState& s) {
        if (s.bodies.empty() || !placed_.insert(s.entity).second) return;
        WillieState here;
        if (!adapter_.ReadState(w, s.entity, here) || here.bodies.empty()) return;
        const BodyState& a = here.bodies[0]; const BodyState& b = s.bodies[0];
        if (Dist({a.px, a.py, a.pz}, {b.px, b.py, b.pz}) <= kTeleportDistance) return;
        FVector at = adapter_.ActorLocation(w);
        adapter_.Teleport(w, {at.x + b.px - a.px, at.y + b.py - a.py, at.z + b.pz - a.pz});
    }

    UObject* ClientActor(uint8_t entity) {
        if (entity == world_.myEntity()) return adapter_.PlayerWillie(0);
        auto it = puppets_.find(entity);
        if (it == puppets_.end()) return nullptr;
        if (!adapter_.StillValid(it->second.obj, it->second.index)) { puppets_.erase(it); return nullptr; }
        return it->second.obj;
    }

    void CreatePuppets() {
        auto have = [this](uint32_t id) { return clientBlobs_.AtLeast(id) != nullptr; };
        for (const Spawn& sp : world_.PendingPuppets(have)) {
            game::Adapter::SpawnOptions o;
            o.blob = sp.blobId ? clientBlobs_.AtLeast(sp.blobId) : nullptr;
            o.at = {sp.x, sp.y, sp.z}; o.yawDeg = sp.yaw; o.teamInt = TeamIntFor(sp.team, sp.entity); o.brain = false;   // follows its owner's pose
            UObject* w = adapter_.SpawnFighter(o);
            if (w) { puppets_[sp.entity].Set(w); QueueTint(w, sp.team); if (OwnerMode()) standIns_.Attach(sp.entity, w); }
            else HSM_LOG("client: puppet for entity %u failed to spawn", sp.entity);
            world_.MarkCreated(sp.entity);           // no retry loop on failure; the next level tries again
        }
    }

    void DestroyPuppet(uint8_t entity) {
        auto it = puppets_.find(entity);
        if (it == puppets_.end()) return;
        if (OwnerMode()) { standIns_.Detach(entity); adapter_.DestroyFighter(it->second.obj, it->second.index); }
        else if (adapter_.StillValid(it->second.obj, it->second.index)) adapter_.DestroyActor(it->second.obj);
        puppets_.erase(it);
    }

    void UploadLoadout() {
        std::vector<uint8_t> bytes;
        if (!adapter_.ExportPassport(bytes) || bytes == lastUpload_) return;
        lastUpload_ = bytes;
        if (++uploadId_ == 0) uploadId_ = 1;
        std::vector<BlobChunk> chunks = SplitBlob(uploadId_, bytes);
        if (chunks.empty()) { HSM_LOG("client: loadout too large to send (%zu bytes)", bytes.size()); return; }
        for (const BlobChunk& c : chunks) net_->Send(hostPeer_, kChanReliable, Encode(c), true);
        HSM_LOG("client: loadout sent (%zu bytes, %zu chunks)", bytes.size(), chunks.size());
    }

    void ClientReceive(const NetEvent& ev) {
        const uint8_t* d = ev.data.data(); size_t n = ev.data.size();
        MsgType t; PeekType(d, n, t);
        switch (t) {
            case MsgType::Welcome: {
                Welcome w; Decode(d, n, w);
                welcomed_ = true; mySlot_ = w.slot; world_.SetMyEntity(w.entity); snapshotHz_ = w.snapshotHz; jitter_.clear();
                syncMode_ = SyncMode(w.syncMode);
                HSM_LOG("client: joined session %u as slot %u (entity %u), %s sync", w.sessionId, w.slot, w.entity, OwnerMode() ? "owner" : "host");
                if (OwnerMode() && !streams_.Started()) StartOwner();   // a repeated Welcome keeps the stream clock
                UploadLoadout();
                break;
            }
            case MsgType::Reject: { Reject r; Decode(d, n, r); HSM_LOG("client: rejected by host (reason %d)", int(r.reason)); break; }
            case MsgType::Travel: {
                Travel tr; Decode(d, n, tr);
                if (tr.seq == travelSeq_ && !currentLevel_.empty() && tr.level.find(currentLevel_) != std::string::npos) {
                    // Already here (the host re-sends its travel after our Hello): confirm instead of reloading.
                    if (hostPeer_) { Ready r; r.travelSeq = travelSeq_; r.level = pendingLevel_; net_->Send(hostPeer_, kChanReliable, Encode(r), true); }
                    break;
                }
                RememberFoes();
                size_t applied = game::ApplyGameSettings(tr.settings);
                travelSeq_ = tr.seq; pendingLevel_ = tr.level;
                HSM_LOG("client: following host to %s (%zu settings applied)", tr.level.c_str(), applied);
                game::OpenLevel(lastGameMode_, tr.level, tr.absolute, tr.options);
                break;
            }
            case MsgType::Snapshot: {
                Snapshot s; Decode(d, n, s);
                ++snapsRx_;
                int64_t at = int64_t(GetTickCount64());
                for (const WillieState& w : s.willies)       // remote fighters render ~100 ms behind, interpolated
                    if (w.entity != world_.myEntity()) jitter_.try_emplace(w.entity, 1000.0 / double(snapshotHz_)).first->second.Push(s.tick, at, w);
                if (!haveSnapshot_ || SeqNewer(s.tick, latest_.tick)) { latest_ = std::move(s); haveSnapshot_ = true; }
                break;
            }
            case MsgType::Spawn: {
                Spawn sp; Decode(d, n, sp);
                if (world_.OnSpawn(sp)) DestroyPuppet(sp.entity);
                if (sp.entity == world_.myEntity()) OnOwnSpawn(sp);
                break;
            }
            case MsgType::Despawn: {
                Despawn dp; Decode(d, n, dp);
                jitter_.erase(dp.entity);                  // a respawn must not slide over from where it died
                placed_.erase(dp.entity);
                if (OwnerMode() && dp.entity != world_.myEntity()) {
                    standIns_.Forget(dp.entity); severs_.Forget(dp.entity); gore_.Forget(dp.entity); claims_.Forget(dp.entity); fx_.Forget(dp.entity); equip_.Forget(dp.entity);
                    grabs_.Forget(dp.entity); props_.ForgetEntity(dp.entity);
                }
                if (world_.OnDespawn(dp.entity)) DestroyPuppet(dp.entity);
                if (dp.entity == world_.myEntity()) ownDeadPending_ = true;   // a respawn follows
                break;
            }
            case MsgType::MatchInfo: { MatchInfo m; Decode(d, n, m); info_ = std::move(m); break; }
            case MsgType::Kill: {
                Kill k; Decode(d, n, k); PushFeed(k);
                if (OwnerMode() && k.victim != world_.myEntity()) standIns_.OnDeath(k.victim);
                break;
            }
            case MsgType::Chat: { ChatLine c; Decode(d, n, c); ui::ShowLine(NameOfSlot(c.fromSlot), c.text); break; }
            case MsgType::WorldState: {
                WorldState ws; Decode(d, n, ws);
                worldView_.Apply(ws);
                for (const WorldObject& o : ws.objects) {
                    if ((o.flags & kWorldRemoved) || !o.owner) continue;
                    if (UObject* w = game::FindClientWeapon(adapter_, o.owner, o.slot, [this](uint8_t e) { return ClientActor(e); }))
                        game::ApplyWorldObject(adapter_, w, o);
                }
                break;
            }
            case MsgType::CombatEvent: {
                CombatEvent e; Decode(d, n, e);
                ApplyCombat(e);                                      // clients resolve no hits: all blood comes from the host
                break;
            }
            case MsgType::BlobChunk: {
                BlobChunk c; Decode(d, n, c);
                std::vector<uint8_t> blob;
                BlobAssembler::Result res = clientAssembler_.Add(c, GetTickCount64(), blob);
                if (res == BlobAssembler::Result::Complete && clientBlobs_.Put(c.blobId, std::move(blob)))
                    HSM_LOG("client: loadout %08x for slot %d received", c.blobId, BlobSlot(c.blobId));
                break;
            }
            case MsgType::Pose: {
                PoseMsg m; sync::PoseFrame frame;
                if (OwnerMode() && Decode(d, n, m, &frame) && m.entity != world_.myEntity()) standIns_.OnPose(m, frame, ArrivalMs(ev));
                break;
            }
            case MsgType::Vitals: {
                VitalsMsg v;
                if (OwnerMode() && Decode(d, n, v) && v.entity != world_.myEntity()) {
                    if (!standIns_.OnVitals(v)) break;
                    gore_.OnVitals(v.entity, v.severed, v.dislocated, game::SteadyNowMs());
                    severs_.OnVitals(v, game::SteadyNowMs());
                }
                break;
            }
            case MsgType::HitClaim: {
                HitClaimMsg m;
                if (OwnerMode() && Decode(d, n, m) && m.claim.victim == world_.myEntity()) claims_.Replay(m.claim, game::SteadyNowMs());
                break;
            }
            case MsgType::HitFx: {
                HitFxMsg m;
                if (OwnerMode() && Decode(d, n, m)) claims_.Effect(m.claim);
                break;
            }
            case MsgType::SeverClaim: {
                combat::SeverClaimMsg m;
                if (!OwnerMode() || !Decode(d, n, m)) break;
                if (m.claim.victim == world_.myEntity()) severs_.Replay(m.claim, game::SteadyNowMs());
                else severs_.Remember(m.claim, game::SteadyNowMs());
                break;
            }
            case MsgType::StuckBlade: {
                combat::StuckBladeMsg m;
                if (OwnerMode() && Decode(d, n, m) && m.victim == world_.myEntity()) severs_.OnStuck(m, game::SteadyNowMs());
                break;
            }
            case MsgType::GrabState: {
                GrabStateMsg m;
                if (OwnerMode() && Decode(d, n, m) && m.grabber != world_.myEntity()) grabs_.OnGrabState(m, game::SteadyNowMs());
                break;
            }
            case MsgType::PaintFx: {
                combat::PaintFxMsg m;
                if (OwnerMode() && Decode(d, n, m) && m.entity != world_.myEntity()) fx_.Apply(m);
                break;
            }
            case MsgType::Equip: {
                combat::EquipStateMsg m;
                if (OwnerMode() && Decode(d, n, m) && m.entity != world_.myEntity()) equip_.Apply(m);
                break;
            }
            case MsgType::PropState: { if (OwnerMode()) props_.OnPropState(d, n, kHostSlot); break; }
            case MsgType::PropLease: { if (OwnerMode()) props_.OnPropLease(d, n, kHostSlot); break; }
            case MsgType::Event: {
                Event e; Decode(d, n, e);
                if (e.kind == EventKind::Died && e.a <= 0xFF)
                    if (UObject* w = ClientActor(uint8_t(e.a))) adapter_.TriggerDeath(w);
                break;
            }
            default: break;
        }
    }

    // Chaos contacts reach Blueprints only through component hit delegates; a client fighting in a session
    // blocks them so damage is computed by the host only.
    void InstallClientSuppression() {
        HookFunctionsMatching([](const std::string& n) { return n.find("ComponentHitSignature") != std::string::npos; },
                              [this](UObject* obj, UFunction* fn, void* params) {
                                  return HitHook(obj, fn, params);
                              });
    }

    Config cfg_;
    game::Lobby lobby_;
    std::unique_ptr<ITransport> net_;
    SteamTransport* steamNet_ = nullptr;
    game::Adapter adapter_;
    game::Mirror mirror_;
    bool selfTested_ = false, selfTestOk_ = false;
    UObject* lastGameMode_ = nullptr;
    int32_t lastGameModeIndex_ = -1;
    std::string currentLevel_;
    float sendAccum_ = 0;
    uint32_t travelSeq_ = 0;
    std::string pendingLevel_;
    // host
    bool hostPending_ = false;
    Roster roster_;
    HostDirector director_;
    BlobStore hostBlobs_;
    std::array<uint32_t, kMaxWillies> blobVersion_{};
    std::vector<uint8_t> hostExport_;
    std::map<uint32_t, Peer> peers_;
    std::array<Fighter, kMaxFighters> fighters_;
    std::optional<Travel> lastTravel_;
    bool picksValid_ = false;
    FVector spawnCenter_;
    std::vector<Point3> spawnCands_;
    std::vector<SpawnPick> picks_;
    uint32_t tick_ = 0, eventSeq_ = 0, sessionId_ = NowMs();
    // match (host)
    struct Tint { ActorRef actor; int8_t team = kNoTeam; int tries = 0; uint32_t next = 0; };
    Match match_;
    KillLog kills_;
    std::map<uint8_t, int8_t> slotTeam_;
    std::map<uint8_t, ActorRef> bots_;
    UObject* hostWillie_ = nullptr;
    bool adoptStock_ = false, wavesEnabled_ = false, sidesValid_ = false, infoDirty_ = false, progression_ = false;
    int wave_ = 0;
    uint32_t nextCorpseSweep_ = 0;
    uint32_t nextScan_ = 0, nextInfo_ = 0;
    std::vector<Point3> sidePoints_;
    std::vector<int8_t> sides_;
    // match (both)
    MatchInfo info_;
    std::deque<std::pair<uint32_t, std::string>> feed_;
    std::vector<Tint> tints_;
    bool ownDeadPending_ = false;
    uint32_t ownDiedAt_ = 0, nextOwnRespawn_ = 0;
    // client
    uint32_t hostPeer_ = 0, uploadId_ = 0;
    bool welcomed_ = false, haveSnapshot_ = false, snappedOwn_ = false;
    uint8_t mySlot_ = 0;
    ClientWorld world_;
    BlobStore clientBlobs_;
    BlobAssembler clientAssembler_;
    std::vector<uint8_t> lastUpload_;
    std::map<uint8_t, ActorRef> puppets_;
    LevelSpawnGate levelGate_;                   // owner sync: the level's start-up cleanup is over (hsm_level_gate.h)
    uint32_t nextGateProbe_ = 0;
    bool stockFlowGuard_ = false;                    // GuardStockFlow's hooks block while set (per tick)
    uint32_t stockFlowBlocked_ = 0, unpaused_ = 0;
    RecreateLimiter recreate_;                   // owner sync: lost stand-ins, both sides
    std::set<uint8_t> lostSlots_;                // host, owner sync: client stand-ins the game destroyed
    InputCapture capture_;
    Snapshot latest_;
    std::atomic<UObject*> myWillie_{nullptr};
    std::set<uint8_t> placed_;                 // client: bodies already moved to their first snapshot
    struct Desync { double sum = 0, max = 0; uint32_t n = 0; };
    std::map<uint8_t, Desync> desync_;
    uint32_t inputsTx_ = 0, measuredTick_ = 0, bloodSent_ = 0, bloodApplied_ = 0, hitsBlocked_ = 0;
    bool hooksBound_ = false;
    std::map<uint8_t, uint32_t> lastBloodAt_;
    std::unordered_map<UFunction*, int32_t> otherActorOffset_;
    bool inFight_ = false;          // InFightLevel() cached per tick for the hit hook
    HostLossMonitor hostLoss_{0};
    ReservationTable reservations_;
    SeveredMasks severedSent_;
    WorldTracker worldTracker_;
    WorldView worldView_;
    uint32_t worldEvery_ = 0;
    double groundOffset_ = 0;
    bool groundValid_ = false, foesStopped_ = false;
    int savedFoes_ = -1;
    uint32_t lastFoeSweep_ = 0;
    uint32_t inputFixAt_ = 0, nextHealth_ = 0, snapsRx_ = 0, nextHello_ = 0, hellosSent_ = 0;
    bool inputFixRepeat_ = false;
    std::map<uint8_t, JitterBuffer<32>> jitter_;
    uint16_t snapshotHz_ = 30;
    SpectateCycler spectate_;
    uint8_t spectating_ = 0;
    UObject* spectateActor_ = nullptr;
    bool spectateKeyDown_ = false;
    // agent bridge / auto-run
    std::array<Held, kAxisCount> held_{};
    bool quitRequested_ = false, automated_ = false;
    uint32_t startedAt_ = 0, quitAfterMs_ = 0, nextAuto_ = 0, lastModeTick_ = 0, lastEarlyPump_ = 0, noPawnSince_ = 0;
    static constexpr uint32_t kLocalFighterGraceMs = 15000;   // longer than the stock spawner's latent delay
    std::string autoLevel_;
    // owner sync (both roles)
    SyncMode syncMode_ = SyncMode::Host;
    game::OwnerStreams streams_;
    game::StandIns standIns_;
    game::Claims claims_;
    game::FxSync fx_;
    game::EquipSync equip_;
    game::GrabSync grabs_;
    game::PropSync props_;
    game::Severs severs_;
    game::StandInGore gore_;
    struct OwnerLife { bool aliveSeen = false; uint32_t deadSince = 0; };
    std::map<uint8_t, OwnerLife> lives_;       // host: client fighters as their vitals tell it
    bool ownDeathReported_ = false;
    uint32_t nextKeepAlive_ = 0, streamsDropped_ = 0, claimsRejected_ = 0;
    static constexpr size_t kFrameSamples = 4096;   // 5 s of frames at up to ~800 fps; the rest of a window is not sampled
    std::vector<double> frameMs_;
};

std::unique_ptr<SessionImpl> g_session;
UClass* g_gameModeClass = nullptr;
}  // namespace

bool StartMod(const Config& cfg) {
    if (!Init(cfg.testForceScan && (cfg.role == Role::Host || cfg.role == Role::Client))) return false;
    g_session = std::make_unique<SessionImpl>(cfg);
    if (!g_session->Start()) { g_session.reset(); return false; }
    // Pump the session from the GameMode's Blueprint tick (game thread, every frame, menus included).
    // Widgets keep ticking while the game is paused: let them pump the agent bridge too.
    HookFunctionsMatching([](const std::string& n) { return n == "Tick"; }, [](UObject*, UFunction*, void*) { g_session->PumpAgentEarly(); return false; });
    HookFunctionsMatching([](const std::string& n) { return n == "ReceiveTick"; }, [](UObject* obj, UFunction* fn, void* params) {
        if (g_session->IsTickBlocked(obj)) return true;   // a stand-in: never ticks
        static int seenClasses = 0;
        if (!g_gameModeClass && seenClasses < 12) {     // startup diagnostics: which Blueprints tick before the game mode
            static std::vector<UClass*> seen;
            UClass* c = ClassOf(obj);
            if (std::find(seen.begin(), seen.end(), c) == seen.end()) { seen.push_back(c); ++seenClasses; HSM_LOG("session: first tick of %s", ObjectName(reinterpret_cast<UObject*>(c)).c_str()); }
        }
        if (!g_gameModeClass) {
            UClass* c = ClassOf(obj);
            if (ObjectName(reinterpret_cast<UObject*>(c)) != kGameModeClass) { g_session->PumpAgentEarly(); return false; }
            g_gameModeClass = c;
        }
        if (ClassOf(obj) != g_gameModeClass) { g_session->PumpAgentEarly(); return false; }
        float dt = 0.f;
        if (FProperty* p = FindProperty(fn, "DeltaSeconds")) std::memcpy(&dt, static_cast<uint8_t*>(params) + PropertyOffset(p), sizeof dt);
        if (!(dt > 0.f && dt < 1.f)) dt = 1.f / 60.f;
        g_session->Tick(obj, dt);
        return false;
    });
    if (!InstallProcessEventHook()) { RemoveProcessEventHook(); ClearHooks(); g_session.reset(); return false; }
    if (int port = CmdArgInt(CommandLineUtf8(), "hsmbridge", 0, 1024, 65535)) agent::Start(uint16_t(port));
    HSM_LOG("session: started (%s)", RoleName(cfg.role));
    return true;
}

}  // namespace hsm

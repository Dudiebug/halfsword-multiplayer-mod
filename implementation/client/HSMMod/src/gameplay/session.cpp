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
#include "util/hsm_names.h"
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
#include "match/hsm_hud_scene.h"
#include "match/hsm_maps.h"
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
#include <chrono>
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
constexpr double kNoSpawnerRingCm = 400.0;       // spawner-less maps: candidates on a ring this far around the host
constexpr double kSideRingCm = 900.0;            // spawner-less maps: team sides on a ring this far around the host
constexpr double kTeleportDistance = 300.0;
constexpr size_t kHitImpactPointOffset = 40;     // FHitResult: FaceIndex, Time, Distance, Location(24) then ImpactPoint           // off: first live test showed puppets missing on that path      // client: snap its own fighter on the first snapshot if farther
constexpr uint8_t kFirstBotEntity = kMaxFighters + 1, kLastBotEntity = uint8_t(kMaxWillies);
static_assert(Match::kMaxBotsAlive <= kLastBotEntity - kFirstBotEntity + 1, "every live bot needs an entity");
constexpr uint32_t kInfoIntervalMs = 1000, kScanIntervalMs = 1000, kTintRetryMs = 500;
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

// What a level gate that timed out was still waiting for, in log words.
const char* GateWaitingName(LevelSpawnGate::Waiting w) {
    switch (w) {
        case LevelSpawnGate::Waiting::Manager: return "a level manager";
        case LevelSpawnGate::Waiting::PlayerSpawnedCleared: return "\"Player Spawned\" to clear";
        case LevelSpawnGate::Waiting::PlayerSpawned: return "\"Player Spawned\"";
        case LevelSpawnGate::Waiting::Pawn: return "a player pawn";
        case LevelSpawnGate::Waiting::Nothing: break;
    }
    return "nothing";
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
        case MsgType::DeathReport: { DeathReport m; return Decode(d, n, m); }
        case MsgType::GiveUp: { GiveUpMsg m; return Decode(d, n, m); }
        case MsgType::HitClaim: { HitClaimMsg m; return Decode(d, n, m); }
        case MsgType::SeverClaim: { combat::SeverClaimMsg m; return Decode(d, n, m); }
        case MsgType::SeverNotice: { combat::SeverNoticeMsg m; return Decode(d, n, m); }
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
        case MsgType::SeverNotice: { combat::SeverNoticeMsg m; return Decode(d, n, m); }
        case MsgType::StuckBlade: { combat::StuckBladeMsg m; return Decode(d, n, m); }
        case MsgType::GrabState: { GrabStateMsg m; return Decode(d, n, m); }
        case MsgType::PropState: { PropStateMsg m; return Decode(d, n, m); }
        case MsgType::PropLease: { PropLeaseMsg m; return Decode(d, n, m) && m.action == LeaseAction::Grant; }
        case MsgType::PropCensus: { PropCensusMsg m; return Decode(d, n, m); }
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
        ui::ChatSetup(cfg_.chatKey, [this](bool on) { adapter_.CaptureInput(on); }, [this] { return adapter_.GamePaused(); });
    }

    bool Start() {
        if (cfg_.role == Role::Steam) return true;   // transport is created when a lobby is entered
        auto enet = std::make_unique<ENetTransport>();
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
    // Wall time of one Tick (the pump): our own share of a frame, against the frame time the game mode reports.
    void NoteTickCost(double ms) {
        if (!sampling_ || tickMs_.size() >= kFrameSamples) return;
        if (tickMs_.empty()) {   // the window starts: VM and ProcessEvent hook time from here
            vmNsAtWindow_ = GetScriptHookStats().hookNs;
            const ProcessEventHookStats pe = GetProcessEventHookStats();
            peNsAtWindow_ = pe.hookNs;
            peCallsAtWindow_ = pe.calls;
            peAllAtWindow_ = pe.all;
            const ReflectionCacheStats rc = GetReflectionCacheStats();
            lookupsAtWindow_ = rc.hits + rc.misses;
            lookupMissesAtWindow_ = rc.misses;
        }
        tickMs_.push_back(ms);
    }

    // The pump's cost by part, summed over the frames of a health window (as tickMs_ samples them): which of our own
    // parts grows when the frames slow down. Each part's time includes the game code it runs (a replayed claim's Get
    // Damage counts under claims, a stand-in's native cut under severs or net).
    enum PumpPart : uint8_t { kPumpNet, kPumpStreams, kPumpServo, kPumpClaims, kPumpSevers, kPumpGore, kPumpGrabs, kPumpProps,
                              kPumpEquip, kPumpMatch, kPumpParts };
    template <class F> void Timed(PumpPart part, F&& f) {
        if (!sampling_ || tickMs_.size() >= kFrameSamples) { f(); return; }
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        pumpMs_[part] += ms;
        pumpMaxMs_[part] = std::max(pumpMaxMs_[part], ms);
    }
    void ClearPumpCost() { pumpMs_.fill(0); pumpMaxMs_.fill(0); }

    // " | pump ms/frame: net A streams B ... (max P x ms)": P is the part with the longest single run in the window.
    std::string PumpLine(double frames) const {
        static constexpr const char* kNames[kPumpParts] = {"net", "streams", "servo", "claims", "severs", "gore", "grabs",
                                                           "props", "equip", "match"};
        std::string line = " | pump ms/frame:";
        char b[48];
        size_t worst = 0;
        for (size_t i = 0; i < kPumpParts; ++i) {
            std::snprintf(b, sizeof b, " %s %.2f", kNames[i], pumpMs_[i] / frames);
            line += b;
            if (pumpMaxMs_[i] > pumpMaxMs_[worst]) worst = i;
        }
        std::snprintf(b, sizeof b, " (max %s %.1f ms)", kNames[worst], pumpMaxMs_[worst]);
        return line + b;
    }

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

    // Owner sync: the pause menu pauses the world (Esc: Willie_BP.cpp:11091 opens UI_Pause_Eng, whose Construct pauses),
    // which stops the GameMode tick and with it this player's stream. The others saw them freeze, and a client whose host
    // paused went back to the main menu after 7 s (logs 4, 7, 8). The check in Tick cannot see a pause that outlasts the
    // frame; widgets still tick while paused (UI_Pause_Eng has its own Tick), so one of them lifts it. The menu stays
    // open. Esc while it is open: Willie's pause action (it checks only IsGamePaused) opens a second menu, but both
    // menus' Ticks see Esc held that frame and close, so Esc still closes the menu.
    void LiftPauseFromWidgetTick() {
        const uint32_t now = NowMs();
        if (!pauseLiftArmed_ || now - lastModeTick_ < 200 || now - lastPauseCheck_ < 50) return;
        lastPauseCheck_ = now;
        if (!(lastGameMode_ && ObjectAt(lastGameModeIndex_) == lastGameMode_ && adapter_.World() == lastGameMode_)) return;
        if (!adapter_.GamePaused()) return;
        adapter_.SetGamePaused(false);
        if (++unpaused_ % 100 == 1) HSM_LOG("session: game paused (pause menu?) during an owner-sync match; unpaused (%u time(s))", unpaused_);
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
        lastModeTick_ = NowMs();
        fighterOfFrame_.clear();
        spawnIgnoreState_ = SpawnIgnoreState::InTick;
        struct EndOfTick { SpawnIgnoreState& s; ~EndOfTick() { s = SpawnIgnoreState::Outside; } } endOfTick{spawnIgnoreState_};
        sampling_ = net_ && InFightLevel();                               // sampled only where the health line is logged
        if (!sampling_) { frameMs_.clear(); tickMs_.clear(); ClearPumpCost(); }
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
        }
        if (!selfTestOk_) return;
        if (steamMode()) lobby_.Tick();
        // Level change first: everything after this point must use the current world, never the old game mode.
        bool levelChanged = gameMode != lastGameMode_;
        if (levelChanged) FlushReflectionCache();     // the old level's classes are gone (again each retry: harmless)
        if (!adapter_.Resolve(gameMode)) return;     // retried next tick, with the level change still pending
        if (levelChanged) { lastGameMode_ = gameMode; lastGameModeIndex_ = ObjectIndex(gameMode); OnLevelLoaded(); }
        inFight_ = InFightLevel();
        if (hostPending_) { hostPending_ = false; BecomeHost(); }
        agent::Tick([this](const AgentCommand& c) { return HandleAgent(c); });
        AutoRun();
        if (!inFight_) ui::Tick(*this, gameMode);   // the main menu exists only in menu levels (its scan walks every object)
        EnsureLocalFighter();
        test::Tick(adapter_, InFightLevel(), dt);
        ApplyHeldInput();
        if (mirror_.Active() && net_) mirror_.Command({"off"}, adapter_, true, InFightLevel());   // offline bench only
        mirror_.Tick(adapter_, dt);
        bool joined = net_ && (steamMode() ? lobby_.InLobby() : true);
        adapter_.SetDestroyAware(net_ && OwnerMode());
        adapter_.SetSubclassWeapons(OwnerMode());   // offline too, so the mirror bench sees the weapons owner sync sees
        stockFlowGuard_ = joined && MatchLevel() && OwnerMode();
        pauseLiftArmed_ = stockFlowGuard_;
        adapter_.SuppressStockScreens(joined && MatchLevel(), OwnerMode());
        if (stockFlowGuard_ && adapter_.GamePaused()) {   // owner sync: a paused game stops this player's stream
            adapter_.SetGamePaused(false);
            if (++unpaused_ % 100 == 1) HSM_LOG("session: game was paused during an owner-sync match; unpaused (%u time(s))", unpaused_);
        }
        if (!net_) { ui::HudTick({}); ui::ChatDrop("session over"); return; }
        if (IsHost()) HostTick(dt); else ClientTick(dt);
        TintTick();
        // Twice: after the level's own setup, then once more; never while the chat holds the keyboard (it gives it back).
        if (inputFixAt_ && NowMs() >= inputFixAt_ && !ui::Typing()) {
            adapter_.RestoreGameInput();
            HSM_LOG("session: game input mode restored");
            inputFixAt_ = inputFixRepeat_ ? 0 : NowMs() + 3000; inputFixRepeat_ = !inputFixRepeat_;
        }
        if (net_) SpectateTick();
        if (net_ && InFightLevel() && NowMs() >= nextHealth_) { nextHealth_ = NowMs() + 5000; LogHealth(); }
        if (info_.phase != chatPhase_) {   // the results: a line being typed is dropped, a new one may be opened
            if (info_.phase == uint8_t(MatchPhase::Results)) ui::ChatDrop("match over");
            chatPhase_ = info_.phase;
        }
        GiveUpTick();
        DropsTick();
        HudScene hud = BuildHud();
        ui::HudTick(hud);
        ui::ChatTick(hud);
        if (std::optional<std::string> line = ui::TakeOutgoing(); line && net_) SendChat(*line);
    }

    // One line every 5 s in a session fight: enough to tell "no input arriving" from "input arriving, nothing moves".
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
        HSM_LOG("%s: %s", who, CensusLine().c_str());
        if (std::string line = standIns_.HealthLine(); !line.empty()) HSM_LOG("%s: %s", who, line.c_str());
        HSM_LOG("%s: %s", who, claims_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, fx_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, equip_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, severs_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, gore_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, grabs_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, props_.HealthLine().c_str());
    }

    // Every fighter in the level and its state, next to the frame time: what grows when the game slows down (bodies left
    // behind, fighters on the ground or bleeding). Two object scans per 5 s window; their time ends the line.
    std::string CensusLine() {
        const auto t0 = std::chrono::steady_clock::now();
        int total = 0, dead = 0, fallen = 0, out = 0, stand = 0;
        float bleeding = 0;
        for (UObject* w : adapter_.AllWillies()) {
            ++total;
            if (standIns_.IsStandIn(w)) ++stand;
            VitalsMsg v;
            if (!adapter_.ReadVitals(w, 0, v)) continue;
            dead += (v.flags & 1) ? 1 : 0;
            fallen += (v.flags & 2) ? 1 : 0;
            out += (v.flags & 4) ? 1 : 0;
            bleeding += v.bleeding;
        }
        const size_t weapons = adapter_.WeaponActors().size();
        const double scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char b[256];
        std::snprintf(b, sizeof b, "census: fighters %d (stand-ins %d) dead %d fallen %d knocked out %d, bleeding total %.2f; "
                      "weapons %zu, objects %d; stand-in timeline calls blocked %u; scan %.1f ms",
                      total, stand, dead, fallen, out, double(bleeding), weapons, ObjectCount(), timelinesBlocked_, scanMs);
        timelinesBlocked_ = 0;
        std::string line = b;
        VitalsMsg mine;
        if (UObject* me = adapter_.PlayerWillie(0); me && adapter_.ReadVitals(me, 0, mine)) {
            std::snprintf(b, sizeof b, " | mine:%s%s%s health %.0f bleeding %.2f", (mine.flags & 1) ? " dead" : "",
                          (mine.flags & 2) ? " fallen" : "", (mine.flags & 4) ? " knocked out" : "", double(mine.health), double(mine.bleeding));
            line += b;
        }
        return line;
    }

    // Frame time (the game mode's DeltaSeconds) since the previous call, and the round trip to every peer: a long frame
    // and a slow link both look like lag on the other screen; this line tells them apart.
    std::string FrameAndPingLine() {
        char b[160];
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
        // Our share: the pump (stand-in servo, streams, our replays of claims including the game code they run), the VM
        // hook callbacks (claims, fx, ...) and the other ProcessEvent hooks (the hit hook on every contact, the tick and
        // timeline filters; the pump runs inside one of them and is taken out there, DiscountHookTime). Not counted: the
        // detours' lookup for calls nothing hooks. The rest of a frame is the game's own (physics, ragdolls, blood, rendering).
        if (!tickMs_.empty()) {
            double sum = 0, mx = 0;
            for (double v : tickMs_) { sum += v; mx = std::max(mx, v); }
            const double frames = double(tickMs_.size());
            const ScriptHookStats vm = GetScriptHookStats();
            const double vmMs = double(vm.hookNs - vmNsAtWindow_) / 1e6 / frames;
            const ProcessEventHookStats pe = GetProcessEventHookStats();
            const double peMs = double(pe.hookNs - peNsAtWindow_) / 1e6 / frames;
            const double peCalls = double(pe.calls - peCallsAtWindow_) / frames;
            const double peAll = double(pe.all - peAllAtWindow_) / frames;
            std::snprintf(b, sizeof b, " | mod tick avg %.2f max %.1f ms, vm hooks %.2f ms/frame, other hooks %.2f ms/frame (%.0f calls of %.0f)",
                          sum / frames, mx, vmMs, peMs, peCalls, peAll);
            line += b;
            // Which hooks (two-player test 13: 65 ms/frame at 1100 calls late in a match; two-player logs 6 to 9: 2 to 3.5 ms
            // in fights, from 15 to 25 us a call).
            if (peMs >= 1.0) line += " [top hooks: " + TopHookFunctions(3, frames) + "]";
            else TopHookFunctions(0, frames);
            const ReflectionCacheStats rc = GetReflectionCacheStats();   // FindFunction/FindProperty: misses walk names
            std::snprintf(b, sizeof b, ", lookups %.0f/frame (%.1f walked)", double(rc.hits + rc.misses - lookupsAtWindow_) / frames,
                          double(rc.misses - lookupMissesAtWindow_) / frames);
            line += b;
            line += PumpLine(frames);
        }
        tickMs_.clear();
        ClearPumpCost();
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
        if (IsHost()) { net_->Broadcast(kChanReliable, Encode(c), true); ui::ShowLine(MyName(), ChatNameInk(info_, 1), c.text); }
        else if (hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(c), true);
    }

    // A stand-in (the mirror bench's or a session's) whose own Willie ReceiveTick must not run: it re-arms the muscles,
    // pain and damage the stand-in's neutralisation switched off.
    bool IsTickBlocked(const UObject* o) const { return o && (o == mirror_.TickBlocked() || standIns_.IsStandIn(o)); }
    bool BlockStandInTimeline(const UObject* o) {
        if (!IsTickBlocked(o)) return false;
        ++timelinesBlocked_;
        return true;
    }

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
        return "{\"entity\":" + std::to_string(entity) + ",\"pos\":" + Vec3(adapter_.ActorLocation(w)) + ",\"dead\":" +
               (adapter_.IsDead(w) ? "true" : "false") + ",\"health\":" + std::to_string(hp ? int(*hp) : -1) + "}";
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
    // An owner-sync client waits for its level gate instead (the gate's exit spawns the announced fighter) and is
    // only the short fallback after it (LocalFighterGrace).
    void EnsureLocalFighter() {
        if (!(net_ || automated_) || !InFightLevel() || adapter_.PlayerWillie(0)) { noPawnSince_ = 0; return; }
        double graceMs = 0;
        if (!LocalFighterGrace(net_ && !IsHost() && OwnerMode(), levelGate_.Open(), graceMs)) { noPawnSince_ = 0; return; }
        uint32_t now = NowMs();
        if (!noPawnSince_) { noPawnSince_ = now; return; }
        if (double(now - noPawnSince_) < graceMs) return;
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
        HSM_LOG("session: no player pawn in %s after %.0f ms; spawning the local fighter", currentLevel_.c_str(), graceMs);
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
        bots_.clear(); match_ = Match{}; kills_ = KillLog{}; info_ = MatchInfo{}; feed_.clear(); banner_.Reset(); tints_.clear();
        ownDeadPending_ = false;
        mirrorBlob_.clear(); ownBlob_ = 0;
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
        standIns_.Clear(); grabs_.Clear(); streams_.Clear(); lives_.clear(); respawnQueue_.clear();
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
        pauseLiftArmed_ = false;                       // the old level's world is going away
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
        ui::ChatDrop("level change");   // the new level's controller starts with the game's input
        agent::VmHookFlush();
        mirror_.OnLevel();
        if (ScriptHookActive()) ResolveScriptHooks();   // the level's Blueprint classes and overrides are loaded now
        haveSnapshot_ = false; snappedOwn_ = false; picksValid_ = false;
        held_.fill(Held{});                          // agent-held axes do not carry into the next level
        groundValid_ = false; foesStopped_ = false;
        oneFight_ = InFightLevel() && !adapter_.FreeModeActive();   // a client has the host's GI settings (Travel) by now
        if (!InFightLevel()) RestoreFoes();
        inputFixAt_ = net_ && InFightLevel() ? NowMs() + 1500 : 0;
        severedSent_.clear(); worldTracker_ = WorldTracker{}; worldView_ = WorldView{}; jitter_.clear(); placed_.clear();
        hitKind_.clear(); fighterOfFrame_.clear(); otherActorOffset_.clear();   // the old level's classes and functions are gone
        spawnIgnore_.clear();
        // Host side: every actor of the old level is gone.
        for (Fighter& f : fighters_) { f.actor.Clear(); f.input.ReleaseAll(); f.wasDead = false; }
        director_.OnLevel(InFightLevel(), NowMs(), travelSeq_);
        bots_.clear(); match_ = Match{}; kills_ = KillLog{}; sidesValid_ = false; wave_ = 0; info_ = MatchInfo{}; feed_.clear(); banner_.Reset();
        tints_.clear();
        ownDeadPending_ = false; ownDiedAt_ = 0; mirrorBlob_.clear(); ownBlob_ = 0;
        // Client side: puppets died with the level; the host re-announces them after Ready.
        world_.OnLevel(); puppets_.clear();
        levelGate_.OnLevel(game::SteadyNowMs()); nextGateProbe_ = 0; managerProbed_ = false; recreate_.Clear(); lostSlots_.clear();
        // Owner sync, client: the stock pawn the level gives player 0 is replaced by the dressed fighter the host
        // announces, once the level's own start-up cleanup is over (ClientGateOpen).
        if (net_ && !IsHost() && OwnerMode() && InFightLevel()) ownDeadPending_ = true;
        // Owner sync: every stand-in died with the level (the feeds stay: their owners keep streaming).
        for (uint8_t e = 1; e <= kMaxWillies; ++e) { standIns_.Detach(e); severs_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); }
        gore_.Clear();
        grabs_.Clear();
        props_.Clear();                              // every prop actor died with the level
        lives_.clear(); ownDeathReported_ = false; respawnQueue_.clear();
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

    // Owner sync on this machine: a fresh stream clock, and the claims hooks with what they need to know of the session.
    void StartOwner() {
        streams_.Start(game::SteadyNowMs());
        GuardStockFlow();
        game::Claims::Env env;
        env.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        env.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        env.send = [this](const combat::HitClaim& c) {
            if (IsHost()) RouteHostClaim(kHostSlot, c);
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(HitClaimMsg{c}), true);
        };
        env.allowed = [this](uint8_t attacker, uint8_t victim) {
            return HitAllowed(true, KnownTeam(attacker), KnownTeam(victim), IsHost() ? match_.config().friendlyFire : info_.friendlyFire);
        };
        env.prop = [this](UObject* actor, game::PropSync::Touch& out) { return props_.PropOf(actor, out); };
        env.propActor = [this](uint64_t id) { return props_.ActorOf(id); };
        env.myEntity = [this]() { return IsHost() ? uint8_t(1) : world_.myEntity(); };
        if (!claims_.Start(adapter_, standIns_, streams_, std::move(env)))
            HSM_LOG("session: WARNING owner sync without the script VM hook: blows between this machine and stand-ins do nothing");
        // Blood and wounds on fighters simulated here, shown on every other machine's stand-in of them.
        game::FxSync::Env fenv;
        fenv.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        fenv.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        fenv.send = [this](const combat::PaintFxMsg& m) {
            if (!net_) return;
            if (IsHost() && combat::IsStripRequest(m)) {   // to the fallen fighter's owner only
                if (std::optional<uint32_t> to = PeerOfSlot(OwnerSlotOf(m.entity))) net_->Send(*to, kChanReliable, Encode(m), true);
            } else if (IsHost()) net_->Broadcast(kChanReliable, Encode(m), true);
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
        sev.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        sev.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        sev.sendSever = [this](const combat::SeverClaim& c) {
            if (IsHost()) RouteHostSever(kHostSlot, c);
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(combat::SeverClaimMsg{c}), true);
        };
        sev.sendNotice = [this](const combat::SeverClaim& c) {
            if (IsHost()) RouteHostNotice(kHostSlot, c);
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(combat::SeverNoticeMsg{c}), true);
        };
        sev.sendStuck = [this](const combat::StuckBladeMsg& m) {
            if (IsHost()) RouteHostStuck(kHostSlot, m);
            else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(m), true);
        };
        sev.allowed = [this](uint8_t attacker, uint8_t victim) {
            return HitAllowed(true, KnownTeam(attacker), KnownTeam(victim), IsHost() ? match_.config().friendlyFire : info_.friendlyFire);
        };
        sev.nativeShown = [this](uint8_t e, uint8_t part) { gore_.NoteNativeSever(e, part); };
        sev.nativeLost = [this](uint8_t e, uint8_t part) { gore_.NoteNativeLost(e, part); };
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
        // Stand-in hands matched to their owners' poses (pickups, leftover weapons): Equip's view of each hand, gore's cut-off hands.
        penv.equipToken = [this](uint8_t e, int hand, uint8_t& token) { return equip_.OwnerToken(e, hand, token); };
        penv.mayBeDraw = [this](uint8_t e, uint32_t cls) { return equip_.MayBeDraw(e, cls); };
        penv.mayBeSheathe = [this](uint8_t e, int hand) { return equip_.MayBeSheathe(e, hand); };
        penv.deadBones = [this](uint8_t e) { return gore_.DeadBones(e); };
        penv.handsChanged = [this](uint8_t e) { equip_.Replan(e); };
        // Level props: the host censuses a fight level from its first tick in it; a client matches once its level gate is open.
        penv.fightLevel = [this] { return InFightLevel() && (IsHost() || levelGate_.Open()); };
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
        Timed(kPumpNet, [&] {
            NetEvent ev;
            while (net_->Poll(ev)) {
                if (ev.kind == NetEvent::Connected) HSM_LOG("host: peer %u connected", ev.peer);
                else if (ev.kind == NetEvent::Disconnected) OnClientLeft(ev.peer);
                else HostReceive(ev);
            }
        });
        for (uint8_t slot : reservations_.Expire(NowMs())) {
            bool back = std::any_of(peers_.begin(), peers_.end(), [&](const auto& kv) { return kv.second.slot == slot; });
            if (const RosterMember* m = roster_.BySlot(slot); m && !back) { HSM_LOG("host: slot %u did not rejoin in time", slot); RemoveMember(slot, m->key); }
        }
        if (director_.fight()) SpawnDue();
        if (!OwnerMode()) for (uint8_t slot = 1; slot < kMaxFighters; ++slot) DriveRemoteFighter(slot, dt);
        Timed(kPumpMatch, [&] { HostMatchTick(); });
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
    // GrabState and PropSync's leases; level props on the host's census and the same leases.
    void OwnerHostTick(float dt) {
        const double now = game::SteadyNowMs();
        std::vector<std::pair<uint8_t, UObject*>> owned;
        if (UObject* me = myWillie_.load()) owned.push_back({uint8_t(1), me});
        for (auto& [e, ref] : bots_) if (adapter_.StillValid(ref.obj, ref.index)) owned.push_back({e, ref.obj});
        Timed(kPumpStreams, [&] {
            streams_.Tick(adapter_, now, dt, owned, [this](const std::vector<uint8_t>& b) { net_->Broadcast(kChanUnreliable, b, false); });
        });
        Timed(kPumpServo, [&] {
            standIns_.Tick(adapter_, now, dt, OwnedPelvises(owned));
            RecoverLostStandIns(now);
        });
        Timed(kPumpClaims, [&] { claims_.Tick(now); });
        Timed(kPumpSevers, [&] { severs_.Tick(now, owned); });
        Timed(kPumpGore, [&] { gore_.Tick(now); });
        Timed(kPumpGrabs, [&] { grabs_.Tick(now, dt, owned); });
        Timed(kPumpProps, [&] { props_.Tick(now, owned); });
        Timed(kPumpEquip, [&] { equip_.Tick(now, owned); });
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
            standIns_.OnVitals(v);
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
    // friendly-fire rule, credited for the kill log, then replayed by the victim's owner and shown to everyone else.
    void RouteHostClaim(uint8_t senderSlot, const combat::HitClaim& c) {
        const ClaimRoute r = RouteClaim(senderSlot, c.attacker, c.victim, ConnectedSlots());
        if (!r.accept || !HitAllowed(true, KnownTeam(c.attacker), KnownTeam(c.victim), match_.config().friendlyFire)) { ++claimsRejected_; return; }
        kills_.OnHit(c.victim, c.attacker, NowMs());
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

    // A sever the owner's fighter took natively (combat::SeverNoticeMsg; the sender owns that fighter): every other
    // machine keeps it as it keeps a claim, until the owner's vitals confirm the part. Nobody replays it.
    void RouteHostNotice(uint8_t senderSlot, const combat::SeverClaim& c) {
        const std::vector<uint8_t> msg = Encode(combat::SeverNoticeMsg{c});
        for (uint8_t slot : RelayTargets(senderSlot, ConnectedSlots()))
            if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, msg, true);
        if (senderSlot != kHostSlot) severs_.Remember(c, game::SteadyNowMs());
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
    void StopStockFoes() {
        if (foesStopped_ || !InFightLevel() || IsBotFormatHere()) return;
        foesStopped_ = true;
        adapter_.SetFreeModeFoes(0);
        HSM_LOG("session: stock Free Mode foes stopped for this fight");
    }
    void RestoreFoes() { if (savedFoes_ >= 0) { adapter_.SetFreeModeFoes(savedFoes_); savedFoes_ = -1; } }
    bool IsBotFormatHere() const { return IsHost() ? IsBotFormat(match_.config().format) || adoptStock_ : IsBotFormat(Format(info_.format)); }

    // Spawn height: the stock spawner put the host's body at the right height above its floor; every spawn point
    // gets that same height above the floor under it (spawner actors sit at varying heights, so players popped out of
    // the ground).
    // The trace looks through every fighter and what it carries (test 5: a respawn landed on top of the fighter standing
    // at the spot); the stand height is measured once per level and never negative (SaneStandOffset; the Abyss read
    // -96 cm), so a spawn is never put below the floor it traced. Unchanged when a trace fails.
    FVector Grounded(FVector at, const std::vector<UObject*>& ignore) {
        UObject* host = adapter_.PlayerWillie(0);
        if (!groundValid_) {
            double fz;
            std::string hit;
            if (!host || !adapter_.FloorZ(adapter_.ActorLocation(host), ignore, fz, &hit)) return at;
            const double raw = adapter_.ActorLocation(host).z - fz;
            groundOffset_ = SaneStandOffset(raw); groundValid_ = true;
            if (groundOffset_ != raw)
                HSM_LOG("host: the floor under the host traced %.0f cm below it in %s (hit %s); fighters stand %.0f cm above the floor",
                        raw, currentLevel_.c_str(), hit.c_str(), groundOffset_);
            else HSM_LOG("host: fighters stand %.0f cm above the floor in %s (hit %s)", groundOffset_, currentLevel_.c_str(), hit.c_str());
        }
        double fz;
        if (adapter_.FloorZ(at, ignore, fz)) at.z = fz + groundOffset_;
        return at;
    }

    // What a spawn's floor traces look through (Adapter::BodiesAndGear). That walks the object array, so the spawns of
    // one Tick (a round start, a wave) share one walk; never kept past it (garbage collection runs between frames and
    // the engine dereferences the list), and a spawn outside Tick (a lobby button) walks on its own.
    const std::vector<UObject*>& SpawnIgnore() {
        if (spawnIgnoreState_ == SpawnIgnoreState::Cached) return spawnIgnore_;
        spawnIgnore_ = adapter_.BodiesAndGear();
        if (spawnIgnoreState_ == SpawnIgnoreState::InTick) spawnIgnoreState_ = SpawnIgnoreState::Cached;
        return spawnIgnore_;
    }

    // Where every body of the session lies or stands now (corpses too), but `self`'s: a (re)spawn keeps clear of them.
    std::vector<Point3> BodiesExcept(uint8_t self) {
        std::vector<Point3> out;
        auto add = [&](uint8_t e) {
            if (e == self) return;
            if (UObject* w = EntityActor(e)) { FVector v = adapter_.ActorLocation(w); out.push_back({v.x, v.y, v.z}); }
        };
        for (const RosterMember& m : roster_.members()) add(m.entity);
        for (auto& [e, ref] : bots_) add(e);
        return out;
    }

    // Spawner-less maps (the Abyss): points near a ring around the host on the navmesh, the way the Abyss places its own
    // enemies (NavigableRing); without two of those, the ring's points where a fighter can stand (StandableRing).
    std::vector<Point3> RingCandidates(double radius, const std::vector<UObject*>& ignore) {
        UObject* host = adapter_.PlayerWillie(0);
        double hostFloor;
        if (!host || !adapter_.FloorZ(adapter_.ActorLocation(host), ignore, hostFloor)) return {};
        const FVector c = adapter_.ActorLocation(host);
        auto floorAt = [&](const Point3& p) -> std::optional<double> {
            double z;
            if (!adapter_.FloorZ({p.x, p.y, p.z}, ignore, z)) return std::nullopt;
            return z;
        };
        auto navAt = [&](const Point3& aim, double tolerance) -> std::optional<Point3> {
            FVector v;
            if (!adapter_.NavigablePoint({aim.x, aim.y, aim.z}, tolerance, v)) return std::nullopt;
            return Point3{v.x, v.y, v.z};
        };
        const std::vector<Point3> fighters = BodiesExcept(1);
        std::vector<Point3> nav = NavigableRing({c.x, c.y, c.z}, hostFloor, navAt, floorAt, fighters, radius);
        if (nav.size() >= 2) {
            HSM_LOG("host: no spawners in %s; %zu navmesh points near a %.0f cm ring around the host", currentLevel_.c_str(), nav.size(), radius);
            return nav;
        }
        std::vector<Point3> ring = StandableRing({c.x, c.y, c.z}, hostFloor, floorAt, fighters, radius);
        HSM_LOG("host: no spawners in %s; %zu navmesh points, %zu of 8 points on a %.0f cm ring around the host can be stood on",
                currentLevel_.c_str(), nav.size(), ring.size(), radius);
        return ring;
    }

    // Spawn points: the host's own fighter is candidate 0, the level's Willie spawners (or, without any, a ring of
    // standable points) follow; farthest-point sampling gives slot k its own spot, SpreadPositions keeps every spot
    // kMinFighterGapCm from the others, and FreeSpot from the bodies standing there by now (a late join).
    FVector SpawnPoint(uint8_t slot, double& yawDeg) {
        const std::vector<UObject*>& ignore = SpawnIgnore();
        if (!picksValid_) {
            UObject* host = adapter_.PlayerWillie(0);
            spawnCenter_ = host ? adapter_.ActorLocation(host) : FVector{};
            std::vector<Point3> cands{{spawnCenter_.x, spawnCenter_.y, spawnCenter_.z}};
            for (const FVector& v : adapter_.SpawnCandidates())
                if (std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z) > 1.0) cands.push_back({v.x, v.y, v.z});   // (0,0,0) = unread spawner
            HSM_LOG("host: %zu spawn candidates in %s", cands.size() - 1, currentLevel_.c_str());
            if (cands.size() == 1) for (const Point3& p : RingCandidates(kNoSpawnerRingCm, ignore)) cands.push_back(p);
            spawnSpots_ = SpreadPositions(cands, kMaxFighters);
            picksValid_ = true;
        }
        const Point3 c = FreeSpot(spawnSpots_[slot], BodiesExcept(Roster::EntityForSlot(slot)), 0, kMinFighterGapCm);
        FVector at{c.x, c.y, c.z};
        double dx = spawnCenter_.x - at.x, dy = spawnCenter_.y - at.y;
        yawDeg = (dx == 0 && dy == 0) ? 0 : std::atan2(dy, dx) * 57.29577951308232;   // face the host
        return Grounded(at, ignore);
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
                if (match_.phase() != MatchPhase::Idle && !match_.Has(sp.entity)) JoinerGear(slot);   // joins a running match
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
        } else if (OwnerMode() && t == MsgType::SeverNotice) {
            combat::SeverNoticeMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.claim.victim)) { ++streamsDropped_; return; }
            RouteHostNotice(peer.slot, m.claim);
        } else if (OwnerMode() && t == MsgType::StuckBlade) {
            combat::StuckBladeMsg m; Decode(d, n, m);
            RouteHostStuck(peer.slot, m);
        } else if (OwnerMode() && t == MsgType::PaintFx) {
            combat::PaintFxMsg m;
            if (!Decode(d, n, m)) { ++streamsDropped_; return; }
            if (combat::IsStripRequest(m)) {
                // A strip of a fighter the sender does not own goes to that fighter's owner alone, who checks it is fallen.
                const uint8_t owner = OwnerSlotOf(m.entity);
                if (owner == 0xFF || owner == peer.slot) { ++streamsDropped_; return; }
                if (owner == kHostSlot) fx_.Apply(m);
                else if (std::optional<uint32_t> to = PeerOfSlot(owner)) net_->Send(*to, kChanReliable, ev.data, true);
                return;
            }
            if (!MayStream(peer.slot, m.entity)) { ++streamsDropped_; return; }
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
        } else if (OwnerMode() && t == MsgType::DeathReport) {
            DeathReport r; Decode(d, n, r);
            if (!MayStream(peer.slot, r.victim) || !match_.Alive(r.victim) || RespawnQueued(r.victim)) return;
            const uint32_t now = NowMs();
            const bool named = ValidEntity(r.killer) && r.killer != r.victim && match_.Has(r.killer);
            HandleDeath(r.victim, named ? r.killer : kills_.KillerOf(r.victim, now), now);
        } else if (t == MsgType::GiveUp) {
            GiveUpMsg g;
            if (!Decode(d, n, g) || !MayStream(peer.slot, g.entity)) { ++streamsDropped_; return; }
            HandleGiveUp(g.entity, NowMs());
        } else if (t == MsgType::Input) {
            if (OwnerMode()) return;                    // every owner plays its own fighter
            InputFrame f; Decode(d, n, f);
            Fighter& fr = fighters_[peer.slot];
            fr.input.Receive(f); ++fr.inputsRx; fr.lastInputAt = NowMs();
        } else if (t == MsgType::Ready) {
            Ready r; Decode(d, n, r);
            director_.OnReady(peer.slot, r.travelSeq, NowMs());
            for (const Spawn& sp : director_.LiveSpawns()) net_->Send(ev.peer, kChanReliable, Encode(sp), true);
            // The level props' census, then every current lease (a client refuses a level prop's Grant its census lacks),
            // after the level load that cleared the joiner's view (props_.Clear).
            if (OwnerMode()) {
                for (const std::vector<uint8_t>& c : props_.CensusForJoiner()) net_->Send(ev.peer, kChanReliable, c, true);
                for (const std::vector<uint8_t>& g : props_.GrantsForJoiner()) net_->Send(ev.peer, kChanReliable, g, true);
            }
            HSM_LOG("host: slot %u ready in %s (travel #%u)", peer.slot, r.level.c_str(), r.travelSeq);
        } else if (t == MsgType::Chat) {
            ChatLine c; Decode(d, n, c);
            if (!peer.chat.Allow(NowMs())) return;
            c.fromSlot = peer.slot;
            net_->Broadcast(kChanReliable, Encode(c), true);
            ui::ShowLine(NameOfSlot(peer.slot), ChatNameInk(info_, Roster::EntityForSlot(peer.slot)), c.text);
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
        match_.RemoveFighter(Roster::EntityForSlot(slot)); infoDirty_ = true;
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
        // Cheap first (test 5: bodies against the ground were most of the 8970 calls of one Abyss frame): a contact
        // with no weapon on either side has no attacker, so it is never blocked, credited or bled for; what a class
        // is gets looked up once per class, whom an actor belongs to once per frame.
        const HitKind selfKind = HitKindOf(obj), otherKind = HitKindOf(other);
        const bool selfWeapon = selfKind == HitKind::Weapon, otherWeapon = otherKind == HitKind::Weapon;
        if (selfWeapon && otherWeapon) { if (owner) NoteWeaponClash(obj, other); return false; }
        if (!selfWeapon && !otherWeapon) return false;
        UObject* weapon = selfWeapon ? obj : other;
        UObject* victim = selfWeapon ? (otherKind == HitKind::Willie ? other : FighterOfThisFrame(other)) : obj;
        if (!victim || HitKindOf(victim) != HitKind::Willie) return false;
        UObject* attacker = FighterOfThisFrame(weapon);
        uint8_t ve = owner ? OwnedEntityOf(victim) : LocalEntityOf(victim);
        uint8_t ae = !attacker ? 0 : owner ? OwnedEntityOf(attacker) : LocalEntityOf(attacker);
        if (!ve) return false;                        // not a session fighter (e.g. a stock foe being removed), or a stand-in
        bool ff = IsHost() ? match_.config().friendlyFire : info_.friendlyFire;
        if (!HitAllowed(true, ae ? KnownTeam(ae) : kNoTeam, KnownTeam(ve), ff)) { ++hitsBlocked_; return true; }
        if (ae && ae != ve) {
            if (IsHost()) kills_.OnHit(ve, ae, NowMs());
            if (!owner) SendBlood(ve, victim, fn, params);
        }
        return false;
    }

    void NoteWeaponClash(UObject* a, UObject* b) {
        UObject* fa = FighterOfThisFrame(a);
        UObject* fb = FighterOfThisFrame(b);
        const uint8_t sa = standIns_.EntityOf(fa), sb = standIns_.EntityOf(fb);
        if (sa && !sb && OwnedEntityOf(fb)) claims_.NoteClash(sa, a, game::SteadyNowMs());
        else if (sb && !sa && OwnedEntityOf(fa)) claims_.NoteClash(sb, b, game::SteadyNowMs());
    }

    // What a contact's actor is, per class: hit events fire thousands of times a frame. Not remembered while the
    // fighter class is not known yet (a Willie would read as other); dropped on a level change and when owner sync
    // (which decides what counts as a weapon) is switched.
    enum class HitKind : uint8_t { Other, Willie, Weapon };
    HitKind HitKindOf(UObject* o) {
        if (!o) return HitKind::Other;
        const bool owner = OwnerMode();
        if (owner != hitKindOwner_) { hitKind_.clear(); hitKindOwner_ = owner; }
        UClass* c = ClassOf(o);
        auto it = hitKind_.find(c);
        if (it != hitKind_.end()) return it->second;
        const HitKind k = adapter_.IsWillie(o) ? HitKind::Willie : adapter_.IsWeapon(o) ? HitKind::Weapon : HitKind::Other;
        if (k != HitKind::Other || adapter_.WillieClassKnown()) hitKind_.emplace(c, k);
        return k;
    }
    // Adapter::FighterOf (attach parents, a weapon's holder) once per actor per frame; Tick starts each frame empty.
    UObject* FighterOfThisFrame(UObject* actor) {
        if (!actor) return nullptr;
        auto it = fighterOfFrame_.find(actor);
        if (it != fighterOfFrame_.end()) return it->second;
        UObject* f = adapter_.FighterOf(actor);
        fighterOfFrame_.emplace(actor, f);
        return f;
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

    // The match StartMatch would start now: the lobby's settings, or Progression co-op outside Free Mode.
    MatchConfig StartConfig() {
        MatchConfig cfg = steamMode() ? lobby_.config() : MatchConfig{};
        if (!adapter_.FreeModeActive()) { cfg.format = Format::PlayersVsBots; cfg.scoreLimit = 100; cfg.timeLimitS = 0; }   // co-op against the event's opponents, no clock
        if (cfg.format == Format::Duel) cfg.friendlyFire = true;   // two fighters, no teams: every hit counts
        return cfg;
    }

    void StartMatch() {
        MatchConfig cfg = StartConfig();
        bool progression = !adapter_.FreeModeActive();
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
        match_.Start(cfg, list, NowMs());
        kills_ = KillLog{}; wave_ = 0; adoptStock_ = IsBotFormat(cfg.format); wavesEnabled_ = adoptStock_ && !progression; looted_.clear();
        respawnQueue_.clear();
        Regear startGear = RegearFor(match_.config(), RespawnCause::Rematch);   // everybody starts together; None for Own and bot formats
        bool gearApplied = startGear != Regear::None;
        progression_ = progression; oneFight_ = progression;
        sidesValid_ = false;
        HSM_LOG("host: match started (format %d, limit %u, time %us, friendly fire %s)%s", int(cfg.format), cfg.scoreLimit, cfg.timeLimitS,
                cfg.friendlyFire ? "on" : "off", progression ? " - Progression co-op" : "");
        HSM_LOG("host: match gear %d, tier %u", int(cfg.gear), cfg.tier);
        RegearSlots(match_.config(), AllSlots(), startGear, "match start");
        BroadcastInfo(NowMs());                          // the clients know the match's gear before their Spawns arrive
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
            if (!(wanted && hostWillie_ && allIn)) { if (wanted && OwnerMode()) MakeStartGearAhead(now); return; }
            StartMatch();
        }
        if (now >= nextScan_) { nextScan_ = now + kScanIntervalMs; ScanStockWillies(); }
        DetectDeaths(now);
        Match::Events ev = match_.Tick(now);
        if (ev.live || ev.ended || ev.rematch || ev.waveStarted || ev.waveCleared || ev.roundReset) infoDirty_ = true;
        if (ev.waveStarted) HSM_LOG("host: wave %d starts (%d bots)", match_.wave() + 1, match_.botsLeft());
        if (ev.waveCleared) HSM_LOG("host: wave cleared");
        if (ev.roundReset) HSM_LOG("host: duel round reset");
        if (dropWatch_.Observe(match_.config().format, uint8_t(match_.phase()), uint16_t(match_.round()))) ClearDroppedWeapons();
        if (ev.ended) HSM_LOG("host: match over, winner key %d", match_.winner());
        MakeGearAhead(now);
        if (ev.rematch && progression_) {                // Progression fights end back in the Tavern, like the stock game
            HSM_LOG("host: Progression fight over; returning everyone to the hub");
            HostTravel(kHubLevel);
            return;
        }
        if (ev.regear != Regear::None) {                 // the blobs go out on the reliable channel before the Spawns
            std::vector<uint8_t> slots;
            if (ev.rematch) slots = AllSlots();
            else for (uint8_t e : ev.respawn) if (e >= 1 && e <= kMaxFighters && roster_.BySlot(uint8_t(e - 1))) slots.push_back(uint8_t(e - 1));
            RegearSlots(match_.config(), slots, ev.regear, ev.rematch ? "rematch" : "respawn");
            infoDirty_ = true;
        }
        for (uint8_t e : ev.respawn)
            if (!OwnerMode()) RespawnEntity(e);
            else if (!RespawnQueued(e)) respawnQueue_.push_back(e);
        if (ev.rematch) RespawnAll();
        if (!respawnQueue_.empty()) { const uint8_t e = respawnQueue_.front(); respawnQueue_.pop_front(); RespawnEntity(e); }
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
        if (wavesEnabled_ && match_.config().format == Format::PlayersVsBots)   // zombies disintegrate (the stock death)
            for (uint8_t e : match_.StaleBotCorpses(now, Match::kLootDelayMs)) {
                auto it = bots_.find(e);
                if (it == bots_.end() || !looted_.insert(e).second || !adapter_.StillValid(it->second.obj, it->second.index)) continue;
                int weapons = 0;
                const int pieces = adapter_.DropLoot(it->second.obj, uint32_t(e) * 2654435761u ^ now, weapons);
                HSM_LOG("host: bot %u dropped its loot: %d weapon(s), %d armour piece(s)", e, weapons, pieces);
            }
        if (infoDirty_ || now >= nextInfo_) BroadcastInfo(now);
    }

    void DetectDeaths(uint32_t now) {
        std::vector<uint8_t> entities;
        for (const RosterMember& m : roster_.members()) entities.push_back(m.entity);
        for (auto& [e, ref] : bots_) entities.push_back(e);
        for (uint8_t e : entities) {
            if (!match_.Alive(e) || RespawnQueued(e)) continue;   // its old body is still dead until its respawn
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
        {
            match_.OnDeath(e, killer, now);
            Kill k; k.killer = killer; k.victim = e;
            if (killer == 0) k.flags = 4;
            else if (killer == e) k.flags = 2;
            else if (match_.TeamOf(killer) != kNoTeam && match_.TeamOf(killer) == match_.TeamOf(e)) k.flags = 1;
            HSM_LOG("host: entity %u died (killer %u, flags %u, round %d)", e, killer, unsigned(k.flags), match_.round());
            net_->Broadcast(kChanReliable, Encode(k), true);
            PushFeed(k);
            infoDirty_ = true;
        }
    }

    // Held G: the fighter concedes the Duel round being fought (Match::Forfeit); the feed says they gave up. In the bot
    // formats' break between waves it is the player's vote to skip the break (Match::VoteSkip).
    void HandleGiveUp(uint8_t e, uint32_t now) {
        if (IsBotFormat(match_.config().format)) {
            if (!match_.VoteSkip(e, now)) { HSM_LOG("match: #%u held G, but there is no break to skip (or it already voted)", e); return; }
            HSM_LOG("match: #%u votes to skip the break (%zu/%zu)", e, match_.SkipVotes(), match_.SkipVotesNeeded());
            infoDirty_ = true;
            return;
        }
        if (RespawnQueued(e)) return;                     // the new round has not reached its fighter yet
        if (!match_.Forfeit(e, now)) { HSM_LOG("match: #%u held G, but there is no Duel round to give up", e); return; }
        HSM_LOG("match: #%u gave up the round", e);
        Kill k; k.victim = e; k.flags = 8;
        net_->Broadcast(kChanReliable, Encode(k), true);
        PushFeed(k);
        infoDirty_ = true;
    }

    // Duel's new round, and free-for-all and teams' rematch, start without the weapons and armour pieces dropped before
    // (weapons piled up 5 -> 36 in test 13). Every machine clears its own copies; the prop sync lets go of the ones it
    // was moving. Armour the level had at the match start stays (LooseTimer's baseline).
    void ClearDroppedWeapons() {
        if (!OwnerMode() || !MatchLevel()) return;
        const int weapons = adapter_.DestroyDroppedWeapons();
        int armor = 0;
        if (looseOn_) {
            std::unordered_map<uint64_t, UObject*> byId;
            const std::vector<uint64_t> ids = LooseIds(byId);
            const double now = double(NowMs());
            looseTimer_.Sweep(ids, now);                                   // a piece dropped since the last sweep is timed too
            for (uint64_t id : looseTimer_.Sweep(ids, now + kIdleDespawnMs)) { adapter_.DestroyActor(byId[id]); ++armor; }   // all but the baseline
        }
        HSM_LOG("drops: %s, %d dropped weapon(s) and %d armour piece(s) cleared",
                Format(info_.format) == Format::Duel ? "new round" : "rematch", weapons, armor);
    }

    // The loose armour pieces here, keyed by object (LooseTimer ids).
    std::vector<uint64_t> LooseIds(std::unordered_map<uint64_t, UObject*>& byId) {
        std::vector<uint64_t> ids;
        for (UObject* a : adapter_.LooseArmor()) { const uint64_t id = uint64_t(reinterpret_cast<uintptr_t>(a)); ids.push_back(id); byId[id] = a; }
        return ids;
    }

    // Every machine, every frame: what clears the drops in this format (DropClearFor). The bot formats: weapons a
    // minute after they went FREE (the prop sync), loose armour a minute after it was first seen loose here. The timer's
    // baseline is taken when a match starts in this level, so armour the level had stays.
    void DropsTick() {
        const bool running = OwnerMode() && MatchLevel() && info_.phase != uint8_t(MatchPhase::Idle);
        const bool idle = running && DropClearFor(Format(info_.format)) == DropClear::AfterIdle;
        props_.SetIdleDespawn(idle);
        if (!running) { if (looseOn_) { looseTimer_.Reset(); looseOn_ = false; } return; }
        const uint32_t now = NowMs();
        if (looseOn_ && now < nextLooseSweep_) return;
        looseOn_ = true; nextLooseSweep_ = now + kLooseSweepMs;
        std::unordered_map<uint64_t, UObject*> byId;
        const std::vector<uint64_t> due = looseTimer_.Sweep(LooseIds(byId), double(now));
        if (!idle) return;                                                 // timed only, for the next clear
        for (uint64_t id : due) {
            if (++loosePurged_ <= 20)
                HSM_LOG("drops: loose armour %s despawned: a minute loose", ObjectName(byId[id]).c_str());
            adapter_.DestroyActor(byId[id]);
        }
    }

    // Hold G in a fought Duel round to give it up (GiveUpHold), or in a bot format's break to vote to skip it: the host's
    // own here, a client's by asking the host.
    void GiveUpTick() {
        const uint32_t now = NowMs();
        bool down = false;
        const bool fought = info_.phase == uint8_t(MatchPhase::Live) || info_.phase == uint8_t(MatchPhase::SuddenDeath);
        UObject* own = adapter_.PlayerWillie(0);
        const bool duel = info_.format == uint8_t(Format::Duel) && fought && !InRoundGap(info_) && own && !adapter_.IsDead(own) &&
                          EntityAlive(MyEntity());
        const bool skip = InWaveBreak(info_);
        if (MatchLevel() && (duel || skip) && !ui::Typing()) {
            if (GetAsyncKeyState('G') & 0x8000) {   // the key first: asking whose window is in front is the dearer call
                DWORD pid = 0;
                if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
                down = pid == GetCurrentProcessId();
            }
        }
        if (!giveUp_.Update(down, now)) return;
        const uint8_t me = MyEntity();
        HSM_LOG("match: G held %u ms: %s (#%u)", unsigned(GiveUpHold::kHoldMs), skip ? "voting to skip the break" : "giving up the round", me);
        if (IsHost()) HandleGiveUp(me, now);
        else if (hostPeer_) { GiveUpMsg g; g.entity = me; net_->Send(hostPeer_, kChanReliable, Encode(g), true); }
    }

    // Team side point for a (re)spawn, farthest from living enemies; each entity has its own spread slot around it
    // (SpreadSlot: humans on ring 1, bots further out), moved on to the next free one when a body is there.
    FVector SidePoint(int8_t team, uint8_t entity, double& yawDeg) {
        const std::vector<UObject*>& ignore = SpawnIgnore();
        if (!sidesValid_) {
            sidePoints_.clear();
            for (const FVector& v : adapter_.SpawnCandidates()) sidePoints_.push_back({v.x, v.y, v.z});
            if (sidePoints_.size() < 2) sidePoints_ = RingCandidates(kSideRingCm, ignore);   // no spawners: a ring around the host
            if (sidePoints_.size() < 2) {                  // nothing standable found: the ring unchecked
                FVector c = hostWillie_ ? adapter_.ActorLocation(hostWillie_) : FVector{};
                sidePoints_.clear();
                for (int i = 0; i < 8; ++i) sidePoints_.push_back({c.x + std::cos(i * 0.785398) * kSideRingCm, c.y + std::sin(i * 0.785398) * kSideRingCm, c.z});
            }
            sides_ = PartitionSides(sidePoints_, std::max(TeamCount(match_.config().format), 1));
            sidesValid_ = true;
        }
        std::vector<Point3> enemies;
        for (const RosterMember& m : roster_.members()) AddEnemy(enemies, m.entity, team, entity);
        for (auto& [e, ref] : bots_) AddEnemy(enemies, e, team, entity);
        const Point3& p = sidePoints_[PickRespawn(sidePoints_, sides_, team, enemies)];
        const Point3 spot = FreeSpot(p, BodiesExcept(entity), entity, kMinFighterGapCm);
        FVector at = Grounded({spot.x, spot.y, spot.z}, ignore);
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
        Spawn sp; sp.entity = e; sp.ownerSlot = slot; sp.kind = EntityKind::Fighter; sp.team = team; sp.blobId = o.blob ? id : 0;
        sp.x = float(at.x); sp.y = float(at.y); sp.z = float(at.z); sp.yaw = float(yaw);
        director_.MarkSpawned(sp);
        Despawn dp; dp.entity = e;
        net_->Broadcast(kChanReliable, Encode(dp), true);
        net_->Broadcast(kChanReliable, Encode(sp), true);
        match_.MarkRespawned(e); kills_.Forget(e);
        if (w) QueueTint(w, team); else HSM_LOG("host: respawn of entity %u failed", e);
    }

    // Owner sync: respawns wait here and go one per tick (about 60-120 ms each). A Duel round reset did them all in one
    // tick (two-player log 9: 410-470 ms). The match counts a queued fighter alive, but its old body is still the dead
    // one, so deaths, DeathReports and give-ups are ignored for it until its turn.
    bool RespawnQueued(uint8_t e) const { return std::find(respawnQueue_.begin(), respawnQueue_.end(), e) != respawnQueue_.end(); }

    void RespawnAll() {
        respawnQueue_.clear();
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
            // The Abyss level replaces every enemy it loses, so removing them loops forever: there they are adopted
            // (synced) while bot entities last and otherwise left alone.
            bool abyss = currentLevel_.find("Abyss") != std::string::npos;
            uint8_t e = (adoptStock_ || abyss) && !adapter_.IsDead(w) ? FreeBotEntity() : 0;
            if (e) { AnnounceBot(e, w); HSM_LOG("host: stock fighter adopted as bot entity %u", e); }
            else if (!abyss) { adapter_.DestroyActor(w); ++purged; }
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
        adapter_.DestroyFighter(it->second.obj, it->second.index);   // its armour and weapons too (DestroyActor left them)
        looted_.erase(e);
        Despawn dp; dp.entity = e; net_->Broadcast(kChanReliable, Encode(dp), true);
        director_.Forget(e); match_.RemoveFighter(e); kills_.Forget(e); severedSent_.erase(e); severs_.Forget(e); gore_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e);
        bots_.erase(it);
        infoDirty_ = true;
    }

    std::vector<uint8_t> AllSlots() const {
        std::vector<uint8_t> s;
        for (const RosterMember& m : roster_.members()) s.push_back(m.slot);
        return s;
    }

    // Generated gear (RegearFor): Each rolls a fresh loadout per slot, Shared one loadout for all of them. StoreAndRelay
    // makes it the slot's blob for its next Spawn; the chunks precede that Spawn on the reliable channel.
    void RegearSlots(const MatchConfig& cfg, const std::vector<uint8_t>& slots, Regear mode, const char* why) {
        if (mode == Regear::None || slots.empty()) return;
        const double t0 = game::SteadyNowMs();
        int ahead = 0;
        auto next = [&] {                                // one made ahead in the gap if there is one, else made now
            std::vector<uint8_t> b = gearPool_.Take(cfg.tier);
            if (!b.empty()) { ++ahead; return b; }
            return game::RandomPassportBlob(adapter_, cfg.tier);
        };
        std::vector<uint8_t> shared;
        if (mode == Regear::Shared) shared = next();
        int generated = mode == Regear::Shared && !shared.empty() ? 1 : 0, stored = 0;
        for (uint8_t slot : slots) {
            std::vector<uint8_t> blob = mode == Regear::Shared ? shared : next();
            if (blob.empty()) { HSM_LOG("host: no generated gear for slot %u; it keeps its loadout", slot); continue; }
            if (mode == Regear::Each) {
                ++generated;
                HSM_LOG("host: entity %u respawns with a new tier-%u loadout", unsigned(Roster::EntityForSlot(slot)), cfg.tier);
            }
            StoreAndRelay(slot, std::move(blob), 0); ++stored;
            if (slot == 0) NewGearNotice(cfg.tier);
        }
        if (mode == Regear::Shared && stored) {
            mirrorBlob_ = shared;
            if (match_.round()) HSM_LOG("host: round %d: new loadout for everyone (tier %u)", match_.round(), cfg.tier);
            else HSM_LOG("host: %s: new loadout for everyone (tier %u)", why, cfg.tier);
        }
        HSM_LOG("host: %d loadout(s) generated in %.1f ms (%s; %d made ahead)", generated, game::SteadyNowMs() - t0, why, ahead);
    }

    // Owner sync, before the match starts: its generated start loadouts, made one at a time while the players' fighters
    // come in. Made at the start they stalled that frame along with everyone's respawn (two-player log 9: 208 ms of a
    // 456 ms frame); RegearSlots takes them from the pool. Any not made by then are made there, as before.
    void MakeStartGearAhead(uint32_t now) {
        if (now < nextGearAhead_) return;
        const MatchConfig cfg = StartConfig();
        const size_t want = StartLoadoutsAhead(cfg, roster_.size());
        if (!want || !gearPool_.Wants(cfg.tier, want)) return;
        std::vector<uint8_t> blob = game::RandomPassportBlob(adapter_, cfg.tier);
        nextGearAhead_ = now + (blob.empty() ? 1000 : 250);
        if (blob.empty()) return;
        gearPool_.Put(cfg.tier, std::move(blob));
        HSM_LOG("host: start loadout %zu of %zu made ahead (tier %u)", gearPool_.size(), want, unsigned(cfg.tier));
    }

    // The coming respawns' generated loadouts, made one at a time while nobody fights: each stalls the frame about
    // 110 ms, and made together at the round reset they stalled it 230-260 ms just as the round began (test 13).
    // The first waits a second into the gap (not on the deciding blow), the next a quarter second after it.
    void MakeGearAhead(uint32_t now) {
        const uint8_t tier = match_.config().tier;
        const size_t want = match_.LoadoutsAhead();
        if (!want) { nextGearAhead_ = now + 1000; return; }
        if (now < nextGearAhead_ || !gearPool_.Wants(tier, want)) return;
        std::vector<uint8_t> blob = game::RandomPassportBlob(adapter_, tier);
        nextGearAhead_ = now + (blob.empty() ? 1000 : 250);
        if (blob.empty()) return;
        gearPool_.Put(tier, std::move(blob));
        HSM_LOG("host: loadout %zu of %zu made ahead (tier %u)", gearPool_.size(), want, unsigned(tier));
    }

    // A fighter joining a running match takes its generated gear: the current shared loadout, or a fresh one.
    void JoinerGear(uint8_t slot) {
        const MatchConfig& c = match_.config();
        if (RegearFor(c, RespawnCause::Rematch) == Regear::Shared && !mirrorBlob_.empty()) {
            StoreAndRelay(slot, mirrorBlob_, 0);
            HSM_LOG("host: entity %u joins with the match's shared loadout", unsigned(Roster::EntityForSlot(slot)));
        } else RegearSlots(c, {slot}, RegearFor(c, RespawnCause::Individual), "join");
    }

    uint16_t PingOfSlot(uint8_t slot) {
        if (!net_ || slot == 0) return 0;
        for (const auto& [peer, p] : peers_) if (p.slot == slot) return uint16_t(std::min<uint32_t>(net_->RoundTripMs(peer), 65535u));
        return 0;
    }

    void BroadcastInfo(uint32_t now) {
        infoDirty_ = false; nextInfo_ = now + kInfoIntervalMs;
        MatchInfo m;
        const MatchConfig& c = match_.config();
        m.format = uint8_t(c.format); m.scoreLimit = c.scoreLimit; m.timeLimitS = c.timeLimitS; m.respawnDelayS = c.respawnDelayS;
        m.friendlyFire = c.friendlyFire; m.phase = uint8_t(match_.phase()); m.timeLeftMs = match_.TimeLeftMs(now);
        m.winner = int16_t(match_.winner());
        m.gear = uint8_t(IsBotFormat(c.format) ? Gear::Own : c.gear); m.tier = c.tier;   // the gear in effect
        m.round = uint16_t(match_.round()); m.roundGapLeftMs = match_.RoundGapLeftMs(now); m.roundWinner = match_.roundWinner();
        if (IsBotFormat(c.format)) {                     // the break between waves in the Duel gap's fields (InWaveBreak)
            m.roundGapLeftMs = match_.BreakLeftMs(now);
            m.roundWinner = uint8_t(std::min<size_t>(match_.SkipVotes(), 255));
        }
        for (const auto& [key, score] : match_.scores()) if (m.scores.size() < kMaxWillies) m.scores.push_back({int16_t(key), int16_t(score)});
        for (const RosterMember& r : roster_.members())
            m.fighters.push_back({r.entity, match_.TeamOf(r.entity), int16_t(match_.Kills(r.entity)), int16_t(match_.Deaths(r.entity)), r.name,
                                  PingOfSlot(r.slot), match_.Alive(r.entity)});
        for (auto& [e, ref] : bots_)
            if (m.fighters.size() < kMaxWillies)
                m.fighters.push_back({e, kBotTeam, int16_t(match_.Kills(e)), int16_t(match_.Deaths(e)), "Bot", 0, match_.Alive(e)});
        net_->Broadcast(kChanReliable, Encode(m), true);
        info_ = std::move(m); infoAt_ = now;
        banner_.Observe(info_, MyEntity(), now);
    }

    // ------------------------------------------------------------------ match (both)
    uint8_t MyEntity() const { return IsHost() ? uint8_t(1) : world_.myEntity(); }
    void PushFeed(const Kill& k) {
        feed_.push_back({NowMs(), MakeFeedRow(info_, k, MyEntity())});
        while (feed_.size() > kFeedRows) feed_.pop_front();
    }
    // This machine's fighter comes back in generated gear.
    void NewGearNotice(uint8_t tier) { banner_.Fire("NEW GEAR", "TIER " + std::to_string(tier), style::kWarn, NowMs()); }

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

    HudScene BuildHud() {
        if (!MatchLevel() || info_.phase == uint8_t(MatchPhase::Idle)) return {};
        uint32_t now = NowMs();
        UObject* own = adapter_.PlayerWillie(0);
        bool dead = own && adapter_.IsDead(own);
        if (dead && !ownDiedAt_) ownDiedAt_ = now;
        if (!dead && !ownDeadPending_) ownDiedAt_ = 0;
        LocalHud l;
        l.me = MyEntity();
        l.infoAtMs = infoAt_;            // MatchInfo comes once a second; the scene runs its clocks on between
        l.dead = ownDiedAt_ != 0; l.diedAtMs = ownDiedAt_;
        l.tab = ui::HudTabHeld();
        l.oneFight = oneFight_;
        if (const MapInfo* map = FindMap(currentLevel_)) l.map = map->display;
        l.banner = banner_.shot();
        l.feed.assign(feed_.begin(), feed_.end());
        l.giveUp = giveUp_.progress();
        return BuildHudScene(info_, l, now);
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
        bool right = GetAsyncKeyState(VK_RIGHT) & 0x8000, left = GetAsyncKeyState(VK_LEFT) & 0x8000;
        if (right || left) {   // the keys first: asking whose window is in front costs more
            DWORD pid = 0;
            if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
            if (pid != GetCurrentProcessId()) right = left = false;
        }
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
        ownDeadPending_ = false;
        placed_.erase(world_.myEntity());
        const std::vector<uint8_t>* assigned = sp.blobId ? clientBlobs_.Get(sp.blobId) : nullptr;   // e.g. generated gear
        bool generated = info_.gear != uint8_t(Gear::Own);   // own gear: the Spawn names this client's upload, never relayed back
        if (generated && sp.blobId && !assigned)
            HSM_LOG("client: own Spawn names loadout %08x, which has not arrived; respawning in the uploaded loadout", sp.blobId);
        if (generated && assigned && ownBlob_ && sp.blobId != ownBlob_) NewGearNotice(info_.tier);
        ownBlob_ = sp.blobId;
        game::Adapter::SpawnOptions o; o.blob = assigned ? assigned : lastUpload_.empty() ? nullptr : &lastUpload_;
        o.at = {sp.x, sp.y, sp.z}; o.yawDeg = sp.yaw; o.teamInt = 1;
        severs_.Forget(world_.myEntity());          // a new body: its stuck blades and holds start afresh
        claims_.Forget(world_.myEntity()); fx_.Forget(world_.myEntity()); equip_.Forget(world_.myEntity());
        UObject* w = adapter_.RespawnLocalPlayer(o);
        bodyWork_ = true;
        snappedOwn_ = false;                         // input hooks are per function and follow myWillie_
        if (w) QueueTint(w, sp.team);
    }

    // Everything that is not this player's fighter or a puppet of the host's world is local-only: remove it.
    void PurgeForeignClient() {
        UObject* me = adapter_.PlayerWillie(0);
        int purged = 0;
        for (UObject* w : adapter_.AllWillies()) {
            if (w == me) continue;
            bool puppet = false;
            for (auto& [e, ref] : puppets_) puppet = puppet || ref.obj == w;
            if (!puppet) { adapter_.DestroyActor(w); ++purged; }
        }
        if (purged) HSM_LOG("client: removed %d local-only fighter(s)", purged);
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
        bodyWork_ = false;
        Timed(kPumpNet, [&] {
            NetEvent ev;
            while (!bodyWork_ && net_->Poll(ev)) {   // the rest of the queue waits for the next frame (bodyWork_)
                if (ev.kind == NetEvent::Connected) {
                    hostPeer_ = ev.peer;
                    SendHello();
                } else if (ev.kind == NetEvent::Disconnected) {
                    HSM_LOG("client: disconnected from host");
                    if (welcomed_ && InFightLevel()) hostGone = hostLoss_.OnDisconnect(NowMs()) == HostLossAction::ReturnToMenu;
                    welcomed_ = false; hostPeer_ = 0;
                } else { hostLoss_.OnPacket(NowMs()); ClientReceive(ev); }
            }
        });
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
        if (OwnerMode()) RecoverLostPuppets();
        CreatePuppets();
        if (MatchLevel() && info_.phase != uint8_t(MatchPhase::Idle) && NowMs() >= nextScan_) { nextScan_ = NowMs() + kScanIntervalMs; PurgeForeignClient(); }
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
        if (p.manager && !managerProbed_) {
            // Once per level: the delay the manager waits before it spawns the player (a Blueprint default the RE
            // tools cannot decode offline; test 5's line decides LevelSpawnGate::kMaxWaitMs).
            managerProbed_ = true;
            const double* delay = Field<double>(managers.front(), "Level Load Delay");
            if (delay) HSM_LOG("client: level manager found; Level Load Delay %.2f s, Player Spawned %d, pawn %d", *delay, p.playerSpawned, p.pawn);
            else HSM_LOG("client: level manager found; Level Load Delay unreadable, Player Spawned %d, pawn %d", p.playerSpawned, p.pawn);
            // Level props: 0.1 s into the level its manager destroys each prop tagged 'Random Delete' on a RandomBool of
            // this machine's own when "Purge Map" is set; the host's census decides which stay (PropSync purges the
            // rest), so the manager's own purge is turned off here. Best effort: the world time says whether in time.
            bool purge = false;
            const bool purgeRead = GetBool(managers.front(), "Purge Map", purge);
            const bool purgeOff = purgeRead && purge && SetBool(managers.front(), "Purge Map", false);
            HSM_LOG("client: level manager's Purge Map %s at world time %.2f s (its purge runs at 0.10 s)",
                    !purgeRead ? "unreadable" : !purge ? "already off" : purgeOff ? "turned off" : "on (could not turn it off)",
                    adapter_.WorldSeconds());
        }
        if (!levelGate_.Update(now, p)) return false;
        const char* why = levelGate_.why() == LevelSpawnGate::Why::LevelSpawned ? "the level spawned its player"
                        : levelGate_.why() == LevelSpawnGate::Why::NoManager ? "no level manager" : "timed out waiting for the level";
        const LevelProbe& last = levelGate_.lastProbe();
        if (levelGate_.why() == LevelSpawnGate::Why::TimedOut)
            HSM_LOG("client: level start-up over (%s, still waiting for %s; last probe: manager %d, Player Spawned %d, pawn %d); spawning fighters",
                    why, GateWaitingName(levelGate_.waitingFor()), last.manager, last.playerSpawned, last.pawn);
        else HSM_LOG("client: level start-up over (%s); spawning fighters", why);
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
        Timed(kPumpStreams, [&] {
            streams_.Tick(adapter_, now, dt, owned, [this](const std::vector<uint8_t>& b) { if (hostPeer_) net_->Send(hostPeer_, kChanUnreliable, b, false); });
            ReportOwnDeath(me, now);
        });
        Timed(kPumpServo, [&] { standIns_.Tick(adapter_, now, dt, OwnedPelvises(owned)); });
        Timed(kPumpClaims, [&] { claims_.Tick(now); });
        Timed(kPumpSevers, [&] { severs_.Tick(now, owned); });
        Timed(kPumpGore, [&] { gore_.Tick(now); });
        Timed(kPumpGrabs, [&] { grabs_.Tick(now, dt, owned); });
        Timed(kPumpProps, [&] { props_.Tick(now, owned); });
        Timed(kPumpEquip, [&] { equip_.Tick(now, owned); });
    }

    // Once per life: my fighter died here, natively; the host referees the match and credits the last blow I took.
    void ReportOwnDeath(UObject* me, double now) {
        if (!me || !adapter_.IsDead(me)) { if (me) ownDeathReported_ = false; return; }   // alive: a new life
        if (ownDeathReported_ || !hostPeer_) return;
        ownDeathReported_ = true;
        DeathReport r; r.victim = world_.myEntity(); r.killer = claims_.RecentAttacker(r.victim, now);
        net_->Send(hostPeer_, kChanReliable, Encode(r), true);
        HSM_LOG("client: my fighter died; reported to the host (last blow from entity %u)", r.killer);
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
        if (bodyWork_) return;
        auto have = [this](uint32_t id) { return clientBlobs_.AtLeast(id) != nullptr; };
        for (const Spawn& sp : world_.PendingPuppets(have)) {
            game::Adapter::SpawnOptions o;
            o.blob = sp.blobId ? clientBlobs_.AtLeast(sp.blobId) : nullptr;
            o.at = {sp.x, sp.y, sp.z}; o.yawDeg = sp.yaw; o.teamInt = TeamIntFor(sp.team, sp.entity); o.brain = false;   // follows its owner's pose
            UObject* w = adapter_.SpawnFighter(o);
            if (w) { puppets_[sp.entity].Set(w); QueueTint(w, sp.team); if (OwnerMode()) standIns_.Attach(sp.entity, w); }
            else HSM_LOG("client: puppet for entity %u failed to spawn", sp.entity);
            world_.MarkCreated(sp.entity);           // no retry loop on failure; the next level tries again
            bodyWork_ = true;
            break;                                   // one per frame
        }
    }

    void DestroyPuppet(uint8_t entity) {
        auto it = puppets_.find(entity);
        if (it == puppets_.end()) return;
        bodyWork_ = true;
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
            case MsgType::MatchInfo: {
                MatchInfo m; Decode(d, n, m); info_ = std::move(m); infoAt_ = NowMs();
                banner_.Observe(info_, world_.myEntity(), infoAt_);
                if (dropWatch_.Observe(Format(info_.format), info_.phase, info_.round)) ClearDroppedWeapons();
                break;
            }
            case MsgType::Kill: { Kill k; Decode(d, n, k); PushFeed(k); break; }
            case MsgType::Chat: {
                ChatLine c; Decode(d, n, c);
                ui::ShowLine(NameOfSlot(c.fromSlot), ChatNameInk(info_, Roster::EntityForSlot(c.fromSlot)), c.text);
                break;
            }
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
                    standIns_.OnVitals(v);
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
            case MsgType::SeverNotice: {
                combat::SeverNoticeMsg m;
                if (OwnerMode() && Decode(d, n, m) && m.claim.victim != world_.myEntity()) severs_.Remember(m.claim, game::SteadyNowMs());
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
                if (OwnerMode() && Decode(d, n, m) && (m.entity != world_.myEntity()) != combat::IsStripRequest(m)) fx_.Apply(m);
                break;
            }
            case MsgType::Equip: {
                combat::EquipStateMsg m;
                if (OwnerMode() && Decode(d, n, m) && m.entity != world_.myEntity()) equip_.Apply(m);
                break;
            }
            case MsgType::PropState: { if (OwnerMode()) props_.OnPropState(d, n, kHostSlot); break; }
            case MsgType::PropLease: { if (OwnerMode()) props_.OnPropLease(d, n, kHostSlot); break; }
            case MsgType::PropCensus: { if (OwnerMode()) props_.OnPropCensus(d, n); break; }
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
    std::vector<uint8_t> mirrorBlob_;            // the match's shared generated loadout (Everyone identical), for joiners
    std::map<uint32_t, Peer> peers_;
    std::array<Fighter, kMaxFighters> fighters_;
    std::optional<Travel> lastTravel_;
    bool picksValid_ = false;
    FVector spawnCenter_;
    std::vector<Point3> spawnSpots_;           // SpreadPositions per slot (picksValid_)
    uint32_t tick_ = 0, eventSeq_ = 0, sessionId_ = NowMs();
    // match (host)
    struct Tint { ActorRef actor; int8_t team = kNoTeam; int tries = 0; uint32_t next = 0; };
    Match match_;
    KillLog kills_;
    std::map<uint8_t, int8_t> slotTeam_;
    std::map<uint8_t, ActorRef> bots_;
    UObject* hostWillie_ = nullptr;
    bool adoptStock_ = false, wavesEnabled_ = false, sidesValid_ = false, infoDirty_ = false, progression_ = false;
    bool oneFight_ = false;                          // this level holds a Progression fight: the HUD shows one fight, no waves
    int wave_ = 0;
    uint32_t nextCorpseSweep_ = 0;
    uint32_t nextScan_ = 0, nextInfo_ = 0;
    std::vector<Point3> sidePoints_;
    std::vector<int8_t> sides_;
    // match (both)
    MatchInfo info_;
    uint32_t infoAt_ = 0;     // when info_ was made (host) or arrived (client), for Extrapolate
    std::deque<FeedItem> feed_;
    BannerTracker banner_;    // the centre banner the match's changes raise
    std::vector<Tint> tints_;
    bool ownDeadPending_ = false;
    std::deque<uint8_t> respawnQueue_;   // host, owner sync: entities whose respawn waits for its tick (RespawnQueued)
    uint32_t ownDiedAt_ = 0;
    GiveUpHold giveUp_;
    DropClearWatch dropWatch_;                       // Duel round / rematch: the drops are cleared (DropClearFor)
    LooseTimer looseTimer_;                          // loose armour pieces here (DropsTick)
    bool looseOn_ = false;                           // the timer has its baseline for this match
    uint32_t nextLooseSweep_ = 0;
    uint64_t loosePurged_ = 0;
    std::set<uint8_t> looted_;                       // Survival bots that dropped their loot
    static constexpr uint32_t kLooseSweepMs = 5000;
    GearPool gearPool_;
    uint32_t nextGearAhead_ = 0;
    uint32_t ownBlob_ = 0;    // the loadout blob of this machine's last own Spawn (client): a change is new gear
    uint8_t chatPhase_ = 0;   // the match phase the chat last saw (MatchPhase)
    // client
    uint32_t hostPeer_ = 0, uploadId_ = 0;
    bool welcomed_ = false, haveSnapshot_ = false, snappedOwn_ = false;
    // A fighter was destroyed or spawned this tick (about 60 ms each): the next one waits for the next frame. A Duel
    // round reset did four in one tick (two-player logs 8 and 9: 250 ms).
    bool bodyWork_ = false;
    uint8_t mySlot_ = 0;
    ClientWorld world_;
    BlobStore clientBlobs_;
    BlobAssembler clientAssembler_;
    std::vector<uint8_t> lastUpload_;
    std::map<uint8_t, ActorRef> puppets_;
    LevelSpawnGate levelGate_;                   // owner sync: the level's start-up cleanup is over (hsm_level_gate.h)
    uint32_t nextGateProbe_ = 0;
    bool managerProbed_ = false;   // this level's manager seen by ClientGateOpen (its Level Load Delay logged)
    bool pauseLiftArmed_ = false;                    // LiftPauseFromWidgetTick: set per tick, cleared on travel
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
    std::unordered_map<UClass*, HitKind> hitKind_;
    bool hitKindOwner_ = false;
    std::unordered_map<UObject*, UObject*> fighterOfFrame_;
    enum class SpawnIgnoreState : uint8_t { Outside, InTick, Cached };
    std::vector<UObject*> spawnIgnore_;     // SpawnIgnore, this Tick only
    SpawnIgnoreState spawnIgnoreState_ = SpawnIgnoreState::Outside;
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
    uint32_t startedAt_ = 0, quitAfterMs_ = 0, nextAuto_ = 0, lastModeTick_ = 0, lastEarlyPump_ = 0, lastPauseCheck_ = 0, noPawnSince_ = 0;
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
    uint32_t timelinesBlocked_ = 0;             // a stand-in's own timeline callbacks blocked (BlockStandInTimeline), per health window
    static constexpr size_t kFrameSamples = 4096;   // 5 s of frames at up to ~800 fps; the rest of a window is not sampled
    std::vector<double> frameMs_, tickMs_;
    std::array<double, kPumpParts> pumpMs_{}, pumpMaxMs_{};   // Timed: per part, this window
    bool sampling_ = false;
    uint64_t vmNsAtWindow_ = 0;
    uint64_t peNsAtWindow_ = 0, peCallsAtWindow_ = 0, peAllAtWindow_ = 0;
    uint64_t lookupsAtWindow_ = 0, lookupMissesAtWindow_ = 0;
};

std::unique_ptr<SessionImpl> g_session;
UClass* g_gameModeClass = nullptr;
}  // namespace

bool StartMod(const Config& cfg) {
    if (!Init()) return false;
    game::SetStandInGripLock(cfg.standInGripLock);
    g_session = std::make_unique<SessionImpl>(cfg);
    if (!g_session->Start()) return false;
    if (int port = CmdArgInt(CommandLineUtf8(), "hsmbridge", 0, 1024, 65535)) agent::Start(uint16_t(port));
    // Pump the session from the GameMode's Blueprint tick (game thread, every frame, menus included).
    // Widgets keep ticking while the game is paused: let them pump the agent bridge too, and lift a match's pause.
    HookFunctionsMatching([](const std::string& n) { return n == "Tick"; }, [](UObject*, UFunction*, void*) {
        g_session->LiftPauseFromWidgetTick();
        g_session->PumpAgentEarly();
        return false;
    });
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
            if (!SameNameAscii(ObjectName(reinterpret_cast<UObject*>(c)), kGameModeClass)) { g_session->PumpAgentEarly(); return false; }
            g_gameModeClass = c;
        }
        if (ClassOf(obj) != g_gameModeClass) { g_session->PumpAgentEarly(); return false; }
        float dt = 0.f;
        if (FProperty* p = FindProperty(fn, "DeltaSeconds")) std::memcpy(&dt, static_cast<uint8_t*>(params) + PropertyOffset(p), sizeof dt);
        if (!(dt > 0.f && dt < 1.f)) dt = 1.f / 60.f;
        const auto t0 = std::chrono::steady_clock::now();
        g_session->Tick(obj, dt);
        const auto spent = std::chrono::steady_clock::now() - t0;
        DiscountHookTime(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count()));   // reported as the mod tick
        g_session->NoteTickCost(std::chrono::duration<double, std::milli>(spent).count());
        return false;
    });
    // A stand-in's own timelines tick with its TimelineComponents, not with the blocked ReceiveTick. These ones run the
    // Willie's own logic, which on a stand-in is the owner's to run and stream:
    //   Timeline_0/1 (Willie_BP.cpp:21168, 21232), the grip check: with the stand-in's tonus at zero, the hand's constraint
    //     force soon beats its strength and the hand lets go: Set Up Right/Left Hand Weapon with no weapon (a drop, left
    //     floating by the stand-in's gravity-off weapon), Release Grab, or Get Damage on the hand;
    //   Timeline (21139) and Timeline_10 (21297, 21302), waking up: they clear Invulnerable, and Timeline_10 overwrites
    //     Consciousness.
    HookFunctionsMatching([](const std::string& n) {
        return n == "Timeline_0__UpdateFunc" || n == "Timeline_1__UpdateFunc" || n == "Timeline__UpdateFunc" ||
               n == "Timeline_10__UpdateFunc" || n == "Timeline_10__FinishedFunc";
    }, [](UObject* obj, UFunction*, void*) { return g_session->BlockStandInTimeline(obj); });
    if (!InstallProcessEventHook()) return false;
    HSM_LOG("session: started (%s)", RoleName(cfg.role));
    return true;
}

}  // namespace hsm

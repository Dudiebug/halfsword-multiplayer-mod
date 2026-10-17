#include "steam/steam_api.h"
#include "steam/hsm_steam_abi.h"
#include <unordered_map>
#include <tuple>
#include "log.h"
#include "ue/hook.h"
#include <windows.h>
#include <iterator>
#include <string>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace hsm::steam {
namespace {

using namespace abi;
constexpr int32_t kIdentitySteamId = 16;

constexpr int kCbLobbyEnter = 504, kCbLobbyData = 505, kCbLobbyChatUpdate = 506, kCbLobbyChatMsg = 507;
constexpr int kCbLobbyMatchList = 510, kCbLobbyCreated = 513, kCbJoinRequested = 333, kCbRichJoin = 337;
constexpr int kCbSessionRequest = 1251, kCbSessionFailed = 1252;
constexpr int kCbRelayStatus = 1281, kCbServersConnected = 101, kCbServerConnectFailure = 102, kCbServersDisconnected = 103;

// ---------------------------------------------------------------- flat API
HMODULE g_dll = nullptr;
void *g_mm = nullptr, *g_friends = nullptr, *g_user = nullptr, *g_utils = nullptr, *g_msgs = nullptr, *g_netUtils = nullptr;
template <class F> F Fn(const char* name) { return g_dll ? reinterpret_cast<F>(GetProcAddress(g_dll, name)) : nullptr; }
#define FLAT(ret, name, ...) using name##_t = ret (*)(__VA_ARGS__); name##_t name = nullptr;
struct Api {
    FLAT(int32_t, GetHSteamPipe)
    FLAT(void, RegisterCallback, void*, int)
    FLAT(void, RunCallbacks)
    FLAT(uint64_t, CreateLobby, void*, int, int)
    FLAT(uint64_t, JoinLobby, void*, uint64_t)
    FLAT(void, LeaveLobby, void*, uint64_t)
    FLAT(bool, SetLobbyData, void*, uint64_t, const char*, const char*)
    FLAT(const char*, GetLobbyData, void*, uint64_t, const char*)
    FLAT(void, SetLobbyMemberData, void*, uint64_t, const char*, const char*)
    FLAT(const char*, GetLobbyMemberData, void*, uint64_t, uint64_t, const char*)
    FLAT(int, GetNumLobbyMembers, void*, uint64_t)
    FLAT(uint64_t, GetLobbyMemberByIndex, void*, uint64_t, int)
    FLAT(uint64_t, GetLobbyOwner, void*, uint64_t)
    FLAT(bool, SetLobbyType, void*, uint64_t, int)
    FLAT(bool, SetLobbyJoinable, void*, uint64_t, bool)
    FLAT(uint64_t, RequestLobbyList, void*)
    FLAT(uint64_t, GetLobbyByIndex, void*, int)
    FLAT(void, AddRequestLobbyListStringFilter, void*, const char*, const char*, int)
    FLAT(bool, SendLobbyChatMsg, void*, uint64_t, const void*, int)
    FLAT(int, GetLobbyChatEntry, void*, uint64_t, int, uint64_t*, void*, int, int*)
    FLAT(void, ActivateGameOverlayInviteDialog, void*, uint64_t)
    FLAT(bool, SetRichPresence, void*, const char*, const char*)
    FLAT(const char*, GetFriendPersonaName, void*, uint64_t)
    FLAT(const char*, GetPersonaName, void*)
    FLAT(uint64_t, GetSteamID, void*)
    FLAT(bool, IsAPICallCompleted, void*, uint64_t, bool*)
    FLAT(bool, GetAPICallResult, void*, uint64_t, void*, int, int, bool*)
    FLAT(uint32_t, GetAppID, void*)
    FLAT(bool, IsOverlayEnabled, void*)
    FLAT(int, SendMessageToUser, void*, const NetIdentity&, const void*, uint32_t, int, int)
    FLAT(int, ReceiveMessagesOnChannel, void*, int, NetMessage**, int)
    FLAT(bool, AcceptSessionWithUser, void*, const NetIdentity&)
    FLAT(bool, CloseSessionWithUser, void*, const NetIdentity&)
    FLAT(int, GetSessionConnectionInfo, void*, const NetIdentity&, NetConnectionInfo*, NetStatus*)
    FLAT(void, InitRelayNetworkAccess, void*)
    FLAT(int64_t, GetLocalTimestamp, void*)
    FLAT(void, ReleaseMessage, void*)
} g;
#undef FLAT

NetIdentity Identity(SteamId id) { NetIdentity n{}; n.type = kIdentitySteamId; n.size = 8; n.steamId = id; return n; }

// ---------------------------------------------------------------- callbacks
// CCallbackBase as laid out by MSVC: vptr, uint8 flags, int callback id. The vtable has Run(p, ioFailure, call),
// Run(p) and GetCallbackSizeBytes(). Both Run overloads share one handler: rcx = this, rdx = payload in each case.
struct CallbackObj { void** vtable; uint8_t flags; int32_t id; int32_t size; };
std::mutex g_mu;
std::deque<Event> g_events;
std::unordered_map<SteamId, std::tuple<int, uint32_t, int32_t>> g_connectionStates;
void Push(Event e) { std::lock_guard<std::mutex> lock(g_mu); g_events.push_back(std::move(e)); }

void __cdecl OnCallback(CallbackObj* self, void* p) {
    Event e{};
    switch (self->id) {
        case kCbLobbyEnter: { auto* c = static_cast<LobbyEnter*>(p); e.kind = Event::LobbyEntered; e.lobby = c->lobby; e.a = c->response; e.ok = c->response == 1; break; }
        case kCbLobbyData: { auto* c = static_cast<LobbyDataUpdate*>(p); e.kind = Event::LobbyDataChanged; e.lobby = c->lobby; e.user = c->member; e.ok = c->success != 0; break; }
        case kCbLobbyChatUpdate: { auto* c = static_cast<LobbyChatUpdate*>(p); e.kind = Event::LobbyMembersChanged; e.lobby = c->lobby; e.user = c->changed; e.a = c->state; break; }
        case kCbLobbyChatMsg: { auto* c = static_cast<LobbyChatMsg*>(p); e.kind = Event::LobbyChat; e.lobby = c->lobby; e.user = c->user; e.a = c->chatId; break; }
        case kCbJoinRequested: { auto* c = static_cast<GameLobbyJoinRequested*>(p); e.kind = Event::JoinRequested; e.lobby = c->lobby; e.user = c->friendId; break; }
        case kCbRichJoin: {   // GameRichPresenceJoinRequested_t { CSteamID friend; char connect[256]; }
            e.kind = Event::JoinRequested; std::memcpy(&e.user, p, 8);
            const char* s = static_cast<const char*>(p) + 8; e.text.assign(s, strnlen(s, 256));
            if (const char* id = std::strstr(e.text.c_str(), "+connect_lobby ")) e.lobby = std::strtoull(id + 15, nullptr, 10);
            break;
        }
        case kCbSessionRequest: { auto* c = static_cast<SessionRequest*>(p); e.kind = Event::SessionRequest; e.user = c->remote.steamId; break; }
        case kCbSessionFailed: {
            const auto& c = static_cast<SessionFailed*>(p)->info;
            e.kind = Event::SessionFailed; e.ok = false;
            if (c.remote.type == kIdentitySteamId) e.user = c.remote.steamId;
            e.a = uint32_t(c.endReason); e.text.assign(c.endDebug, strnlen(c.endDebug, sizeof c.endDebug));
            break;
        }
        case kCbRelayStatus: {
            const auto& c = *static_cast<RelayStatus*>(p);
            HSM_LOG("steam: callback id=%d relay avail=%d config=%d anyRelay=%d measuring=%d detail=%.256s",
                    self->id, c.available, c.networkConfig, c.anyRelay, c.measuring, c.debug);
            return;
        }
        case kCbServersConnected: e.kind = Event::ServersConnected; break;
        case kCbServersDisconnected: e.kind = Event::ServersDisconnected; e.ok = false; e.a = uint32_t(static_cast<ServersDisconnected*>(p)->result); break;
        case kCbServerConnectFailure: e.kind = Event::ServerConnectFailure; e.ok = false; e.a = uint32_t(static_cast<ServerConnectFailure*>(p)->result); break;
        default: return;
    }
    HSM_LOG("steam: callback id=%d bytes=%d lobby=%llu user=%llu ok=%d code=%u detail=%s", self->id, self->size,
            (unsigned long long)e.lobby, (unsigned long long)e.user, int(e.ok), e.a, e.text.c_str());
    Push(std::move(e));
}
int32_t __cdecl OnGetSize(CallbackObj* self) { return self->size; }
void* g_vtable[3] = { reinterpret_cast<void*>(&OnCallback), reinterpret_cast<void*>(&OnCallback), reinterpret_cast<void*>(&OnGetSize) };
CallbackObj g_callbacks[] = {
    {g_vtable, 0, kCbLobbyEnter, int32_t(sizeof(LobbyEnter))}, {g_vtable, 0, kCbLobbyData, int32_t(sizeof(LobbyDataUpdate))},
    {g_vtable, 0, kCbLobbyChatUpdate, int32_t(sizeof(LobbyChatUpdate))}, {g_vtable, 0, kCbLobbyChatMsg, int32_t(sizeof(LobbyChatMsg))},
    {g_vtable, 0, kCbJoinRequested, int32_t(sizeof(GameLobbyJoinRequested))}, {g_vtable, 0, kCbRichJoin, int32_t(sizeof(RichJoinRequested))},
    {g_vtable, 0, kCbSessionRequest, int32_t(sizeof(SessionRequest))}, {g_vtable, 0, kCbSessionFailed, int32_t(sizeof(SessionFailed))},
    {g_vtable, 0, kCbRelayStatus, int32_t(sizeof(RelayStatus))}, {g_vtable, 0, kCbServersConnected, 1},
    {g_vtable, 0, kCbServersDisconnected, int32_t(sizeof(ServersDisconnected))}, {g_vtable, 0, kCbServerConnectFailure, int32_t(sizeof(ServerConnectFailure))},
};

// ---------------------------------------------------------------- callback pumping
// The engine pumps SteamAPI_RunCallbacks only when OnlineSubsystemSteam is active. We hook the export to see
// whether anyone calls it; if nobody does shortly after init, we pump it ourselves from the game thread.
using RunCallbacksFn = void (*)();
RunCallbacksFn g_runOriginal = nullptr;
std::atomic<uint64_t> g_lastExternalPump{0};
thread_local bool t_selfPump = false;
void RunCallbacksDetour() { if (!t_selfPump) g_lastExternalPump = GetTickCount64(); g_runOriginal(); }
bool g_selfPump = false;
ULONGLONG g_readyAt = 0;

// ---------------------------------------------------------------- call results (polled)
struct PendingCall { ApiCall call; int id; Event::Kind kind; };
std::vector<PendingCall> g_pending;
bool g_ready = false;
}  // namespace

// The engine's own copy, relative to the game exe: HalfswordUE5/Binaries/Win64 -> Engine/Binaries/ThirdParty/...
HMODULE LoadBundledSteamApi() {
    wchar_t exe[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!n || n >= std::size(exe)) return nullptr;
    std::wstring dir(exe, n);
    for (int up = 0; up < 4; ++up) { size_t cut = dir.find_last_of(L"\\/"); if (cut == std::wstring::npos) return nullptr; dir.resize(cut); }
    std::wstring path = dir + L"\\Engine\\Binaries\\ThirdParty\\Steamworks\\Steamv157\\Win64\\steam_api64.dll";
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    HSM_LOG("steam: loading the game's steam_api64.dll %s", m ? "ok" : "failed");
    return m;
}

bool Ready() { return g_ready; }

bool Init() {
    if (g_ready) return true;
    static int lastStep = -1;
    auto step = [](int n, const char* what) { if (n != lastStep) { lastStep = n; HSM_LOG("steam: %s", what); } };
    if (!g_dll) g_dll = GetModuleHandleW(L"steam_api64.dll");
    if (!g_dll) g_dll = LoadBundledSteamApi();
    if (!g_dll) { step(1, "steam_api64.dll is not loaded and the game's copy could not be loaded"); return false; }
    g.GetHSteamPipe = Fn<Api::GetHSteamPipe_t>("SteamAPI_GetHSteamPipe");
    if (!g.GetHSteamPipe) { step(2, "steam_api64.dll has no SteamAPI_GetHSteamPipe"); return false; }
    // Half Sword ships OnlineSubsystemSteam disabled, so the game itself never calls SteamAPI_Init.
    static bool triedInit = false;
    if (g.GetHSteamPipe() == 0 && !triedInit) {
        triedInit = true;
        auto init = Fn<bool (*)()>("SteamAPI_Init");
        bool ok = init && init();
        HSM_LOG("steam: the game had not initialised Steam; SteamAPI_Init() %s", ok ? "succeeded" : "failed (is Steam running, and was the game started by Steam?)");
    }
    if (g.GetHSteamPipe() == 0) { step(3, "Steam is not initialised (no pipe)"); return false; }
    g_mm = Fn<void* (*)()>("SteamAPI_SteamMatchmaking_v009") ? Fn<void* (*)()>("SteamAPI_SteamMatchmaking_v009")() : nullptr;
    g_friends = Fn<void* (*)()>("SteamAPI_SteamFriends_v017") ? Fn<void* (*)()>("SteamAPI_SteamFriends_v017")() : nullptr;
    // SDK 1.57 declares v023, but the game's DLL in Steamv157 exports only v021.
    // GetSteamID is the unchanged flat operation we use on either interface.
    auto userAccessor = Fn<void* (*)()>("SteamAPI_SteamUser_v023");
    if (!userAccessor) { userAccessor = Fn<void* (*)()>("SteamAPI_SteamUser_v021"); HSM_LOG("steam: using bundled SteamUser v021 compatibility accessor"); }
    g_user = userAccessor ? userAccessor() : nullptr;
    g_utils = Fn<void* (*)()>("SteamAPI_SteamUtils_v010") ? Fn<void* (*)()>("SteamAPI_SteamUtils_v010")() : nullptr;
    g_msgs = Fn<void* (*)()>("SteamAPI_SteamNetworkingMessages_SteamAPI_v002") ? Fn<void* (*)()>("SteamAPI_SteamNetworkingMessages_SteamAPI_v002")() : nullptr;
    g_netUtils = Fn<void* (*)()>("SteamAPI_SteamNetworkingUtils_SteamAPI_v004") ? Fn<void* (*)()>("SteamAPI_SteamNetworkingUtils_SteamAPI_v004")() : nullptr;
#define BIND(field, name) g.field = Fn<Api::field##_t>(name); if (!g.field) { HSM_LOG("steam: missing export %s", name); return false; }
    BIND(RegisterCallback, "SteamAPI_RegisterCallback")
    BIND(RunCallbacks, "SteamAPI_RunCallbacks")
    BIND(CreateLobby, "SteamAPI_ISteamMatchmaking_CreateLobby")
    BIND(JoinLobby, "SteamAPI_ISteamMatchmaking_JoinLobby")
    BIND(LeaveLobby, "SteamAPI_ISteamMatchmaking_LeaveLobby")
    BIND(SetLobbyData, "SteamAPI_ISteamMatchmaking_SetLobbyData")
    BIND(GetLobbyData, "SteamAPI_ISteamMatchmaking_GetLobbyData")
    BIND(SetLobbyMemberData, "SteamAPI_ISteamMatchmaking_SetLobbyMemberData")
    BIND(GetLobbyMemberData, "SteamAPI_ISteamMatchmaking_GetLobbyMemberData")
    BIND(GetNumLobbyMembers, "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers")
    BIND(GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex")
    BIND(GetLobbyOwner, "SteamAPI_ISteamMatchmaking_GetLobbyOwner")
    BIND(SetLobbyType, "SteamAPI_ISteamMatchmaking_SetLobbyType")
    BIND(SetLobbyJoinable, "SteamAPI_ISteamMatchmaking_SetLobbyJoinable")
    BIND(RequestLobbyList, "SteamAPI_ISteamMatchmaking_RequestLobbyList")
    BIND(GetLobbyByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyByIndex")
    BIND(AddRequestLobbyListStringFilter, "SteamAPI_ISteamMatchmaking_AddRequestLobbyListStringFilter")
    BIND(SendLobbyChatMsg, "SteamAPI_ISteamMatchmaking_SendLobbyChatMsg")
    BIND(GetLobbyChatEntry, "SteamAPI_ISteamMatchmaking_GetLobbyChatEntry")
    BIND(ActivateGameOverlayInviteDialog, "SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog")
    BIND(SetRichPresence, "SteamAPI_ISteamFriends_SetRichPresence")
    BIND(GetFriendPersonaName, "SteamAPI_ISteamFriends_GetFriendPersonaName")
    BIND(GetPersonaName, "SteamAPI_ISteamFriends_GetPersonaName")
    BIND(GetSteamID, "SteamAPI_ISteamUser_GetSteamID")
    BIND(IsAPICallCompleted, "SteamAPI_ISteamUtils_IsAPICallCompleted")
    BIND(GetAPICallResult, "SteamAPI_ISteamUtils_GetAPICallResult")
    BIND(GetAppID, "SteamAPI_ISteamUtils_GetAppID")
    BIND(IsOverlayEnabled, "SteamAPI_ISteamUtils_IsOverlayEnabled")
    BIND(SendMessageToUser, "SteamAPI_ISteamNetworkingMessages_SendMessageToUser")
    BIND(ReceiveMessagesOnChannel, "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel")
    BIND(AcceptSessionWithUser, "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser")
    BIND(CloseSessionWithUser, "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser")
    BIND(GetSessionConnectionInfo, "SteamAPI_ISteamNetworkingMessages_GetSessionConnectionInfo")
    BIND(InitRelayNetworkAccess, "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess")
    BIND(ReleaseMessage, "SteamAPI_SteamNetworkingMessage_t_Release")
#undef BIND
    // Optional: the clock of m_usecTimeReceived, so a message's age is known (pose arrival times); without it the
    // session stamps arrivals when it polls them.
    g.GetLocalTimestamp = Fn<Api::GetLocalTimestamp_t>("SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp");
    if (!g.GetLocalTimestamp) HSM_LOG("steam: no SteamAPI_ISteamNetworkingUtils_GetLocalTimestamp; arrival times are the poll time");
    if (!g_mm || !g_friends || !g_user || !g_utils || !g_msgs || !g_netUtils) { HSM_LOG("steam: interface accessor returned null"); return false; }
    for (CallbackObj& cb : g_callbacks) g.RegisterCallback(&cb, cb.id);
    bool hooked = ue::HookNative(reinterpret_cast<void*>(g.RunCallbacks), reinterpret_cast<void*>(&RunCallbacksDetour), reinterpret_cast<void**>(&g_runOriginal), "SteamAPI_RunCallbacks");
    HSM_LOG("steam: callback pump observer %s", hooked ? "installed" : "unavailable; direct pump fallback enabled");
    g_readyAt = GetTickCount64();
    g_ready = true;
    HSM_LOG("steam: ready (app %u, user %llu, %s)", AppId(), (unsigned long long)MySteamId(), PersonaName(MySteamId()).c_str());
    return true;
}

void Tick() {
    if (!g_ready) return;
    const uint64_t external = g_lastExternalPump.load();
    bool selfPump = GetTickCount64() - (external ? external : g_readyAt) > 2000;
    if (g_selfPump != selfPump) {
        g_selfPump = selfPump;
        HSM_LOG("steam: callback pump source=%s", g_selfPump ? "mod" : "game");
    }
    if (g_selfPump) { t_selfPump = true; (g_runOriginal ? g_runOriginal : g.RunCallbacks)(); t_selfPump = false; }
    for (size_t i = 0; i < g_pending.size();) {
        PendingCall pc = g_pending[i];
        bool failed = false;
        if (!g.IsAPICallCompleted(g_utils, pc.call, &failed)) { ++i; continue; }
        g_pending.erase(g_pending.begin() + long(i));
        Event e{}; e.kind = pc.kind; e.ok = false;
        if (pc.id == kCbLobbyCreated) {
            LobbyCreated r{};
            if (g.GetAPICallResult(g_utils, pc.call, &r, sizeof r, pc.id, &failed) && !failed) { e.ok = r.result == 1; e.lobby = r.lobby; e.a = uint32_t(r.result); }
        } else if (pc.id == kCbLobbyMatchList) {
            LobbyMatchList r{};
            if (g.GetAPICallResult(g_utils, pc.call, &r, sizeof r, pc.id, &failed) && !failed) {
                e.ok = true;
                for (uint32_t k = 0; k < r.count && k < 200; ++k) e.lobbies.push_back(g.GetLobbyByIndex(g_mm, int(k)));
            }
        }
        HSM_LOG("steam: call-result handle=%llu id=%d ok=%d io_failure=%d lobby=%llu result=%u count=%zu",
                (unsigned long long)pc.call, pc.id, int(e.ok), int(failed), (unsigned long long)e.lobby, e.a, e.lobbies.size());
        Push(std::move(e));
    }
}

bool Poll(Event& out) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_events.empty()) return false;
    out = std::move(g_events.front()); g_events.pop_front();
    if (out.kind == Event::LobbyChat) {   // fetch the text on the game thread
        char buf[2048] = {}; uint64_t user = 0; int type = 0;
        int n = g.GetLobbyChatEntry(g_mm, out.lobby, int(out.a), &user, buf, sizeof buf - 1, &type);
        out.text.assign(buf, n > 0 ? strnlen(buf, size_t(n)) : 0);
    }
    return true;
}

SteamId MySteamId() { return g_ready ? g.GetSteamID(g_user) : 0; }
std::string PersonaName(SteamId user) {
    if (!g_ready) return {};
    const char* n = user == MySteamId() ? g.GetPersonaName(g_friends) : g.GetFriendPersonaName(g_friends, user);
    return n ? n : "";
}
uint32_t AppId() { return g_ready ? g.GetAppID(g_utils) : 0; }

std::string LaunchConnectLobby() {
    std::wstring cmd = GetCommandLineW();
    size_t p = cmd.find(L"+connect_lobby");
    if (p == std::wstring::npos) return {};
    p += 14; while (p < cmd.size() && cmd[p] == L' ') ++p;
    std::string id; while (p < cmd.size() && cmd[p] >= L'0' && cmd[p] <= L'9') id.push_back(char(cmd[p++]));
    if (!id.empty()) HSM_LOG("steam: launch invite lobby=%s", id.c_str());
    return id;
}

void CreateLobby(LobbyType type, int maxMembers) {
    if (!g_ready) return;
    ApiCall call = g.CreateLobby(g_mm, int(type), maxMembers);
    HSM_LOG("steam: CreateLobby type=%d max=%d handle=%llu", int(type), maxMembers, (unsigned long long)call);
    if (call) g_pending.push_back({call, kCbLobbyCreated, Event::LobbyCreated});
    else { Event e{}; e.kind = Event::LobbyCreated; e.ok = false; Push(std::move(e)); }
}
void JoinLobby(SteamId lobby) {
    if (!g_ready) return;
    ApiCall call = g.JoinLobby(g_mm, lobby);
    HSM_LOG("steam: JoinLobby lobby=%llu handle=%llu", (unsigned long long)lobby, (unsigned long long)call);
    if (!call) { Event e{}; e.kind = Event::LobbyEntered; e.lobby = lobby; e.ok = false; Push(std::move(e)); }
}
void LeaveLobby(SteamId lobby) { if (g_ready && lobby) { HSM_LOG("steam: LeaveLobby lobby=%llu", (unsigned long long)lobby); g.LeaveLobby(g_mm, lobby); } }
bool SetLobbyType(SteamId lobby, LobbyType type) { return g_ready && g.SetLobbyType(g_mm, lobby, int(type)); }
bool SetLobbyJoinable(SteamId lobby, bool joinable) { return g_ready && g.SetLobbyJoinable(g_mm, lobby, joinable); }
bool SetLobbyData(SteamId lobby, const std::string& k, const std::string& v) { return g_ready && g.SetLobbyData(g_mm, lobby, k.c_str(), v.c_str()); }
std::string LobbyData(SteamId lobby, const std::string& k) { const char* s = g_ready ? g.GetLobbyData(g_mm, lobby, k.c_str()) : nullptr; return s ? s : ""; }
void SetMemberData(SteamId lobby, const std::string& k, const std::string& v) { if (g_ready) g.SetLobbyMemberData(g_mm, lobby, k.c_str(), v.c_str()); }
std::string MemberData(SteamId lobby, SteamId user, const std::string& k) { const char* s = g_ready ? g.GetLobbyMemberData(g_mm, lobby, user, k.c_str()) : nullptr; return s ? s : ""; }
std::vector<SteamId> LobbyMembers(SteamId lobby) {
    std::vector<SteamId> out; if (!g_ready || !lobby) return out;
    int n = g.GetNumLobbyMembers(g_mm, lobby);
    for (int i = 0; i < n && i < 64; ++i) out.push_back(g.GetLobbyMemberByIndex(g_mm, lobby, i));
    return out;
}
SteamId LobbyOwner(SteamId lobby) { return g_ready && lobby ? g.GetLobbyOwner(g_mm, lobby) : 0; }
bool SendLobbyChat(SteamId lobby, const std::string& t) { return g_ready && g.SendLobbyChatMsg(g_mm, lobby, t.c_str(), int(t.size() + 1)); }
void RequestLobbyList(const std::string& key, const std::string& value) {
    if (!g_ready) return;
    if (!key.empty()) g.AddRequestLobbyListStringFilter(g_mm, key.c_str(), value.c_str(), 0 /* equal */);
    ApiCall call = g.RequestLobbyList(g_mm);
    HSM_LOG("steam: RequestLobbyList handle=%llu", (unsigned long long)call);
    if (call) g_pending.push_back({call, kCbLobbyMatchList, Event::LobbyList});
    else { Event e{}; e.kind = Event::LobbyList; e.ok = false; Push(std::move(e)); }
}
void InviteOverlay(SteamId lobby) { if (g_ready) { HSM_LOG("steam: invite overlay lobby=%llu enabled=%d", (unsigned long long)lobby, int(g.IsOverlayEnabled(g_utils))); g.ActivateGameOverlayInviteDialog(g_friends, lobby); } }
void SetRichPresence(const std::string& k, const std::string& v) { if (g_ready) { bool ok = g.SetRichPresence(g_friends, k.c_str(), v.c_str()); HSM_LOG("steam: rich-presence key=%s value=%s ok=%d", k.c_str(), v.c_str(), int(ok)); } }

void InitRelay() { if (g_ready) g.InitRelayNetworkAccess(g_netUtils); }
int Send(SteamId to, int channel, const void* data, uint32_t size, int flags) {
    if (!g_ready) return 3; // k_EResultNoConnection
    NetIdentity id = Identity(to);
    return g.SendMessageToUser(g_msgs, id, data, size, flags, channel);
}
size_t Receive(int channel, std::vector<Message>& out, int max) {
    if (!g_ready) return 0;
    NetMessage* msgs[64];
    int n = g.ReceiveMessagesOnChannel(g_msgs, channel, msgs, max < 64 ? max : 64);
    const int64_t nowUs = n > 0 && g.GetLocalTimestamp ? g.GetLocalTimestamp(g_netUtils) : 0;
    for (int i = 0; i < n; ++i) {
        NetMessage* m = msgs[i];
        if (m->peer.type == kIdentitySteamId && m->size >= 0) {
            Message o; o.from = m->peer.steamId; o.channel = channel;
            if (nowUs > 0 && m->usecReceived > 0 && m->usecReceived <= nowUs) o.ageMs = double(nowUs - m->usecReceived) / 1000.0;
            o.data.assign(static_cast<uint8_t*>(m->data), static_cast<uint8_t*>(m->data) + m->size);
            out.push_back(std::move(o));
        }
        g.ReleaseMessage(m);
    }
    return size_t(n > 0 ? n : 0);
}
void AcceptSession(SteamId user) { if (g_ready) { NetIdentity id = Identity(user); bool ok = g.AcceptSessionWithUser(g_msgs, id); HSM_LOG("steam-net: accept user=%llu ok=%d", (unsigned long long)user, int(ok)); } }
void CloseSession(SteamId user) { if (g_ready) { NetIdentity id = Identity(user); bool ok = g.CloseSessionWithUser(g_msgs, id); g_connectionStates.erase(user); HSM_LOG("steam-net: close user=%llu ok=%d", (unsigned long long)user, int(ok)); } }
int PingMs(SteamId user) {
    if (!g_ready) return -1;
    NetIdentity id = Identity(user);
    NetStatus status{};
    int state = g.GetSessionConnectionInfo(g_msgs, id, nullptr, &status);
    return state == 3 ? status.ping : -1;
}
void LogSessionState(SteamId user) {
    if (!g_ready) return;
    NetIdentity id = Identity(user); NetConnectionInfo info{}; NetStatus status{};
    int state = g.GetSessionConnectionInfo(g_msgs, id, &info, &status);
    auto diagnostic = std::make_tuple(state, info.relayPop, info.flags);
    auto it = g_connectionStates.find(user);
    if (it != g_connectionStates.end() && it->second == diagnostic) return;
    g_connectionStates[user] = diagnostic;
    HSM_LOG("steam-net: state user=%llu state=%d ping=%d relay_pop=%u flags=%d reason=%d detail=%.128s",
            (unsigned long long)user, state, status.ping, info.relayPop, info.flags, info.endReason, info.endDebug);
}

}  // namespace hsm::steam

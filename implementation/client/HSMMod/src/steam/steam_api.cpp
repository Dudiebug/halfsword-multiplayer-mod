#include "steam/steam_api.h"
#include "steam/hsm_steam_abi.h"
#include "log.h"
#include "ue/hook.h"
#include <windows.h>
#include <algorithm>
#include <iterator>
#include <string>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>

namespace hsm::steam {
namespace {

using namespace abi;   // Steamworks 1.57 layouts (steam/hsm_steam_abi.h)
constexpr int32_t kIdentitySteamId = 16;

constexpr int kCbLobbyEnter = 504, kCbLobbyData = 505, kCbLobbyChatUpdate = 506, kCbLobbyChatMsg = 507;
constexpr int kCbLobbyMatchList = 510, kCbLobbyCreated = 513, kCbJoinRequested = 333, kCbRichJoin = 337;
constexpr int kCbSessionRequest = 1251, kCbSessionFailed = 1252;
constexpr int kCbRelayStatus = 1281, kCbServerConnectFailure = 102, kCbServersDisconnected = 103;

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
    FLAT(int, GetMediumFriendAvatar, void*, uint64_t)
    FLAT(bool, GetImageSize, void*, int, uint32_t*, uint32_t*)
    FLAT(bool, GetImageRGBA, void*, int, uint8_t*, int)
    FLAT(uint64_t, GetSteamID, void*)
    FLAT(bool, IsAPICallCompleted, void*, uint64_t, bool*)
    FLAT(bool, GetAPICallResult, void*, uint64_t, void*, int, int, bool*)
    FLAT(uint32_t, GetAppID, void*)
    FLAT(int, SendMessageToUser, void*, const NetIdentity*, const void*, uint32_t, int, int)
    FLAT(int, ReceiveMessagesOnChannel, void*, int, NetMessage**, int)
    FLAT(bool, AcceptSessionWithUser, void*, const NetIdentity*)
    FLAT(bool, CloseSessionWithUser, void*, const NetIdentity*)
    FLAT(int, GetSessionConnectionInfo, void*, const NetIdentity*, NetConnectionInfo*, NetStatus*)
    FLAT(void, InitRelayNetworkAccess, void*)
    FLAT(int64_t, GetLocalTimestamp, void*)
    FLAT(void, ReleaseMessage, void*)
    FLAT(void, StartVoiceRecording, void*)
    FLAT(void, StopVoiceRecording, void*)
    FLAT(int, GetAvailableVoice, void*, uint32_t*, uint32_t*, uint32_t)
    FLAT(int, GetVoice, void*, bool, void*, uint32_t, uint32_t*, bool, void*, uint32_t, uint32_t*, uint32_t)
    FLAT(int, DecompressVoice, void*, const void*, uint32_t, void*, uint32_t, uint32_t*, uint32_t)
} g;
bool VoiceAvailableUnchecked() { return g.StartVoiceRecording && g.StopVoiceRecording && g.GetAvailableVoice && g.GetVoice && g.DecompressVoice; }
#undef FLAT

NetIdentity Identity(SteamId id) { NetIdentity n{}; n.type = kIdentitySteamId; n.size = 8; n.steamId = id; return n; }

// ---------------------------------------------------------------- callbacks
// CCallbackBase as laid out by MSVC: vptr, uint8 flags, int callback id. The vtable has Run(p, ioFailure, call),
// Run(p) and GetCallbackSizeBytes(). Both Run overloads share one handler: rcx = this, rdx = payload in each case.
struct CallbackObj { void** vtable; uint8_t flags; int32_t id; int32_t size; };
std::mutex g_mu;
std::deque<Event> g_events;
void Push(Event e) { std::lock_guard<std::mutex> lock(g_mu); g_events.push_back(std::move(e)); }

void __cdecl OnCallback(CallbackObj* self, void* p) {
    Event e{};
    switch (self->id) {
        case kCbLobbyEnter: { auto* c = static_cast<LobbyEnter*>(p); e.kind = Event::LobbyEntered; e.lobby = c->lobby; e.ok = c->response == 1; break; }
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
            const NetConnectionInfo& c = static_cast<SessionFailed*>(p)->info;
            e.kind = Event::SessionFailed; e.ok = false;
            if (c.remote.type == kIdentitySteamId) e.user = c.remote.steamId;
            e.a = uint32_t(c.endReason); e.text.assign(c.endDebug, strnlen(c.endDebug, sizeof c.endDebug));
            break;
        }
        // Diagnostics only (from CSLewin's PR #1): why a relay or the Steam servers went away.
        case kCbRelayStatus: {
            const RelayStatus& c = *static_cast<RelayStatus*>(p);
            HSM_LOG("steam: relay status avail=%d config=%d anyRelay=%d measuring=%d %.256s", c.available, c.networkConfig, c.anyRelay, c.measuring, c.debug);
            return;
        }
        case kCbServersDisconnected: HSM_LOG("steam: disconnected from the Steam servers (result %d)", static_cast<ServersDisconnected*>(p)->result); return;
        case kCbServerConnectFailure: HSM_LOG("steam: could not reach the Steam servers (result %d)", static_cast<ServerConnectFailure*>(p)->result); return;
        default: return;
    }
    Push(std::move(e));
}
int32_t __cdecl OnGetSize(CallbackObj* self) { return self->size; }
void* g_vtable[3] = { reinterpret_cast<void*>(&OnCallback), reinterpret_cast<void*>(&OnCallback), reinterpret_cast<void*>(&OnGetSize) };
CallbackObj g_callbacks[] = {
    {g_vtable, 0, kCbLobbyEnter, int32_t(sizeof(LobbyEnter))}, {g_vtable, 0, kCbLobbyData, int32_t(sizeof(LobbyDataUpdate))},
    {g_vtable, 0, kCbLobbyChatUpdate, int32_t(sizeof(LobbyChatUpdate))}, {g_vtable, 0, kCbLobbyChatMsg, int32_t(sizeof(LobbyChatMsg))},
    {g_vtable, 0, kCbJoinRequested, int32_t(sizeof(GameLobbyJoinRequested))}, {g_vtable, 0, kCbRichJoin, int32_t(sizeof(RichJoinRequested))},
    // SessionFailed was registered at 1024 bytes; it must be its real size, 696 (CSLewin's PR #1).
    {g_vtable, 0, kCbSessionRequest, int32_t(sizeof(SessionRequest))}, {g_vtable, 0, kCbSessionFailed, int32_t(sizeof(SessionFailed))},
    {g_vtable, 0, kCbRelayStatus, int32_t(sizeof(RelayStatus))}, {g_vtable, 0, kCbServersDisconnected, int32_t(sizeof(ServersDisconnected))},
    {g_vtable, 0, kCbServerConnectFailure, int32_t(sizeof(ServerConnectFailure))},
};

// ---------------------------------------------------------------- callback pumping
// The engine pumps SteamAPI_RunCallbacks only when OnlineSubsystemSteam is active. We hook the export to see
// whether anyone calls it; while nobody has for 2 s we pump it ourselves from the game thread, also when the hook
// could not be installed (then we call the export directly). From CSLewin's PR #1: the old check pumped only if the
// game had never pumped, so a game pump that stopped later left callbacks unpumped.
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
    // Pinned: the game unloads its copy while it shuts down, and anything the mod calls after that would jump into
    // unmapped memory (public #6). A pinned DLL stays mapped until the process ends.
    if (!g_dll && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"steam_api64.dll", &g_dll)) g_dll = nullptr;
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
    g_user = Fn<void* (*)()>("SteamAPI_SteamUser_v021") ? Fn<void* (*)()>("SteamAPI_SteamUser_v021")() : nullptr;
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
    // Optional: avatars on the multiplayer page; without them the rows keep an empty space.
    g.GetMediumFriendAvatar = Fn<Api::GetMediumFriendAvatar_t>("SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    g.GetImageSize = Fn<Api::GetImageSize_t>("SteamAPI_ISteamUtils_GetImageSize");
    g.GetImageRGBA = Fn<Api::GetImageRGBA_t>("SteamAPI_ISteamUtils_GetImageRGBA");
    if (!g.GetMediumFriendAvatar || !g.GetImageSize || !g.GetImageRGBA) HSM_LOG("steam: no avatar exports; the page shows no avatars");
    // Optional: proximity voice; without these exports the voice stays off.
    g.StartVoiceRecording = Fn<Api::StartVoiceRecording_t>("SteamAPI_ISteamUser_StartVoiceRecording");
    g.StopVoiceRecording = Fn<Api::StopVoiceRecording_t>("SteamAPI_ISteamUser_StopVoiceRecording");
    g.GetAvailableVoice = Fn<Api::GetAvailableVoice_t>("SteamAPI_ISteamUser_GetAvailableVoice");
    g.GetVoice = Fn<Api::GetVoice_t>("SteamAPI_ISteamUser_GetVoice");
    g.DecompressVoice = Fn<Api::DecompressVoice_t>("SteamAPI_ISteamUser_DecompressVoice");
    if (!VoiceAvailableUnchecked()) HSM_LOG("steam: no voice exports; proximity voice is off");
    if (!g_mm || !g_friends || !g_user || !g_utils || !g_msgs || !g_netUtils) { HSM_LOG("steam: interface accessor returned null"); return false; }
    for (CallbackObj& cb : g_callbacks) g.RegisterCallback(&cb, cb.id);
    const bool hooked = ue::HookNative(reinterpret_cast<void*>(g.RunCallbacks), reinterpret_cast<void*>(&RunCallbacksDetour),
                                       reinterpret_cast<void**>(&g_runOriginal), "SteamAPI_RunCallbacks");
    if (!hooked) HSM_LOG("steam: could not hook SteamAPI_RunCallbacks; the mod pumps it directly");
    g_readyAt = GetTickCount64();
    g_ready = true;
    HSM_LOG("steam: ready (app %u, user %llu, %s)", AppId(), (unsigned long long)MySteamId(), PersonaName(MySteamId()).c_str());
    return true;
}

void Tick() {
    if (!g_ready) return;
    const uint64_t external = g_lastExternalPump.load();
    const bool selfPump = GetTickCount64() - (external ? external : g_readyAt) > 2000;
    if (selfPump != g_selfPump) {
        g_selfPump = selfPump;
        HSM_LOG("steam: SteamAPI_RunCallbacks pumped by %s", selfPump ? "the mod (nobody else pumps it)" : "the game");
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
            if (g.GetAPICallResult(g_utils, pc.call, &r, sizeof r, pc.id, &failed) && !failed) { e.ok = r.result == 1; e.lobby = r.lobby; }
        } else if (pc.id == kCbLobbyMatchList) {
            LobbyMatchList r{};
            if (g.GetAPICallResult(g_utils, pc.call, &r, sizeof r, pc.id, &failed) && !failed) {
                e.ok = true;
                for (uint32_t k = 0; k < r.count && k < 200; ++k) e.lobbies.push_back(g.GetLobbyByIndex(g_mm, int(k)));
            }
        }
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

// The signed-in user never changes while the game runs, and the session asks for it on every hit event (Lobby::IsHost):
// under Proton every Steam call crosses from Wine into the Linux client, far slower than on Windows, so it is read once.
SteamId MySteamId() {
    static std::atomic<SteamId> mine{0};
    if (!g_ready) return 0;
    SteamId id = mine.load(std::memory_order_relaxed);
    if (!id) mine.store(id = g.GetSteamID(g_user), std::memory_order_relaxed);
    return id;
}
std::string PersonaName(SteamId user) {
    if (!g_ready) return {};
    const char* n = user == MySteamId() ? g.GetPersonaName(g_friends) : g.GetFriendPersonaName(g_friends, user);
    return n ? n : "";
}
uint32_t AppId() { return g_ready ? g.GetAppID(g_utils) : 0; }

// EVoiceResult: 0 OK, 1 not initialised, 2 not recording, 3 no data, 4 buffer too small, 5 corrupt, 6 restricted,
// 7 unsupported codec.
bool VoiceAvailable() { return g_ready && VoiceAvailableUnchecked(); }
void StartVoice() { if (VoiceAvailable()) g.StartVoiceRecording(g_user); }
void StopVoice() { if (VoiceAvailable()) g.StopVoiceRecording(g_user); }

bool TakeVoice(std::vector<uint8_t>& out) {
    out.clear();
    if (!VoiceAvailable()) return false;
    static int lastBad = 0;
    uint32_t avail = 0, unused = 0;
    int r = g.GetAvailableVoice(g_user, &avail, &unused, 0);
    if (r == 3 || r == 2 || (r == 0 && avail == 0)) return true;   // nothing yet, or not recording
    if (r == 0) {
        out.resize(std::min<uint32_t>(avail, 64 * 1024));   // all of it: a smaller buffer is refused and the backlog stays
        uint32_t written = 0;
        r = g.GetVoice(g_user, true, out.data(), uint32_t(out.size()), &written, false, nullptr, 0, nullptr, 0);
        if (r == 0) { out.resize(std::min<size_t>(written, out.size())); return true; }
        out.clear();
        if (r == 3) return true;
    }
    if (r != lastBad) { lastBad = r; HSM_LOG("steam: voice recording returned %d (6: restricted account, 1: Steam voice not ready)", r); }
    return false;
}

bool DecompressVoice(const uint8_t* data, size_t size, uint32_t sampleRate, std::vector<int16_t>& pcm) {
    pcm.clear();
    if (!VoiceAvailable() || !data || size == 0 || size > 8 * 1024) return false;
    pcm.resize(size_t(sampleRate) / 2);   // up to 500 ms per chunk; a bigger one asks again with room for it
    for (int tries = 0; tries < 2; ++tries) {
        uint32_t written = 0;
        const int r = g.DecompressVoice(g_user, data, uint32_t(size), pcm.data(), uint32_t(pcm.size() * 2), &written, sampleRate);
        if (r == 0) { pcm.resize(std::min<size_t>(written / 2, pcm.size())); return true; }
        if (r != 4 || written / 2 <= pcm.size() || written > 4 * sampleRate * 2) break;
        pcm.resize(written / 2);
    }
    pcm.clear();
    return false;
}

int MediumAvatar(SteamId user) { return g_ready && g.GetMediumFriendAvatar && user ? g.GetMediumFriendAvatar(g_friends, user) : 0; }

bool AvatarRGBA(int image, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba) {
    width = height = 0;
    rgba.clear();
    if (!g_ready || !g.GetImageSize || !g.GetImageRGBA || image <= 0) return false;
    if (!g.GetImageSize(g_utils, image, &width, &height) || !width || !height || width > 1024 || height > 1024) return false;
    rgba.resize(size_t(width) * height * 4);
    if (g.GetImageRGBA(g_utils, image, rgba.data(), int(rgba.size()))) return true;
    rgba.clear();
    return false;
}

std::string LaunchConnectLobby() {
    std::wstring cmd = GetCommandLineW();
    size_t p = cmd.find(L"+connect_lobby");
    if (p == std::wstring::npos) return {};
    p += 14; while (p < cmd.size() && cmd[p] == L' ') ++p;
    std::string id; while (p < cmd.size() && cmd[p] >= L'0' && cmd[p] <= L'9') id.push_back(char(cmd[p++]));
    return id;
}

// A zero call handle means Steam refused the call at once; it never completes, so it fails here (CSLewin's PR #1)
// instead of leaving the lobby state waiting forever.
void CreateLobby(LobbyType type, int maxMembers) {
    if (!g_ready) return;
    const ApiCall call = g.CreateLobby(g_mm, int(type), maxMembers);
    if (call) { g_pending.push_back({call, kCbLobbyCreated, Event::LobbyCreated}); return; }
    HSM_LOG("steam: CreateLobby returned no call handle");
    Event e{}; e.kind = Event::LobbyCreated; e.ok = false; Push(std::move(e));
}
void JoinLobby(SteamId lobby) {   // the result arrives as a LobbyEnter callback
    if (!g_ready) return;
    if (g.JoinLobby(g_mm, lobby)) return;
    HSM_LOG("steam: JoinLobby %llu returned no call handle", (unsigned long long)lobby);
    Event e{}; e.kind = Event::LobbyEntered; e.lobby = lobby; e.ok = false; Push(std::move(e));
}
void LeaveLobby(SteamId lobby) { if (g_ready && lobby) g.LeaveLobby(g_mm, lobby); }
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
    const ApiCall call = g.RequestLobbyList(g_mm);
    if (call) { g_pending.push_back({call, kCbLobbyMatchList, Event::LobbyList}); return; }
    HSM_LOG("steam: RequestLobbyList returned no call handle");
    Event e{}; e.kind = Event::LobbyList; e.ok = false; Push(std::move(e));
}
void InviteOverlay(SteamId lobby) { if (g_ready) g.ActivateGameOverlayInviteDialog(g_friends, lobby); }
void SetRichPresence(const std::string& k, const std::string& v) { if (g_ready) g.SetRichPresence(g_friends, k.c_str(), v.c_str()); }

void InitRelay() { if (g_ready) g.InitRelayNetworkAccess(g_netUtils); }
bool Send(SteamId to, int channel, const void* data, uint32_t size, int flags) {
    if (!g_ready) return false;
    NetIdentity id = Identity(to);
    return g.SendMessageToUser(g_msgs, &id, data, size, flags, channel) == 1;   // k_EResultOK
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
void AcceptSession(SteamId user) { if (g_ready) { NetIdentity id = Identity(user); g.AcceptSessionWithUser(g_msgs, &id); } }
void CloseSession(SteamId user) { if (g_ready) { NetIdentity id = Identity(user); g.CloseSessionWithUser(g_msgs, &id); } }
int PingMs(SteamId user) {
    if (!g_ready) return -1;
    NetIdentity id = Identity(user);
    NetStatus status{};
    // Only a connected session (k_ESteamNetworkingConnectionState_Connected) has a ping; any other state reads zeros.
    return g.GetSessionConnectionInfo(g_msgs, &id, nullptr, &status) == 3 ? status.ping : -1;
}

}  // namespace hsm::steam

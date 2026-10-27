#include "config.h"
#include "session/hsm_cmdline.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>

namespace hsm {
namespace {
constexpr char kDefaultIni[] =
    "; HalfSwordMP configuration. role = steam | host | client | off\n"
    "; steam: use the Multiplayer button on the main menu (Steam lobbies, invites, no port forwarding).\n"
    "; host/client: direct IP over UDP (LAN/testing; host forwards the port). off: completely vanilla.\n"
    "[net]\n"
    "role=steam\n"
    "host=127.0.0.1\n"
    "port=27777\n"
    "name=Player\n"
    "snapshot_hz=30\n"
    "input_hz=60\n"
    "; owner: each player's own machine simulates their fighter (no added latency on your own body);\n"
    "; host: the host simulates every fighter (the v0.3 behaviour). Only the host's setting matters.\n"
    "sync_mode=owner\n"
    "; the key that opens the chat: enter, t, y or u (Enter sends, Esc cancels)\n"
    "chat_key=enter\n"
    "; lock: the other players' hands hold their weapons firmly (default); free: each weapon is steered on its own\n"
    "standin_grip=lock\n"
    "; 1: the host starts a match even when another player runs a different mod build (testing only)\n"
    "allow_build_mismatch=0\n"
    "; 1: show the other fighters' names over their heads in a match; 0: off\n"
    "name_tags=1\n"
    "; in-game bug reports: type /report <what went wrong> in the chat to file a GitHub issue with your HSMMod log\n"
    "; (Steam IDs, IP addresses and your Windows user name are removed first). Nothing is sent unless you type it.\n"
    "reports=on\n"
    "report_url=https://reports.dutybug.net\n"
    "; role=server (a hosted retail instance with no fighter of its own that starts matches by itself):\n"
    "; server_transport=direct|steam (steam is experimental), server_format=ffa|duel|teams|survival|zombies,\n"
    "; server_maps=all or a comma list of map names, server_score_limit=0 (format default), server_time_limit_s=0 (none),\n"
    "; server_countdown_s=30, server_min_players=2 (1..5)\n"
    "server_transport=direct\n"
    "server_format=ffa\n"
    "server_maps=all\n"
    "server_score_limit=0\n"
    "server_time_limit_s=0\n"
    "server_countdown_s=30\n"
    "server_min_players=2\n";

std::string ReadString(const std::wstring& ini, const wchar_t* key, const char* def) {
    wchar_t buf[256];
    wchar_t wdef[256];
    MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 256);
    GetPrivateProfileStringW(L"net", key, wdef, buf, 256, ini.c_str());
    char out[3 * 256 + 1];                                   // every UTF-16 unit fits in at most 3 UTF-8 bytes
    if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr)) return def;
    return out;
}
int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

const char* RoleName(Role r) {
    return r == Role::Steam ? "steam" : r == Role::Host ? "host" : r == Role::Client ? "client" : r == Role::Server ? "server" : "off";
}
static Role ParseRole(const std::string& r) {
    return r == "host" ? Role::Host : r == "client" ? Role::Client : r == "off" ? Role::Off : r == "server" ? Role::Server : Role::Steam;
}

Config LoadConfig() {
    std::wstring ini = ModFilePath(L"HSMMod.ini");
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"w")) { std::fputs(kDefaultIni, f); std::fclose(f); }
    }
    Config c;
    std::string role = ReadString(ini, L"role", "steam");
    c.role = ParseRole(role);
    c.hostAddress = ReadString(ini, L"host", "127.0.0.1");
    c.port = uint16_t(Clamp(std::atoi(ReadString(ini, L"port", "27777").c_str()), 1024, 65535));
    c.playerName = ReadString(ini, L"name", "Player").substr(0, 32);
    c.snapshotHz = Clamp(std::atoi(ReadString(ini, L"snapshot_hz", "30").c_str()), 5, 120);
    c.inputHz = Clamp(std::atoi(ReadString(ini, L"input_hz", "60").c_str()), 10, 240);
    c.ownerSync = ReadString(ini, L"sync_mode", "owner") != "host";
    const std::string chatKey = ReadString(ini, L"chat_key", "enter");
    if (!ParseChatKey(chatKey, c.chatKey)) HSM_LOG("config: chat_key=%s is not enter, t, y or u; using enter", chatKey.c_str());
    c.standInGripLock = ReadString(ini, L"standin_grip", "lock") != "free";
    c.allowBuildMismatch = ReadString(ini, L"allow_build_mismatch", "0") == "1";
    c.nameTags = ReadString(ini, L"name_tags", "1") != "0";
    c.reports = ReadString(ini, L"reports", "on") != "off";
    c.reportUrl = ReadString(ini, L"report_url", "https://reports.dutybug.net");
    c.serverTransport = ReadString(ini, L"server_transport", "direct") == "steam" ? "steam" : "direct";
    c.serverFormat = ReadString(ini, L"server_format", "ffa");
    c.serverMaps = ReadString(ini, L"server_maps", "all");
    c.serverScoreLimit = Clamp(std::atoi(ReadString(ini, L"server_score_limit", "0").c_str()), 0, 100);
    c.serverTimeLimitS = Clamp(std::atoi(ReadString(ini, L"server_time_limit_s", "0").c_str()), 0, 3600);
    c.serverCountdownS = Clamp(std::atoi(ReadString(ini, L"server_countdown_s", "30").c_str()), 0, 600);
    c.serverMinPlayers = Clamp(std::atoi(ReadString(ini, L"server_min_players", "2").c_str()), 1, 5);
    // Command-line overrides (several instances share one ini, e.g. a local host + client test).
    std::string cl = CommandLineUtf8();
    if (CommandLineIgnoresHsmFlags(cl)) {   // M6: a Steam join launch (+connect_lobby) cannot steer the mod through -hsm* flags
        HSM_LOG("config: +connect_lobby on the command line, -hsm* flags ignored");
    } else {
        if (auto r = CmdArg(cl, "hsmrole")) c.role = ParseRole(*r);
        if (auto h = CmdArg(cl, "hsmhost"); h && !h->empty()) c.hostAddress = *h;
        c.port = uint16_t(CmdArgInt(cl, "hsmport", c.port, 1024, 65535));
        if (auto n = CmdArg(cl, "hsmname"); n && !n->empty()) c.playerName = n->substr(0, 32);
        if (auto m = CmdArg(cl, "hsmsync"); m && !m->empty()) c.ownerSync = *m != "host";
        if (auto k = CmdArg(cl, "hsmchatkey"); k && !ParseChatKey(*k, c.chatKey))
            HSM_LOG("config: -hsmchatkey=%s is not enter, t, y or u; ignored", k->c_str());
    }
    HSM_LOG("config: role=%s host=%s port=%u snapshot_hz=%d input_hz=%d sync_mode=%s chat_key=%s standin_grip=%s allow_build_mismatch=%d", RoleName(c.role),
            c.hostAddress.c_str(), c.port, c.snapshotHz, c.inputHz, c.ownerSync ? "owner" : "host", ChatKeyName(c.chatKey), c.standInGripLock ? "lock" : "free",
            c.allowBuildMismatch ? 1 : 0);
    HSM_LOG("config: reports=%s name_tags=%d", c.reports ? "on" : "off", c.nameTags ? 1 : 0);
    if (c.role == Role::Server)
        HSM_LOG("config: server transport=%s format=%s maps=%s score_limit=%d time_limit_s=%d countdown_s=%d min_players=%d", c.serverTransport.c_str(),
                c.serverFormat.c_str(), c.serverMaps.c_str(), c.serverScoreLimit, c.serverTimeLimitS, c.serverCountdownS, c.serverMinPlayers);
    return c;
}
}  // namespace hsm

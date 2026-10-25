#include "config.h"
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
    "standin_grip=lock\n";

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

const char* RoleName(Role r) { return r == Role::Steam ? "steam" : r == Role::Host ? "host" : r == Role::Client ? "client" : "off"; }

Config LoadConfig() {
    std::wstring ini = ModFilePath(L"HSMMod.ini");
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"w")) { std::fputs(kDefaultIni, f); std::fclose(f); }
    }
    Config c;
    std::string role = ReadString(ini, L"role", "steam");
    c.role = role == "host" ? Role::Host : role == "client" ? Role::Client : role == "off" ? Role::Off : Role::Steam;
    c.hostAddress = ReadString(ini, L"host", "127.0.0.1");
    c.port = uint16_t(Clamp(std::atoi(ReadString(ini, L"port", "27777").c_str()), 1024, 65535));
    c.playerName = ReadString(ini, L"name", "Player").substr(0, 32);
    c.snapshotHz = Clamp(std::atoi(ReadString(ini, L"snapshot_hz", "30").c_str()), 5, 120);
    c.inputHz = Clamp(std::atoi(ReadString(ini, L"input_hz", "60").c_str()), 10, 240);
    c.ownerSync = ReadString(ini, L"sync_mode", "owner") != "host";
    const std::string chatKey = ReadString(ini, L"chat_key", "enter");
    if (!ParseChatKey(chatKey, c.chatKey)) HSM_LOG("config: chat_key=%s is not enter, t, y or u; using enter", chatKey.c_str());
    c.standInGripLock = ReadString(ini, L"standin_grip", "lock") != "free";
    // Command-line overrides (several instances share one ini, e.g. a local host + client test).
    std::string cl = CommandLineUtf8();
    if (auto r = CmdArg(cl, "hsmrole")) c.role = *r == "host" ? Role::Host : *r == "client" ? Role::Client : *r == "off" ? Role::Off : Role::Steam;
    if (auto h = CmdArg(cl, "hsmhost"); h && !h->empty()) c.hostAddress = *h;
    c.port = uint16_t(CmdArgInt(cl, "hsmport", c.port, 1024, 65535));
    if (auto n = CmdArg(cl, "hsmname"); n && !n->empty()) c.playerName = n->substr(0, 32);
    if (auto m = CmdArg(cl, "hsmsync"); m && !m->empty()) c.ownerSync = *m != "host";
    if (auto k = CmdArg(cl, "hsmchatkey"); k && !ParseChatKey(*k, c.chatKey))
        HSM_LOG("config: -hsmchatkey=%s is not enter, t, y or u; ignored", k->c_str());
    HSM_LOG("config: role=%s host=%s port=%u snapshot_hz=%d input_hz=%d sync_mode=%s chat_key=%s standin_grip=%s", RoleName(c.role), c.hostAddress.c_str(),
            c.port, c.snapshotHz, c.inputHz, c.ownerSync ? "owner" : "host", ChatKeyName(c.chatKey), c.standInGripLock ? "lock" : "free");
    return c;
}
}  // namespace hsm

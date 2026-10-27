#include "config.h"
#include "session/hsm_cmdline.h"
#include "session/hsm_roster.h"
#include "util/hsm_ini.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

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
    "; in-game bug reports: type /bug <what went wrong> in the chat to file a GitHub issue with your HSMMod log\n"
    "; (Steam IDs, IP addresses and your Windows user name are removed first). Nothing is sent unless you type it.\n"
    "reports=on\n"
    "report_url=https://reports.dudiebug.net\n"
    "; 1: also send your HSMMod log (Steam IDs, IPs and your user name removed) when someone in your lobby files a bug report\n"
    "; or a crash is reported. The MULTIPLAYER page has the same switch. 0: off (default)\n"
    "help_improve=0\n"
    "; proximity voice chat: voice=1 on, 0 off (the default; the host's 0 turns it off for everyone). voice_mode=level sends while you\n"
    "; speak above voice_level_db (-60 picks up whispers, -30 only a firm voice); voice_mode=ptt sends while voice_key is\n"
    "; held (a letter, a digit, f1..f12, mouse4 or mouse5). Full volume within voice_near_m, silent past voice_far_m.\n"
    "; A fighter below voice_cutoff percent of full consciousness, knocked out or dead is cut off; the dead hear the dead.\n"
    "voice=0\n"
    "voice_mode=level\n"
    "voice_level_db=-40\n"
    "voice_key=v\n"
    "voice_near_m=5\n"
    "voice_far_m=30\n"
    "voice_cutoff=30\n"
    "voice_volume=100\n"
    "; role=server (a hosted retail instance with no fighter of its own that starts matches by itself):\n"
    "; server_transport=direct|steam (steam is experimental), server_format=ffa|duel|teams|survival|zombies,\n"
    "; server_maps=all or a comma list of map names, server_score_limit=0 (format default), server_time_limit_s=0 (none),\n"
    "; server_countdown_s=30, server_min_players=2 (1..8)\n"
    "; server_admins=Steam IDs (comma list) allowed to /kick, /ban and /unban from the chat; server_votekick=1 lets players\n"
    "; vote a player out (15 min ban). The console takes kick, ban, unban, bans, players and say (HSMMod.cmd)\n"
    "server_transport=direct\n"
    "server_format=ffa\n"
    "server_maps=all\n"
    "server_score_limit=0\n"
    "server_time_limit_s=0\n"
    "server_countdown_s=30\n"
    "server_min_players=2\n"
    "server_admins=\n"
    "server_votekick=1\n";

// voice_key: a letter, a digit, f1..f12, mouse4 or mouse5 (case-insensitive). 0: not one of those.
int ParseVoiceKey(std::string k) {
    for (char& ch : k) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    if (k.size() == 1 && ((k[0] >= 'a' && k[0] <= 'z') || (k[0] >= '0' && k[0] <= '9'))) return std::toupper(static_cast<unsigned char>(k[0]));
    if (k == "mouse4") return 0x05;   // VK_XBUTTON1
    if (k == "mouse5") return 0x06;   // VK_XBUTTON2
    if (k.size() >= 2 && k.size() <= 3 && k[0] == 'f') {
        const int n = std::atoi(k.c_str() + 1);
        if (n >= 1 && n <= 12 && std::to_string(n) == k.substr(1)) return 0x70 + n - 1;   // VK_F1..VK_F12
    }
    return 0;
}

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

// Reads the whole ini, sets the key in the text, writes a sibling file and moves it over the ini (a crash in between
// leaves the old file whole). A UTF-16 ini (a Notepad "Unicode" save) is left alone: GetPrivateProfileString reads it,
// this rewrite would not.
bool SaveHelpImprove(bool on) {
    const std::wstring ini = ModFilePath(L"HSMMod.ini"), tmp = ini + L".tmp";
    std::string text;
    if (FILE* f = _wfopen(ini.c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
    }
    if (text.size() >= 2 && (text.compare(0, 2, "\xFF\xFE") == 0 || text.compare(0, 2, "\xFE\xFF") == 0)) return false;
    const std::string next = util::SetIniValue(text, "net", "help_improve", on ? "1" : "0");
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool wrote = std::fwrite(next.data(), 1, next.size(), f) == next.size();
    if (std::fclose(f) != 0 || !wrote || !MoveFileExW(tmp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
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
    c.mesh = ReadString(ini, L"mesh", "0") == "1";
    c.peerPing = ReadString(ini, L"peer_ping", "1") != "0";
    c.hostMigration = ReadString(ini, L"host_migration", "1") != "0";
    c.voice = ReadString(ini, L"voice", "0") != "0";
    {
        const std::string mode = ReadString(ini, L"voice_mode", "level");
        if (mode == "off") c.voice = false;
        else if (mode == "ptt") c.voiceMode = 1;
        else if (mode != "level") HSM_LOG("config: voice_mode=%s is not level, ptt or off; using level", mode.c_str());
    }
    c.voiceLevelDb = float(Clamp(std::atoi(ReadString(ini, L"voice_level_db", "-40").c_str()), -80, -10));
    {
        const std::string key = ReadString(ini, L"voice_key", "v");
        if (const int vk = ParseVoiceKey(key)) c.voiceKey = vk;
        else HSM_LOG("config: voice_key=%s is not a letter, a digit, f1..f12, mouse4 or mouse5; using v", key.c_str());
    }
    c.voiceNearM = float(Clamp(std::atoi(ReadString(ini, L"voice_near_m", "5").c_str()), 1, 100));
    c.voiceFarM = float(Clamp(std::atoi(ReadString(ini, L"voice_far_m", "30").c_str()), int(c.voiceNearM) + 1, 200));
    c.voiceCutoff = float(Clamp(std::atoi(ReadString(ini, L"voice_cutoff", "30").c_str()), 0, 95)) / 100.f;
    c.voiceVolume = float(Clamp(std::atoi(ReadString(ini, L"voice_volume", "100").c_str()), 0, 200)) / 100.f;
    const std::string chatKey = ReadString(ini, L"chat_key", "enter");
    if (!ParseChatKey(chatKey, c.chatKey)) HSM_LOG("config: chat_key=%s is not enter, t, y or u; using enter", chatKey.c_str());
    c.standInGripLock = ReadString(ini, L"standin_grip", "lock") != "free";
    c.allowBuildMismatch = ReadString(ini, L"allow_build_mismatch", "0") == "1";
    c.nameTags = ReadString(ini, L"name_tags", "1") != "0";
    c.reports = ReadString(ini, L"reports", "on") != "off";
    c.reportUrl = ReadString(ini, L"report_url", "https://reports.dudiebug.net");
    if (c.reportUrl == "https://reports.dutybug.net") c.reportUrl = "https://reports.dudiebug.net";   // a misspelt default written by 0.7.0-pre1/pre2
    c.helpImprove = ReadString(ini, L"help_improve", "0") == "1";
    c.updateCheck = ReadString(ini, L"update_check", "1") != "0";
    c.serverTransport = ReadString(ini, L"server_transport", "direct") == "steam" ? "steam" : "direct";
    c.serverFormat = ReadString(ini, L"server_format", "ffa");
    c.serverMaps = ReadString(ini, L"server_maps", "all");
    c.serverScoreLimit = Clamp(std::atoi(ReadString(ini, L"server_score_limit", "0").c_str()), 0, 100);
    c.serverTimeLimitS = Clamp(std::atoi(ReadString(ini, L"server_time_limit_s", "0").c_str()), 0, 3600);
    c.serverCountdownS = Clamp(std::atoi(ReadString(ini, L"server_countdown_s", "30").c_str()), 0, 600);
    c.serverMinPlayers = Clamp(std::atoi(ReadString(ini, L"server_min_players", "2").c_str()), 1, kMaxPlayers);
    c.serverAdmins = ReadString(ini, L"server_admins", "");
    c.serverVoteKick = ReadString(ini, L"server_votekick", "1") != "0";
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
    HSM_LOG("config: reports=%s name_tags=%d help_improve=%d update_check=%d mesh=%d peer_ping=%d host_migration=%d", c.reports ? "on" : "off", c.nameTags ? 1 : 0,
            c.helpImprove ? 1 : 0, c.updateCheck ? 1 : 0, c.mesh ? 1 : 0, c.peerPing ? 1 : 0, c.hostMigration ? 1 : 0);
    HSM_LOG("config: voice=%d mode=%s level_db=%.0f key=0x%02x near_m=%.0f far_m=%.0f cutoff=%.0f%% volume=%.0f%%", c.voice ? 1 : 0,
            c.voiceMode ? "ptt" : "level", c.voiceLevelDb, c.voiceKey, c.voiceNearM, c.voiceFarM, c.voiceCutoff * 100.f, c.voiceVolume * 100.f);
    if (c.role == Role::Server)
        HSM_LOG("config: server transport=%s format=%s maps=%s score_limit=%d time_limit_s=%d countdown_s=%d min_players=%d", c.serverTransport.c_str(),
                c.serverFormat.c_str(), c.serverMaps.c_str(), c.serverScoreLimit, c.serverTimeLimitS, c.serverCountdownS, c.serverMinPlayers);
    if (c.role == Role::Server)
        HSM_LOG("config: server votekick=%d admins=%s", c.serverVoteKick ? 1 : 0, c.serverAdmins.empty() ? "none (console only)" : "set");
    return c;
}
}  // namespace hsm

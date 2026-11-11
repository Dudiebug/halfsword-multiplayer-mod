#include "config.h"
#include "match/hsm_class.h"
#include "match/hsm_hud_style.h"
#include "match/hsm_match.h"
#include "match/hsm_roles.h"
#include "session/hsm_cmdline.h"
#include "session/hsm_roster.h"
#include "session/hsm_ue4ss.h"
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
    "; HalfSwordMP configuration. role = steam | server | off\n"
    "; steam: use the Multiplayer button on the main menu (Steam lobbies, invites, no port forwarding). off: completely vanilla.\n"
    "[net]\n"
    "role=steam\n"
    "name=Player\n"
    "snapshot_hz=30\n"
    "input_hz=60\n"
    "; 1: skip unused input messages in owner mode with hand-off off; 0: always send (comparison/rollback)\n"
    "input_on_demand=1\n"
    "; the key that opens the chat: enter, t, y or u (Enter sends, Esc cancels)\n"
    "chat_key=enter\n"
    "; lock: the other players' hands hold their weapons firmly (default); free: each weapon is steered on its own\n"
    "standin_grip=lock\n"
    "; 1: align other players' two-handed weapon and arm targets; 0: earlier playback (comparison/rollback)\n"
    "standin_two_hand_pose=1\n"
    "; 1: under that playback both hands hold the shaft firmly; 0: the off hand slides and turns on it as before (comparison)\n"
    "standin_two_hand_lock=1\n"
    "; 1: the host starts a match even when another player runs a different mod build (testing only)\n"
    "allow_build_mismatch=0\n"
    "; 1: every player is built like a fighter of the lobby's tier (same body mass and grip strength), whatever their own\n"
    "; character; 0: each keeps their own build. The host's setting rules the match.\n"
    "force_strength=1\n"
    "; 1: show the other fighters' names over their heads in a match; 0: off\n"
    "name_tags=1\n"
    "; 1: in a Classes lobby, each name tag has a second line with the fighter's class, Strength, Weight and Bones; 0: off\n"
    "class_tags=1\n"
    "; 1: in a Classes lobby, each fighter's muscles show their class's Strength, on every screen; 0: the game's own look\n"
    "class_look=1\n"
    "; 1: a small notice in the top-left corner of a match names each player, their build and their UE4SS mods; 0: off\n"
    "build_cards=1\n"
    "; 1: a short sound from the game's menus when a player joins your lobby or session; 0: off\n"
    "join_sound=1\n"
    "; 1: leave custom maps named as a test (Test_Arena, MapTest, test01) out of the map picker; 0: list them too\n"
    "hide_test_maps=1\n"
    "; 1: other players see your Half Wolverine claws and you see theirs, if you both have LoganClaws_P.pak; 0: off\n"
    "claw_mirror=1\n"
    "; your class, picked in the multiplayer menu. It applies when you fight alone and in Classes lobbies.\n"
    "class=Fair\n"
    "; 1: a class that uses Half Wolverine writes its parts into that mod's settings when the game starts, and yours come back when you pick another class; 0: leave its file alone\n"
    "class_wolverine=1\n"
    "; 1: when you pick a class, Half Wolverine is refreshed in place (UE4SS reloads its mods); 0: restart the game instead\n"
    "wolverine_refresh=1\n"
    "; the class checker key: shows other players' classes, holds them on screen, steps through them and sends them away. A letter or F1 to F12.\n"
    "build_card_key=K\n"
    "; 0 turns off the armour paint crash guard (armour is destroyed at once, as in the stock game)\n"
    "paint_defer=1\n"
    "; the Juggernaut's body size (1.0 = the biggest stock fighter, up to 1.6). Every player needs the same value.\n"
    "juggernaut_scale=1.35\n"
    "; competitive netcode (on by default). competitive=1 sets every key below to its competitive value; a key you\n"
    ";   uncomment still wins. competitive=0 goes back to the netcode before protocol rev 42.\n"
    "competitive=1\n"
    "; 1: share your address with lobby members so a direct path can beat the Steam relay (players who are not friends)\n"
    "direct_p2p=1\n"
    "; 1: send your fighter's pose right after the physics step, about a frame sooner\n"
    ";late_send=1\n"
    "; 1: every blow also goes once unreliably beside the reliable copy (a lost packet costs nothing)\n"
    ";combat_copies=1\n"
    "; warn in the chat when your frame rate stays under this (0: never)\n"
    ";min_fps=60\n"
    "; the other fighters' playout buffer: the share of late poses it waits for (0.5-0.99) and its margin in ms (0-50)\n"
    ";playout_quantile=0.85\n"
    ";playout_margin_ms=4\n"
    "; show the other fighters this many ms nearer the present, extrapolated (0-120)\n"
    ";standin_lead_ms=0\n"
    "; host: a fixed input delay for every player's buttons and movement, so inputs arrive in time: 0, auto or 1-4 frames\n"
    ";input_delay=auto\n"
    "; check each blow against where your fighter really was: off, log (count only) or on (drop a blow that missed)\n"
    "claim_check=log\n"
    "; 1: a blade clash on your screen knocks the other player's real weapon too\n"
    ";deflect=1\n"
    "; in-game bug reports: type /bug <what went wrong> in the chat to file a GitHub issue with your HSMMod log\n"
    "; (Steam IDs, IP addresses and your Windows user name are removed first). Nothing is sent unless you type it.\n"
    "reports=on\n"
    "report_url=https://reports.dudiebug.net\n"
    "; 1: also send your HSMMod log (Steam IDs, IPs and your Windows user name removed; player names stay) when someone in\n"
    "; your lobby files a bug report or your game crashed last time. The MULTIPLAYER page and the Esc menu in a match have\n"
    "; the same switch. 0: off (default). help_improve_asked=1: the one-time question on the multiplayer page was answered.\n"
    "help_improve=0\n"
    "help_improve_asked=0\n"
    "; the look of the mod's menus and HUD: game (the game's own art and typefaces, the default), dark (that art shaded\n"
    "; dark, light text) or classic (the earlier dark look). The SETTINGS page on the multiplayer page switches it; it is\n"
    "; used from the next start of the game.\n"
    "ui_style=game\n"
    "; the opacity of the match HUD, the chat and the fighter card: 0.3 (faint) to 1 (solid, the default).\n"
    "hud_opacity=1\n"
    "; voice chat (the multiplayer page and the Esc menu in a match change these). voice=1 turns it on for you (0, the\n"
    "; default: you neither talk nor hear); voice_reach=proximity fades voices with distance (full volume within\n"
    "; voice_near_m, silent past voice_far_m), everyone lets every living player hear every living one; between two\n"
    "; players the narrower setting holds. voice_cut=0 keeps you talking when knocked out. Your microphone:\n"
    "; voice_mode=ptt sends while voice_key is held (a letter, a digit, f1..f12, mouse4 or mouse5), level sends while you\n"
    "; speak above voice_level_db (-60 picks up whispers, -30 only a firm voice), off never sends. voice_device: a\n"
    "; recording device's name (empty: the Windows default). A fighter below voice_cutoff percent of full consciousness,\n"
    "; knocked out or dead is cut off; the dead hear the dead. votekick=1 lets players vote a player out of your session.\n"
    "voice=0\n"
    "voice_reach=proximity\n"
    "voice_cut=1\n"
    "voice_mode=ptt\n"
    "voice_device=\n"
    "voice_level_db=-40\n"
    "voice_key=v\n"
    "voice_near_m=5\n"
    "voice_far_m=30\n"
    "voice_cutoff=30\n"
    "voice_volume=100\n"
    "; role=server (a hosted retail instance with no fighter of its own that starts matches by itself):\n"
    "; the server opens a public Steam lobby that shows in the game's lobby browser. server_format=ffa|duel|lms|teams|survival|zombies (teams3 / teams4: three or four teams; duel runs as lms unless exactly\n"
    "; 2 players are in), server_teams=2..4 (teams for server_format=teams),\n"
    "; server_maps=all or a comma list of map names, server_score_limit=0 (format default), server_time_limit_s=0 (none),\n"
    "; server_countdown_s=30, server_min_players=2 (1..8), server_damage=stock or a percent (player damage for everyone,\n"
    "; 100 = the game's rate 1.0, inside the game's own damage slider)\n"
    "; server_admins=Steam IDs (comma list) allowed to /kick, /ban, /unban, /clear [all|weapons|armor|rocks] and /resetmap from the chat; server_votekick=1 lets players\n"
    "; vote a player out (15 min) or /voteban them (7 days, two thirds). The console takes kick, ban, unban, bans, players,\n"
    "; clear and say (HSMMod.cmd)\n"
    "server_format=ffa\n"
    "server_teams=2\n"
    "server_maps=all\n"
    "server_score_limit=0\n"
    "server_time_limit_s=0\n"
    "server_countdown_s=30\n"
    "server_min_players=2\n"
    "server_damage=stock\n"
    "server_admins=\n"
    "server_votekick=1\n"
    "votekick=1\n"
    "\n"
    "[mod]\n"
    "; ue4ss=1 (default): HSMMod loads UE4SS (ue4ss/UE4SS.dll next to the game exe, or UE4SS.dll beside it), and replaces any other build with its own, asking first about one it does not know; 0: neither. ue4ss_keep: the hash of the build you said to keep.\n"
    "; ue4ss_order=after (default): load it once HSMMod's hooks are in; before: load it first. UE4SS mods are never synced.\n"
    "; ue4ss_console=0 (default): HSMMod turns UE4SS's console and cheat manager mods off in mods.txt at every start (a Vanilla lobby refuses them); 1: leaves them as you set them.\n"
    "ue4ss=1\n"
    "ue4ss_order=after\n"
    "; record=1: write a 60 Hz recording of every fighter, prop and gameplay event in each fight level to HSMRec-<date>-<time>.hsmrec\n"
    "; next to HSMMod.log (tools/hsmrec.py reads it; for mod development, about 40 MB a minute). Off by default.\n"
    "record=0\n"
    "; pack=<name>: the modpack (Content/Paks/~mods/<name>/ with its HSMPack.ini) your lobbies announce; every player needs the same files.\n"
    "; Empty: the only valid pack installed. off: none. See docs/MODPACK.md. Set mod_rule=bundle with it: vanilla refuses every pak.\n"
    "pack=\n"
    "; Mod rules for the lobbies you host: mod_rule=vanilla (no mods; UE4SS alone is fine; the default), maps (map\n"
    "; paks only), bundle (exactly the pack= above), custom (mod_allow=, or what you run), anything, classes (balanced HSE and Half Wolverine classes).\n"
    "; block_hse=1 refuses players running HS Enhancer (0: allow). mod_official=1 takes official HSMMod releases only.\n"
    "; mod_allow=A, B*1a2b3c4d, +Needed: the custom list (*: the first 8 hex of its SHA-256, +: required).\n"
    "; mod_rules=0 turns all of it off (and the HSMMod build check); mod_challenge=0 stops the host's code checks.\n"
    "; mod_rule=unrestricted: Unrestricted Classes, with no class budget (the MODS tab's UNRESTRICTED checkbox sets this).\n"
    "mod_rules=1\n"
    "mod_challenge=1\n"
    #ifdef HSM_TESTBENCH
    "mod_rule=classes\n"
#else
    "mod_rule=vanilla\n"
#endif

    "block_hse=1\n"
    "mod_official=0\n"
    "mod_allow=\n";

std::string ReadString(const std::wstring& ini, const wchar_t* key, const char* def) {
    wchar_t buf[256];
    wchar_t wdef[256];
    MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 256);
    GetPrivateProfileStringW(L"net", key, wdef, buf, 256, ini.c_str());
    char out[3 * 256 + 1];                                   // every UTF-16 unit fits in at most 3 UTF-8 bytes
    if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr)) return def;
    return out;
}
std::string ReadModString(const std::wstring& ini, const wchar_t* key, const char* def) {
    wchar_t buf[64], wdef[64];
    MultiByteToWideChar(CP_UTF8, 0, def, -1, wdef, 64);
    GetPrivateProfileStringW(L"mod", key, wdef, buf, 64, ini.c_str());
    char out[3 * 64 + 1];
    if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr)) return def;
    return out;
}
// A long [mod] value (mod_allow= lists up to kMaxModAllow names).
std::string ReadModLong(const std::wstring& ini, const wchar_t* key) {
    std::wstring buf(4096, L' ');
    const DWORD n = GetPrivateProfileStringW(L"mod", key, L"", buf.data(), DWORD(buf.size()), ini.c_str());
    buf.resize(n);
    if (buf.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), int(buf.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(len > 0 ? len : 0), ' ');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, buf.c_str(), int(buf.size()), out.data(), len, nullptr, nullptr);
    return out;
}
int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
double ClampD(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
}  // namespace

const char* RoleName(Role r) {
    return r == Role::Steam ? "steam" : r == Role::Server ? "server" : "off";
}
// Direct IP (role=host|client, -hsmrole=host|client) was removed in 0.8.0: logged once, then Steam is used.
static Role ParseRole(const std::string& r, const char* source) {
    if (r == "host" || r == "client") HSM_LOG("config: %s=%s is ignored: direct IP was removed in 0.8.0; using role=steam", source, r.c_str());
    return r == "off" ? Role::Off : r == "server" ? Role::Server : Role::Steam;
}

// Reads the whole ini, sets the key in the text, writes a sibling file and moves it over the ini (a crash in between
// leaves the old file whole). A UTF-16 ini (a Notepad "Unicode" save) is left alone: GetPrivateProfileString reads it,
// this rewrite would not.
bool SaveIniValues(const std::vector<std::pair<std::string, std::string>>& keys, const char* section) {
    const std::wstring ini = ModFilePath(L"HSMMod.ini"), tmp = ini + L".tmp";
    std::string text;
    if (FILE* f = _wfopen(ini.c_str(), L"rb")) {
        char buf[4096];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
        std::fclose(f);
    }
    if (text.size() >= 2 && (text.compare(0, 2, "\xFF\xFE") == 0 || text.compare(0, 2, "\xFE\xFF") == 0)) return false;
    std::string next = text;
    for (const auto& [key, value] : keys) next = util::SetIniValue(next, section, key, value);
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool wrote = std::fwrite(next.data(), 1, next.size(), f) == next.size();
    if (std::fclose(f) != 0 || !wrote || !MoveFileExW(tmp.c_str(), ini.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

bool SaveHelpImprove(bool on) { return SaveIniValues({{"help_improve", on ? "1" : "0"}, {"help_improve_asked", "1"}}); }
const char* UiStyleName(style::MenuLook look) {
    return look == style::MenuLook::Classic ? "classic" : look == style::MenuLook::Game ? "game" : "dark";
}
bool SaveUiStyle(style::MenuLook look) { return SaveIniValues({{"ui_style", UiStyleName(look)}}); }

void ReadModRuleKeys(Config& c) {
    const std::wstring ini = ModFilePath(L"HSMMod.ini");
    #ifdef HSM_TESTBENCH
    c.modRule = ReadModString(ini, L"mod_rule", "classes");
#else
    c.modRule = ReadModString(ini, L"mod_rule", "vanilla");
#endif
    // The one HS Enhancer switch. Older builds wrote block_hse under [net] (the classes checkbox) or mod_cheats under [mod].
    std::string hse = ReadModString(ini, L"block_hse", "");
    if (hse.empty()) hse = ReadString(ini, L"block_hse", "");
    if (hse.empty()) hse = ReadModString(ini, L"mod_cheats", "") == "allow" ? "0" : "1";
    c.blockHse = hse != "0";
    c.modOfficialOnly = ReadModString(ini, L"mod_official", "0") == "1";
    c.modAllow = ReadModLong(ini, L"mod_allow");
}

Config LoadConfig() {
    std::wstring ini = ModFilePath(L"HSMMod.ini");
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (FILE* f = _wfopen(ini.c_str(), L"w")) { std::fputs(kDefaultIni, f); std::fclose(f); }
    }
    Config c;
    std::string role = ReadString(ini, L"role", "steam");
    c.role = ParseRole(role, "role");
    if (!ReadString(ini, L"server_transport", "").empty()) HSM_LOG("config: server_transport is ignored: direct IP was removed in 0.8.0, servers use a public Steam lobby");
    c.playerName = ReadString(ini, L"name", "Player").substr(0, 32);
    c.snapshotHz = Clamp(std::atoi(ReadString(ini, L"snapshot_hz", "30").c_str()), 5, 120);
    c.inputHz = Clamp(std::atoi(ReadString(ini, L"input_hz", "60").c_str()), 10, 240);
    c.inputOnDemand = ReadString(ini, L"input_on_demand", "1") != "0";
    {
        const std::string sm = ReadString(ini, L"sync_mode", "owner");
        c.hostPhysics = sm == "host";
        if (sm != "host" && sm != "owner") HSM_LOG("config: sync_mode=%s is not host or owner; owner", sm.c_str());
    }
    c.mesh = ReadString(ini, L"mesh", "1") != "0";
    c.handOff = ReadString(ini, L"hand_off", "0") != "0";
    c.grabDrag = ReadString(ini, L"grab_drag", "1") != "0";
    c.syncStructures = ParseStructureMode(ReadString(ini, L"sync_structures", "soft"));
    c.structureFollowBroken = ReadString(ini, L"structure_follow_broken", "1") != "0";
    c.structureShareHeld = ReadString(ini, L"structure_share_held_breaks", "1") != "0";
    c.structureFollowMoving = ReadString(ini, L"structure_follow_moving", "1") != "0";
    c.propCensusSnapshot = ReadString(ini, L"prop_census_snapshot", "1") != "0";
    c.structureRestLock = ReadString(ini, L"structure_rest_lock", "1") != "0";
    c.structureBridgesFollow = ReadString(ini, L"structure_bridges_follow", "1") != "0";
    c.structureGameBreak = ReadString(ini, L"structure_game_break", "1") != "0";
    c.hostLockOn = ReadString(ini, L"host_lock_on", "1") != "0";
    c.driveLean = ReadString(ini, L"drive_lean", "1") != "0";
    c.forceStrength = ReadString(ini, L"force_strength", "1") != "0";
    {
        const std::string hd = ReadString(ini, L"host_input_delay", "auto");
        c.hostInputDelay = ParseHostInputDelay(hd);
        if (!c.hostInputDelay.valid) HSM_LOG("config: host_input_delay=%s is not auto, 0 or a number of milliseconds; using auto", hd.c_str());
    }
    c.scabbardFix = ReadString(ini, L"scabbard_fix", "1") != "0";
    c.standInPartsOff = ReadString(ini, L"standin_parts_off", "0") == "1";
    c.standInBoxFix = ReadString(ini, L"standin_box_fix", "1") != "0";
    c.fenceFix = ReadString(ini, L"fence_fix", "1") != "0";
    c.stuckArmorGate = ReadString(ini, L"stuck_armor_gate", "1") != "0";
    c.detour = ReadString(ini, L"detour", "1") != "0";
    c.peerPing = ReadString(ini, L"peer_ping", "1") != "0";
    c.competitive = ReadString(ini, L"competitive", "1") != "0";
    const bool cp = c.competitive;   // the competitive preset only changes defaults: a key in the file still wins
    c.lateSend = ReadString(ini, L"late_send", cp ? "1" : "0") == "1";
    c.combatCopies = ReadString(ini, L"combat_copies", cp ? "1" : "0") == "1";
    c.minFps = std::atoi(ReadString(ini, L"min_fps", cp ? "60" : "0").c_str());
    if (c.minFps != 0) c.minFps = Clamp(c.minFps, 20, 240);
    c.directP2P = ReadString(ini, L"direct_p2p", "1") != "0";
    c.playoutQuantile = std::clamp(std::atof(ReadString(ini, L"playout_quantile", cp ? "0.85" : "0.95").c_str()), 0.5, 0.99);
    c.playoutMarginMs = std::clamp(std::atof(ReadString(ini, L"playout_margin_ms", cp ? "4" : "6").c_str()), 0.0, 50.0);
    c.standinLeadMs = std::clamp(std::atof(ReadString(ini, L"standin_lead_ms", "0").c_str()), 0.0, 120.0);
    {
        const std::string d = ReadString(ini, L"input_delay", cp ? "auto" : "0");
        c.inputDelay = ParseMatchInputDelay(d);
        if (!c.inputDelay.valid) { HSM_LOG("config: input_delay=%s is not 0, auto or 1-4; off", d.c_str()); c.inputDelay = MatchDelaySetting{}; }
        const std::string cc = ReadString(ini, L"claim_check", "log");
        if (!combat::ParseClaimCheckMode(cc, c.claimCheck)) HSM_LOG("config: claim_check=%s is not off, log or on; log", cc.c_str());
    }
    c.deflect = ReadString(ini, L"deflect", cp ? "1" : "0") == "1";
    c.hostMigration = ReadString(ini, L"host_migration", "1") != "0";
    c.mapReset = ReadString(ini, L"map_reset", "1") != "0";
    c.progressionVote = ReadString(ini, L"progression_vote", "1") != "0";
    c.progressionPayout = ReadString(ini, L"progression_payout", "1") != "0";
    c.progressionSave = ReadString(ini, L"progression_save", "1") != "0";
    c.progressionFightEnd = ReadString(ini, L"progression_fight_end", "1") != "0";
    c.progressionClaimG = ReadString(ini, L"progression_claim_g", "1") != "0";
    c.progressionRounds = ReadString(ini, L"progression_rounds", "1") != "0";
    c.voice = ReadString(ini, L"voice", "0") != "0";
    {
        const std::string reach = ReadString(ini, L"voice_reach", "proximity");
        c.voiceReach = voice::ReachFromIni(reach, voice::Reach::Proximity);
        if (c.voiceReach == voice::Reach::Off) c.voiceReach = voice::Reach::Proximity;   // off is voice=0
        if (reach != "proximity" && reach != "everyone") HSM_LOG("config: voice_reach=%s is not proximity or everyone; using proximity", reach.c_str());
        c.voiceCut = ReadString(ini, L"voice_cut", "1") != "0";
        const std::string mode = ReadString(ini, L"voice_mode", "ptt");
        if (!voice::MicFromIni(mode, c.voiceMic)) HSM_LOG("config: voice_mode=%s is not ptt, level or off; using ptt", mode.c_str());
        c.voiceDevice = ReadString(ini, L"voice_device", "");
    }
    c.voiceLevelDb = float(Clamp(std::atoi(ReadString(ini, L"voice_level_db", "-40").c_str()), voice::kLevelMinDb, voice::kLevelMaxDb));
    {
        const std::string key = ReadString(ini, L"voice_key", "v");
        if (const int vk = voice::ParseVoiceKey(key)) c.voiceKey = vk;
        else HSM_LOG("config: voice_key=%s is not a letter, a digit, f1..f12, mouse4 or mouse5; using v", key.c_str());
    }
    c.voiceNearM = float(Clamp(std::atoi(ReadString(ini, L"voice_near_m", "5").c_str()), 1, 100));
    c.voiceFarM = float(Clamp(std::atoi(ReadString(ini, L"voice_far_m", "30").c_str()), int(c.voiceNearM) + 1, 200));
    c.voiceCutoff = float(Clamp(std::atoi(ReadString(ini, L"voice_cutoff", "30").c_str()), 0, 95)) / 100.f;
    c.voiceVolume = float(Clamp(std::atoi(ReadString(ini, L"voice_volume", "100").c_str()), 0, 200)) / 100.f;
    const std::string chatKey = ReadString(ini, L"chat_key", "enter");
    if (!ParseChatKey(chatKey, c.chatKey)) HSM_LOG("config: chat_key=%s is not enter, t, y or u; using enter", chatKey.c_str());
    c.standInGripLock = ReadString(ini, L"standin_grip", "lock") != "free";
    c.standInTwoHandPose = ReadString(ini, L"standin_two_hand_pose", "1") != "0";
    c.standInTwoHandLock = ReadString(ini, L"standin_two_hand_lock", "1") != "0";
    c.allowBuildMismatch = ReadString(ini, L"allow_build_mismatch", "0") == "1";
    c.nameTags = ReadString(ini, L"name_tags", "1") != "0";
    c.classTags = ReadString(ini, L"class_tags", "1") != "0";
    c.classLook = ReadString(ini, L"class_look", "1") != "0";
    c.buildCards = ReadString(ini, L"build_cards", "1") != "0";
    c.joinSound = ReadString(ini, L"join_sound", "1") != "0";
    c.hideTestMaps = ReadString(ini, L"hide_test_maps", "1") != "0";
    c.clawMirror = ReadString(ini, L"claw_mirror", "1") != "0";
    c.className = ReadString(ini, L"class", "");
    c.classWolverine = ReadString(ini, L"class_wolverine", "1") != "0";
    c.wolverineRefresh = ReadString(ini, L"wolverine_refresh", "1") != "0";
    if (c.className == "Launcher") c.className = "Custom 4";   // what test kits before named classes called that slot
    for (int i = 0; i < kCustomClassSlots; ++i) {
        c.classCustom[i] = ReadString(ini, (L"class_custom" + std::to_wstring(i + 1)).c_str(), "");
        c.classCustomName[i] = ReadString(ini, (L"class_custom" + std::to_wstring(i + 1) + L"_name").c_str(), "");
    }
    {
        const std::string k = ReadString(ini, L"build_card_key", "K");
        if (k.size() == 1 && std::isalpha(static_cast<unsigned char>(k[0]))) c.buildCardKey = std::toupper(static_cast<unsigned char>(k[0]));
        else if (k.size() >= 2 && (k[0] == 'F' || k[0] == 'f') && std::atoi(k.c_str() + 1) >= 1 && std::atoi(k.c_str() + 1) <= 12) c.buildCardKey = 0x6F + std::atoi(k.c_str() + 1);
        else HSM_LOG("config: build_card_key=%s is not a letter or F1 to F12; using K", k.c_str());
    }
    c.paintDefer = ReadString(ini, L"paint_defer", "1") != "0";
    c.juggernautScale = ClampD(std::atof(ReadString(ini, L"juggernaut_scale", "1.35").c_str()), hsm::kJuggernautScaleMin, hsm::kJuggernautScaleMax);
    c.reports = ReadString(ini, L"reports", "on") != "off";
    c.reportUrl = ReadString(ini, L"report_url", "https://reports.dudiebug.net");
    if (c.reportUrl == "https://reports.dutybug.net") c.reportUrl = "https://reports.dudiebug.net";   // a misspelt default written by 0.7.0-pre1/pre2
    c.helpImprove = ReadString(ini, L"help_improve", "0") == "1";
    c.helpImproveAsked = c.helpImprove || ReadString(ini, L"help_improve_asked", "0") == "1";
    {
        const std::string look = ReadString(ini, L"ui_style", "game");
        c.uiLook = look == "classic" ? style::MenuLook::Classic : look == "dark" ? style::MenuLook::Dark : style::MenuLook::Game;
        if (look != "dark" && look != "game" && look != "classic") HSM_LOG("config: ui_style=%s is not game, dark or classic; using game", look.c_str());
        HSM_LOG("config: ui_style=%s", UiStyleName(c.uiLook));
    }
    c.hudOpacity = hsm::style::SaneHudOpacity(std::atof(ReadString(ini, L"hud_opacity", "1").c_str()));
    if (c.hudOpacity < 1.0) HSM_LOG("config: hud_opacity=%.2f", c.hudOpacity);
#ifdef HSM_TESTBENCH
    c.reports = false; c.helpImprove = false; c.helpImproveAsked = true;   // a test build sends nothing to the mod's report server
#endif   // already on: nothing to ask
    c.updateCheck = ReadString(ini, L"update_check", "1") != "0";
    c.serverFormat = ReadString(ini, L"server_format", "ffa");
    c.serverTeams = Clamp(std::atoi(ReadString(ini, L"server_teams", "2").c_str()), kMinTeams, kMaxTeams);
    c.serverMaps = ReadString(ini, L"server_maps", "all");
    c.serverScoreLimit = Clamp(std::atoi(ReadString(ini, L"server_score_limit", "0").c_str()), 0, 100);
    c.serverTimeLimitS = Clamp(std::atoi(ReadString(ini, L"server_time_limit_s", "0").c_str()), 0, 3600);
    c.serverCountdownS = Clamp(std::atoi(ReadString(ini, L"server_countdown_s", "30").c_str()), 0, 600);
    c.serverMinPlayers = Clamp(std::atoi(ReadString(ini, L"server_min_players", "2").c_str()), 1, kMaxPlayers);
    {
        const std::string d = ReadString(ini, L"server_damage", "stock");
        c.serverDamagePct = d.empty() || d == "stock" ? -1 : Clamp(std::atoi(d.c_str()), 0, 1000);
    }
    c.serverAdmins = ReadString(ini, L"server_admins", "");
    c.serverVoteKick = ReadString(ini, L"votekick", ReadString(ini, L"server_votekick", "1").c_str()) != "0";
    c.ue4ss = ReadModString(ini, L"ue4ss", "1") != "0";
    c.ue4ssKeep = ReadModString(ini, L"ue4ss_keep", "");
    c.ue4ssConsole = ReadModString(ini, L"ue4ss_console", "0") == "1";
    c.ue4ssBefore = ue4ss::ParseOrder(ReadModString(ini, L"ue4ss_order", "after")) == ue4ss::Order::Before;
    c.modPack = ReadModString(ini, L"pack", "");
    c.modRules = ReadModString(ini, L"mod_rules", "1") != "0";
    c.modChallenge = ReadModString(ini, L"mod_challenge", "1") != "0";
    ReadModRuleKeys(c);
    c.record = ReadModString(ini, L"record", "0") != "0";
    if (c.record) HSM_LOG("config: record=1: fight levels are recorded to HSMRec-*.hsmrec next to this log");
    // Command-line overrides (several instances may share one ini).
    std::string cl = CommandLineUtf8();
    if (CommandLineIgnoresHsmFlags(cl)) {   // M6: a Steam join launch (+connect_lobby) cannot steer the mod through -hsm* flags
        HSM_LOG("config: +connect_lobby on the command line, -hsm* flags ignored");
    } else {
        if (auto r = CmdArg(cl, "hsmrole")) c.role = ParseRole(*r, "-hsmrole");
        if (CmdArg(cl, "hsmhost") || CmdArg(cl, "hsmport")) HSM_LOG("config: -hsmhost and -hsmport are ignored: direct IP was removed in 0.8.0");
        if (auto n = CmdArg(cl, "hsmname"); n && !n->empty()) c.playerName = n->substr(0, 32);
        if (auto b = CmdArg(cl, "hsmbuild")) c.buildName = *b;   // cleaned when it is published
        if (auto m = CmdArg(cl, "hsmsync"); m && (*m == "host" || *m == "owner")) c.hostPhysics = *m == "host";
        if (auto k = CmdArg(cl, "hsmchatkey"); k && !ParseChatKey(*k, c.chatKey))
            HSM_LOG("config: -hsmchatkey=%s is not enter, t, y or u; ignored", k->c_str());
    }
    HSM_LOG("config: role=%s snapshot_hz=%d input_hz=%d input_on_demand=%d sync_mode=%s chat_key=%s standin_grip=%s standin_two_hand_pose=%d standin_two_hand_lock=%d allow_build_mismatch=%d", RoleName(c.role),
            c.snapshotHz, c.inputHz, c.inputOnDemand ? 1 : 0, c.hostPhysics ? "host" : "owner", ChatKeyName(c.chatKey), c.standInGripLock ? "lock" : "free",
            c.standInTwoHandPose ? 1 : 0, c.standInTwoHandLock ? 1 : 0, c.allowBuildMismatch ? 1 : 0);
    HSM_LOG("config: class_tags=%d class_look=%d", c.classTags ? 1 : 0, c.classLook ? 1 : 0);
    HSM_LOG("config: reports=%s name_tags=%d help_improve=%d update_check=%d mesh=%d detour=%d peer_ping=%d host_migration=%d map_reset=%d sync_structures=%s structure_follow_broken=%d structure_share_held_breaks=%d structure_follow_moving=%d prop_census_snapshot=%d structure_rest_lock=%d structure_bridges_follow=%d structure_game_break=%d", c.reports ? "on" : "off", c.nameTags ? 1 : 0,
            c.helpImprove ? 1 : 0, c.updateCheck ? 1 : 0, c.mesh ? 1 : 0, c.detour ? 1 : 0, c.peerPing ? 1 : 0, c.hostMigration ? 1 : 0, c.mapReset ? 1 : 0,
            StructureModeName(c.syncStructures), c.structureFollowBroken ? 1 : 0, c.structureShareHeld ? 1 : 0,
            c.structureFollowMoving ? 1 : 0, c.propCensusSnapshot ? 1 : 0, c.structureRestLock ? 1 : 0,
            c.structureBridgesFollow ? 1 : 0, c.structureGameBreak ? 1 : 0);
    HSM_LOG("config: drive_lean=%d host_input_delay=%s host_lock_on=%d scabbard_fix=%d force_strength=%d stuck_armor_gate=%d standin_parts_off=%d standin_box_fix=%d", c.driveLean ? 1 : 0,
            c.hostInputDelay.mode == HostDelayMode::Auto ? "auto" : c.hostInputDelay.mode == HostDelayMode::Off ? "0" : std::to_string(c.hostInputDelay.fixedMs).c_str(),
            c.hostLockOn ? 1 : 0, c.scabbardFix ? 1 : 0, c.forceStrength ? 1 : 0, c.stuckArmorGate ? 1 : 0, c.standInPartsOff ? 1 : 0, c.standInBoxFix ? 1 : 0);
    HSM_LOG("config: netcode competitive=%d late_send=%d combat_copies=%d min_fps=%d", c.competitive ? 1 : 0, c.lateSend ? 1 : 0,
            c.combatCopies ? 1 : 0, c.minFps);
    HSM_LOG("config: netcode direct_p2p=%d playout_quantile=%.2f playout_margin_ms=%.0f standin_lead_ms=%.0f input_delay=%s claim_check=%s deflect=%d",
            c.directP2P ? 1 : 0, c.playoutQuantile, c.playoutMarginMs, c.standinLeadMs,
            c.inputDelay.mode == MatchDelayMode::Auto ? "auto" : std::to_string(c.inputDelay.frames).c_str(),
            combat::ClaimCheckModeName(c.claimCheck), c.deflect ? 1 : 0);
    HSM_LOG("config: voice=%d reach=%s cut=%d mode=%s device=%s level_db=%.0f key=0x%02x near_m=%.0f far_m=%.0f cutoff=%.0f%% volume=%.0f%% votekick=%d", c.voice ? 1 : 0,
            voice::ReachIni(c.voiceReach), c.voiceCut ? 1 : 0, voice::MicIni(c.voiceMic), c.voiceDevice.empty() ? "default" : "named", c.voiceLevelDb, c.voiceKey, c.voiceNearM, c.voiceFarM, c.voiceCutoff * 100.f, c.voiceVolume * 100.f, c.serverVoteKick ? 1 : 0);
    if (c.role == Role::Server)
        HSM_LOG("config: server format=%s teams=%d maps=%s score_limit=%d time_limit_s=%d countdown_s=%d min_players=%d damage=%d",
                c.serverFormat.c_str(), c.serverTeams, c.serverMaps.c_str(), c.serverScoreLimit, c.serverTimeLimitS, c.serverCountdownS, c.serverMinPlayers, c.serverDamagePct);
    if (c.role == Role::Server)
        HSM_LOG("config: server admins=%s", c.serverAdmins.empty() ? "none (console only)" : "set");
    return c;
}
}  // namespace hsm

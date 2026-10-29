// HSMMod.ini next to the game exe. Missing file or role=off keeps the game fully vanilla.
#pragma once
#include "hsm_chat.h"
#include "voice/hsm_voice_ui.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hsm {
// Steam: lobby-driven (Multiplayer button on the main menu; lobby owner hosts). Server: below.
enum class Role { Off, Steam, Server };   // Server: a hosted retail instance with no fighter of its own, auto-starting matches

struct Config {
    Role role = Role::Steam;
    std::string playerName = "Player";
    int snapshotHz = 30;
    int inputHz = 60;
    // true: clients also send Pose and Vitals straight to each other (Steam lobbies only; session/hsm_mesh.h), and the host
    // stops relaying what a client already hears directly (default from 0.7.0-pre6). false (mesh=0): every stream goes
    // through the host, as in 0.6.5. Used where the host and the client both have it on.
    bool mesh = true;
    // true (default from 0.7.0-pre6): with the mesh on, the host also plans each client pair's fastest path from the
    // measured pings and a client may pass another pair's Pose and Vitals on (detours, session/hsm_detour.h). detour=0
    // turns it off; nothing happens with mesh=0.
    bool detour = true;
    // true (default): on a Steam lobby every client pings the host and every other client every 2 s and reports the
    // results to the host every 10 s; both log them in the health window ("ping:" lines). Measures what a mesh would get
    // without turning it on. Host and clients both need it; mesh=1 is not needed.
    bool peerPing = true;
    // true (default): when a Steam lobby's host leaves, the member Steam makes the new lobby owner hosts the same lobby
    // and the others follow it (session/hsm_migration.h) instead of everyone going back to the menu.
    bool hostMigration = true;
    // true (default): the host resets the map at each Duel round and each rematch (props back to their start, objects spawned
    // in the round gone, the level's own weapons back on their spots); 0 sends no MapReset. Read on the host only.
    bool mapReset = true;
    // Voice chat (gameplay/voice.h). Each player's own setting (0.7.1): voice and voiceReach are off (the default), by
    // distance (proximity: full volume within voiceNearM, silent past voiceFarM) or every living player hears every
    // living one (everyone); two players hear each other at the narrower of their settings. A fighter below voiceCutoff
    // of full consciousness (or knocked out, or dead) is cut off unless its voiceCut is false; the dead hear only the
    // dead. Also each player's own: voiceMic (push to talk on voiceKey by default, an open microphone above voiceLevelDb, or off), voiceDevice (a
    // Windows recording device by name; empty: the Windows default, recorded by Steam) and voiceVolume. The multiplayer
    // page and the in-match menu change them and write them back (SaveIniValues).
    bool voice = false;
    voice::Reach voiceReach = voice::Reach::Proximity;   // when voice is on
    bool voiceCut = true;        // false: keep talking when knocked out or low on consciousness
    voice::Mic voiceMic = voice::Mic::PushToTalk;
    std::string voiceDevice;
    float voiceLevelDb = -40.f;
    int voiceKey = 'V';          // a virtual-key code
    float voiceNearM = 5.f, voiceFarM = 30.f;
    float voiceCutoff = 0.3f;
    float voiceVolume = 1.f;
    ChatKey chatKey = ChatKey::Enter;
    // true: a stand-in's hands lock their grip joints and carry the weapon (the game's own firm grip); false: the grips
    // are freed and each weapon is servoed on its own (pre7 behaviour). Each machine's own setting applies to its stand-ins.
    bool standInGripLock = true;   // opens the chat (enter, t, y or u); the game's Talk is very likely T
    // true: the host starts a match even while another player runs a different mod build (testing only).
    bool allowBuildMismatch = false;
    // In-game bug reports ("/bug" in the chat; net/report_client.h). Nothing is sent without the player's command.
    // true: other fighters' names float over their heads in a match (name_tags=0 turns them off).
    bool nameTags = true;
    // true: armour swapped out while paint tasks still read it is destroyed later (the T5 crash); paint_defer=0 turns it off.
    bool paintDefer = true;
    bool reports = true;
    // true: your HSMMod log is also sent to the report relay when someone in your lobby files a report or a crash is reported
    // (the multiplayer page's HELP IMPROVE THE MOD toggle; SaveHelpImprove writes the key). Off by default.
    bool helpImprove = false;
    std::string reportUrl = "https://reports.dudiebug.net";
    // One request to api.github.com at startup, asking whether a newer HSMMod release exists (net/update_check.h).
    bool updateCheck = true;
    // role=server only (see session/hsm_autoflow.h): a public Steam lobby.
    std::string serverFormat = "ffa";          // ffa | duel | teams | survival | zombies
    std::string serverMaps = "all";            // comma list of map display names, or all
    int serverScoreLimit = 0;                  // 0: the format's default
    int serverTimeLimitS = 0;                  // 0: none
    int serverDamagePct = -1;                  // player damage for everyone, percent of the rate; -1: the stock rate
    int serverCountdownS = 30;                 // lobby countdown once serverMinPlayers have joined
    int serverMinPlayers = 2;
    // Moderation (session/hsm_moderation.h): on role=server, Steam IDs that may /kick, /ban and /unban from the chat (the
    // console, HSMMod.cmd, always may). serverVoteKick (votekick= or server_votekick=): whether players can vote a
    // player out; since 0.7.0-pre6 in every hosted session, from the chat or the in-match menu.
    std::string serverAdmins;
    bool serverVoteKick = true;
    // [mod] section (ue4ss_load.h): HSMMod's dwmapi.dll chain-loads UE4SS. ue4ss=0 skips it; ue4ss_order=before loads it
    // before HSMMod's ProcessEvent hook is installed, after (default) once the hook is in.
    bool ue4ss = true;
    bool ue4ssBefore = false;
};

Config LoadConfig();          // writes a commented default file if none exists
const char* RoleName(Role r);
bool SaveHelpImprove(bool on);   // writes help_improve= under [net] in HSMMod.ini, the rest of the file untouched; false: could not
// Writes these keys under [net] in HSMMod.ini in one replace, the rest of the file untouched; false: could not.
bool SaveIniValues(const std::vector<std::pair<std::string, std::string>>& keys);
}  // namespace hsm

// HSMMod.ini next to the game exe. Missing file or role=off keeps the game fully vanilla.
#pragma once
#include "hsm_chat.h"
#include <cstdint>
#include <string>

namespace hsm {
// Steam: lobby-driven (Multiplayer button on the main menu; lobby owner hosts). Host/Client: direct IP via ENet.
enum class Role { Off, Steam, Host, Client, Server };   // Server: a hosted retail instance with no fighter of its own, auto-starting matches

struct Config {
    Role role = Role::Steam;
    std::string hostAddress = "127.0.0.1";
    uint16_t port = 27777;
    std::string playerName = "Player";
    int snapshotHz = 30;
    int inputHz = 60;
    // Host only (clients take the host's choice from Welcome). true: every machine simulates its own fighter and the
    // others are stand-ins (protocol rev 9 owner authority); false: the host simulates every fighter from inputs.
    bool ownerSync = true;
    // true: clients also send Pose and Vitals straight to each other (Steam lobbies only; session/hsm_mesh.h), and the host
    // stops relaying what a client already hears directly. false (default): every stream goes through the host, as in 0.6.5.
    // Needs mesh=1 on the host and on the clients that should use it; ENet (direct IP) stays a star either way.
    bool mesh = false;
    // true (default): on a Steam lobby every client pings the host and every other client every 2 s and reports the
    // results to the host every 10 s; both log them in the health window ("ping:" lines). Measures what a mesh would get
    // without turning it on. Host and clients both need it; mesh=1 is not needed.
    bool peerPing = true;
    // true (default): when a Steam lobby's host leaves, the member Steam makes the new lobby owner hosts the same lobby
    // and the others follow it (session/hsm_migration.h) instead of everyone going back to the menu.
    bool hostMigration = true;
    // Proximity voice (gameplay/voice.h): Steam records the microphone; the host relays it to the players near the
    // speaker. A fighter below voiceCutoff of full consciousness (or knocked out, or dead) is cut off; the dead hear
    // only the dead. voiceMode 0: open microphone above voiceLevelDb (default); 1: push to talk on voiceKey.
    // Off by default (voice=1 turns it on); the host's voice=0 turns it off for the whole session.
    bool voice = false;
    int voiceMode = 0;
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
    bool reports = true;
    // true: your HSMMod log is also sent to the report relay when someone in your lobby files a report or a crash is reported
    // (the multiplayer page's HELP IMPROVE THE MOD toggle; SaveHelpImprove writes the key). Off by default.
    bool helpImprove = false;
    std::string reportUrl = "https://reports.dudiebug.net";
    // One request to api.github.com at startup, asking whether a newer HSMMod release exists (net/update_check.h).
    bool updateCheck = true;
    // role=server only (see session/hsm_autoflow.h). transport: "direct" (ENet on `port`) or "steam" (experimental).
    std::string serverTransport = "direct";
    std::string serverFormat = "ffa";          // ffa | duel | teams | survival | zombies
    std::string serverMaps = "all";            // comma list of map display names, or all
    int serverScoreLimit = 0;                  // 0: the format's default
    int serverTimeLimitS = 0;                  // 0: none
    int serverCountdownS = 30;                 // lobby countdown once serverMinPlayers have joined
    int serverMinPlayers = 2;
    // role=server moderation (session/hsm_moderation.h): Steam IDs that may /kick, /ban and /unban from the chat (the
    // console, HSMMod.cmd, always may), and whether players can start a /votekick.
    std::string serverAdmins;
    bool serverVoteKick = true;
};

Config LoadConfig();          // writes a commented default file if none exists
const char* RoleName(Role r);
bool SaveHelpImprove(bool on);   // writes help_improve= under [net] in HSMMod.ini, the rest of the file untouched; false: could not
}  // namespace hsm

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
    ChatKey chatKey = ChatKey::Enter;
    // true: a stand-in's hands lock their grip joints and carry the weapon (the game's own firm grip); false: the grips
    // are freed and each weapon is servoed on its own (pre7 behaviour). Each machine's own setting applies to its stand-ins.
    bool standInGripLock = true;   // opens the chat (enter, t, y or u); the game's Talk is very likely T
    // true: the host starts a match even while another player runs a different mod build (testing only).
    bool allowBuildMismatch = false;
    // In-game bug reports ("/report" in the chat; net/report_client.h). Nothing is sent without the player's command.
    // true: other fighters' names float over their heads in a match (name_tags=0 turns them off).
    bool nameTags = true;
    bool reports = true;
    std::string reportUrl = "https://reports.dutybug.net";
    // role=server only (see session/hsm_autoflow.h). transport: "direct" (ENet on `port`) or "steam" (experimental).
    std::string serverTransport = "direct";
    std::string serverFormat = "ffa";          // ffa | duel | teams | survival | zombies
    std::string serverMaps = "all";            // comma list of map display names, or all
    int serverScoreLimit = 0;                  // 0: the format's default
    int serverTimeLimitS = 0;                  // 0: none
    int serverCountdownS = 30;                 // lobby countdown once serverMinPlayers have joined
    int serverMinPlayers = 2;
};

Config LoadConfig();          // writes a commented default file if none exists
const char* RoleName(Role r);
}  // namespace hsm

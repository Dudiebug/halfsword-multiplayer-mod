// HSMMod.ini next to the game exe. Missing file or role=off keeps the game fully vanilla.
#pragma once
#include <cstdint>
#include <string>
#include "net/hsm_impairment.h"
#include "match/hsm_test_match.h"

namespace hsm {
// Steam: lobby-driven (Multiplayer button on the main menu; lobby owner hosts). Host/Client: direct IP via ENet.
enum class Role { Off, Steam, Host, Client };

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
    bool testForceScan = false; // host/client only: bypass the known-build table
    TestMatchConfig testMatch; // direct-IP test matches only
    ImpairmentSettings impairment;  // test-only, ENet outgoing payloads; all zero disables it
};

Config LoadConfig();          // writes a commented default file if none exists
const char* RoleName(Role r);
}  // namespace hsm

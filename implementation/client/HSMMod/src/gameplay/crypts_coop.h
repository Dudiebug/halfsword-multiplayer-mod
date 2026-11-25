// The host's side of the shared Crypts run (docs/PLAN_CRYPTS_COOP.md, PR B). The host runs the game's own dungeon; every other
// machine builds the rooms the host's director placed (crypts_layout.cpp, coop mode). This file is the host's half:
//   - it reads each new room from the director's "Active Levels" (class, Origin Point, parent, which entrance joins its parent,
//     flags) and hands it to the session, which numbers it and sends it to the clients (CryptRoom);
//   - it keeps the dungeon standing while remote players are in it: no room unloads (Event Unload Level, Destroy Level), the
//     SpawnLevel box's end overlap is blocked, the fall box does not destroy a mod fighter, and the director's Destroy Actors in
//     Area (which destroys every Willie but local players 0 and 1) does not run;
//   - a remote player is a mock body, not the game's player 0/1 that the rooms' gates compare with, so every ~100 ms it tests
//     each remote body against each live room's boxes and, the first time one is inside, calls that room's own event with the
//     host's pawn as OtherActor (SpawnLevel: the room's levels; Box: its child rooms; SpawnCharacters: its fighters, the
//     director's enemies, which the session adopts as bots: PR C);
//   - with crypts_host_spawns (the default) the SpawnCharacters box is the mod's: the first time any player, the host included, stands
//     in it, the host picks the room's spawner points with every player's position (session/hsm_crypt_spawn.h) and calls the
//     director's Spawn Character for each; the room's own event (which skips every point near or in sight of the host) is blocked;
//   - the director spawns only while its "Spawned Willies" holds 3 or 4 fighters, and the game empties that list by destroying
//     them (which is blocked here), so the mod takes a dead or removed enemy out of it (session/hsm_crypt_enemies.h).
// Lines start "crypt room:". Game thread only; inert unless OnLevel(true, ...) armed it (a host in the shared run).
#pragma once
#include "session/hsm_crypt_rooms.h"
#include "ue/engine.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm::game::cryptscoop {

struct Remote {
    uint8_t slot = 0;
    std::string name;
    ue::UObject* actor = nullptr;   // the body the host has for that player
};

struct Env {
    ue::UObject* gameInstance = nullptr;   // GI_Settings_C: names the dungeon director
    ue::UObject* hostPawn = nullptr;       // GetPlayerCharacter(0): the pawn the rooms' gates compare OtherActor with
    std::vector<Remote> remotes;
    // A new room was read (seq is 0): the session numbers it, sends it and returns its seq (0: not taken; the room is not offered again).
    std::function<uint16_t(const crypt::CryptRoomMsg&)> onRoom;
    std::function<bool(ue::UObject*)> isDead;   // a fighter that is dead (the director's list drops it)
    std::string hostName;                       // for the log
    // Nothing between `from` and `to`, looking through `ignore` (the players' bodies): the sight test of a spawner point.
    std::function<bool(const ue::FVector&, const ue::FVector&, const std::vector<ue::UObject*>&)> lineClear;
};

void InstallRule();                                                  // once, at start-up (ProcessEvent hooks; inert until armed)
void InstallScripts();                                               // owner sync start: the script-VM blocks (Event Unload Level, ...)
void RemoveScripts();                                                // the session ended: those blocks go
void SetModFighterTest(std::function<bool(const ue::UObject*)> f);   // stand-ins and bots: the bodies the fall box must spare
// [mod] crypts_host_spawns=1 (the default): the host fills a room with enemies for whichever player walks in (session/hsm_crypt_spawn.h)
// and the room's own SpawnCharacters event, which only knows the host's body, is blocked. =0: the 0.8.0 way (the room's event called
// with the host's pawn).
void SetHostSpawns(bool on);
void OnLevel(bool shared, uint32_t nowMs);                           // each level load: armed for a host in the shared Crypts
void Tick(uint32_t nowMs, const Env& env);                           // each session tick
bool Armed();
// Whether `p` is inside the entrance room's SpawnLevel or Box box (the game's Is Entrance? room), for spreading the clients over it.
// True when no entrance room is read yet or its boxes cannot be read: there is nothing to filter by.
bool InEntranceRoom(double x, double y, double z);
// The seq of the room a point is in: inside its SpawnLevel box, else its Box, else its SpawnCharacters box; failing those the room with
// the nearest box centre. 0 when the host has no room read yet (PR D: which room a placed thing belongs to).
uint16_t RoomSeqAt(double x, double y, double z);
int Live();                                                          // rooms alive in the director's list
// The enemy `willie` leaves the director's "Spawned Willies" (the mod is about to destroy it). Returns how many entries went.
int ForgetEnemy(ue::UObject* willie);
int SpawnedWillies();                                                // how many fighters the director's list holds now (-1: not readable)

// The class name of a room ("BP_Procedural_Block_Chamber_1_C") as its catalog index, or -1.
int CatalogOfClass(const std::string& className);

}  // namespace hsm::game::cryptscoop

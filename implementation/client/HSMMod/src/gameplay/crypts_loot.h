// Loot, doors and keys of the shared Crypts run (docs/PLAN_CRYPTS_COOP.md, PR D; the books and the wire are session/hsm_crypt_loot.h).
//   Host: a loot point (BP_Procedural_LootPoint_*, BP_Loot_*) rolls its dice in BeginPlay and spawns a container, altar, food item or
//     key. There is no hook after the call, so the product's own BeginPlay is watched: a product that begins play next to a loot
//     point that just did is that point's. Its class path and place go out as CryptLoot, numbered by the room it stands in (the
//     room's seq from crypts_coop) and its index in that room. A container's contents (rolled a frame after its BeginPlay) are matched to
//     the container the same way. A weapon, bottle, lantern or armour piece goes with its passport struct as a save file (PR D2:
//     Adapter::ExportItemState). The host also watches food, keys and gear: one that is gone or moved away from its place was taken,
//     and that goes out too.
//   Client: its streamed copies of the loot points roll no dice (BeginPlay blocked, the actor destroyed), nor do its containers and
//     altars (so they are empty here: the host's contents are spawned instead). The things the host sent are spawned once the room they
//     stand in is built (gear through Adapter::SpawnItemFromState). A thing taken here is reported to the host, which sends it on; a
//     taken item sent by the host is removed here, unless it is the one this player carries.
//   Doors: the gate (BP_Door_Gate_A_001), the tear-down barricade and the unbreakable barricade of the host's dungeon are numbered by
//     the host as it sees them (CryptDoor) and kept in step: state goes forward only (closed, open, gone). A client spawns the
//     barricades (its rooms never run their logic, so nothing else places them) as inert copies, and applies a gate unlocked by a key
//     or a barricade torn down. A client reports a gate it unlocked; the host checks and sends it on.
// Lines start "crypt coop:". Game thread only; inert unless the host is in a shared Crypts run or a client has the co-op loader armed.
#pragma once
#include "session/hsm_crypt_loot.h"
#include "ue/engine.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace hsm::game {
class Adapter;
namespace cryptsloot {

struct Env {
    bool host = false;
    Adapter* adapter = nullptr;
    std::vector<ue::UObject*> remotes;                     // host: the bodies of the remote players (arms a barricade they walk up to)
    std::function<uint16_t(double, double, double)> roomAt;   // host: the seq of the room a point is in (0: none yet)
    std::function<bool(uint16_t)> roomBuilt;               // client: the room's levels are shown
    std::function<bool()> allBuilt;                        // client: every room the host announced is shown
    std::function<void(const crypt::CryptLootMsg&)> sendLoot;   // host: to every client; client: to the host
    std::function<void(const crypt::CryptDoorMsg&)> sendDoor;
};

void InstallRule();                                        // once, at start-up (BeginPlay hook; inert unless a shared run is armed)
void Reset();                                              // a new run: the books and what they track are forgotten
void Tick(uint32_t nowMs, const Env& env);                 // each session tick
// A message from the wire. `fromClient`: a client's report (the host checks it and sends it on); else the host's.
void OnLoot(const crypt::CryptLootMsg& m, bool fromClient);
void OnDoor(const crypt::CryptDoorMsg& m, bool fromClient);
// Host: every loot record and door so far, for a joiner (each call of `send` is one message).
void ForEachRecord(const std::function<void(const std::vector<uint8_t>&)>& send);

}  // namespace cryptsloot
}  // namespace hsm::game

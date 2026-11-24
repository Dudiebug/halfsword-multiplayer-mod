// The loader for a table of rooms (docs/PLAN_CRYPTS_COOP.md, match/hsm_crypts_layout.h): the Crypts level opened with the
// game's dungeon switched off, and a fixed table of rooms streamed into it, the same on every machine. Lines start
// "crypts layout:". Game thread only. Nothing arms it yet: the lobby has no layout maps any more, and the co-op Crypts (Grave
// Robbers) will arm it with the rooms the host's director placed.
//
// The co-op Crypts (PR B, docs/PLAN_CRYPTS_COOP.md) uses the same rule and streaming, but not a table: a client in the host's shared
// run is armed with ArmCoop (FollowTravel), and CoopTick builds each room the host's director placed (CryptRoom, in seq order) by
// streaming its Module, Lighting and Logic levels at the host's Origin Point under "HSMCrypts_Coop_<seq>_<M|L|G>", clearing the
// gate where it joins its parent, and reporting each room built. No barricades in this PR.
//
// Arm(layout) is what a Travel would call; any other level (or the game's own OpenLevel) disarms it. While armed, a rule blocks
// what builds the dungeon:
//   - the level script's ReceiveBeginPlay (Map_Procedural_Block_Test_Main_C): it spawns the gen-0 room and, 2 s later,
//     calls its Event Force Load inside the script VM, where no ProcessEvent hook can stop it; the rule sets the Game Mode
//     6 the script would have set;
//   - BP_Procedural_Block_Master_C's (and its room children's) ReceiveBeginPlay and bound events;
//   - BP_Procedural_Dungeon_Director_C's ReceiveBeginPlay (it spawns and possesses the player's character and loads the
//     Ambient level) and its reinforcements timer.
// Once the level loads (OnLevel), the loader streams the Ambient level and every row's three levels under fixed instance
// names, clears the gates where two rooms join, bars every open entrance with the game's unbreakable barricade, and
// removes the level's StaticMeshActor_1 as the script does after 10 s.
#pragma once
#include "match/hsm_crypts_layout.h"
#include "session/hsm_crypt_rooms.h"
#include "ue/engine.h"
#include <cstdint>
#include <functional>
#include <string>

namespace hsm::game {
class Adapter;
namespace cryptslayout {

void InstallRule();                            // once, at start-up (blocks only while a layout is armed)
// [mod] crypts_layout_names= (dev): the form of the instance name LoadLevelInstance gets. auto (the default): found on the
// first level of each build, by reading back the package name the engine gave it; short ("HSMCrypts_..."), long (in the
// source level's folder), engine (no name: the engine's own, the rooms' shared dice fall back to the world's name).
void SetNameForm(const std::string& form);
void Arm(const crypts::Layout* layout);        // the next level load builds this layout (nullptr: none)
const crypts::Layout* Armed();
// A client in the host's shared Crypts: the next level load builds the host's rooms (Arm(nullptr), and any level change, clears it).
void ArmCoop(bool on);
bool CoopArmed();
// A level loaded (`context`: an actor of its persistent level). Starts the armed layout when the level is the Crypts
// level and returns it; otherwise the loader stays idle and returns nullptr.
const crypts::Layout* OnLevel(ue::UObject* context, Adapter& adapter, uint32_t nowMs);
void Tick(Adapter& adapter, uint32_t nowMs);   // each session tick
// Client, the co-op Crypts: starts the levels of every room in `rooms` not yet started (the contiguous prefix 1..Known()), clears
// the joints whose two rooms are shown, and calls onBuilt(seq) once for each room whose levels are all shown or failed and whose
// joint is done. Nothing when this level is not a co-op build.
void CoopTick(Adapter& adapter, uint32_t nowMs, const crypt::RoomList& rooms, const std::function<void(uint16_t)>& onBuilt);
bool CoopLive();                               // this level is a co-op build
bool Active();                                 // this level is a layout (or the co-op rooms) the loader builds
int Pending();                                 // its levels still loading (0 when idle; a failed load is not pending)
bool Loading();                                // levels still loading and not yet past the 45 s wait the fighters get
bool InLayout(const ue::UObject* level);       // a ULevel the loader streamed and saw shown
const crypts::Layout* Current();               // the layout this level builds, else nullptr
int RowOfLevel(const ue::UObject* level);      // the row a shown level is one of the three levels of; -1 for none or the Ambient level

}  // namespace cryptslayout
}  // namespace hsm::game

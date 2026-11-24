// Levels that are single-player content: a session never travels into them and never treats them as a match level.
// Build 26568321 added the procedural dungeon (Game Mode enum 6, "Roam"): the Seer's "Go to Hell" and the start menu's top
// button open Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main
// (UI_Dialog_Widget_Seer_GoToHell.cpp:54-59, UI_Startup_Menu.cpp:128-130). BP_Procedural_Dungeon_Director builds the dungeon
// from unseeded random draws at BeginPlay (BP_Procedural_Dungeon_Director.cpp:408), so two machines would walk two different
// dungeons, and BP_LevelManager does no match setup in modes 5 and 6 (BP_LevelManager.cpp:758), so there is no spawn for a
// level gate to wait for. Pure name test; the DLL decides what to do with the answer.
//
// The Crypts level is also the lobby's Crypts map now (match/hsm_maps.h, MapInfo::crypts), so the name alone cannot say whether a
// session may open it: that depends on the run. Crypt Royale Apart opens it as every machine's own run (`gatherRun`, SoloLevelAllowed);
// a shared run (Grave Robbers, Crypt Royale Together: SharedCryptsRun, `sharedRun`) is the one dungeon for everyone and is opened by
// the host's Travel on every machine. IsSoloLevel stays the name test on purpose (it says "this is the dungeon level", which the
// gather code needs for Apart); the run decides what is allowed.
#pragma once
#include <string>

namespace hsm {

// A level path or short map name of the procedural block maps ("Map_Procedural_Block_Test_*", folder "ProceduralBlocksTest").
inline bool IsSoloLevel(const std::string& level) {
    return level.find("Map_Procedural_Block_") != std::string::npos || level.find("/ProceduralBlocksTest/") != std::string::npos;
}

// The Crypts level the start menu opens (Game Mode 6): the one a gather opens on every machine, and the lobby's Crypts map.
inline constexpr const char* kCryptsLevel = "/Game/Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main";

// Whether a session may open `level` now: every level but the solo ones, and the Crypts while this machine opens its own
// gather run (`gatherRun`, Crypt Royale Apart) or follows the host into a shared run (`sharedRun`: the lobby's mode is one
// dungeon for everyone). Even then only the Crypts level: no other solo level.
inline bool SoloLevelAllowed(const std::string& level, bool gatherRun, bool sharedRun = false) {
    if (!IsSoloLevel(level)) return true;
    return (gatherRun || sharedRun) && level.find("Map_Procedural_Block_Test_Main") != std::string::npos;
}

}  // namespace hsm

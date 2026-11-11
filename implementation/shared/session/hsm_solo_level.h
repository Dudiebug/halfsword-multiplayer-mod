// Levels that are single-player content: a session never travels into them and never treats them as a match level.
// Build 26568321 added the procedural dungeon (Game Mode enum 6, "Roam"): the Seer's "Go to Hell" and the start menu's top
// button open Maps/Test/ProceduralBlocksTest/Map_Procedural_Block_Test_Main
// (UI_Dialog_Widget_Seer_GoToHell.cpp:54-59, UI_Startup_Menu.cpp:128-130). BP_Procedural_Dungeon_Director builds the dungeon
// from unseeded random draws at BeginPlay (BP_Procedural_Dungeon_Director.cpp:408), so two machines would walk two different
// dungeons, and BP_LevelManager does no match setup in modes 5 and 6 (BP_LevelManager.cpp:758), so there is no spawn for a
// level gate to wait for. Pure name test; the DLL decides what to do with the answer.
#pragma once
#include <string>

namespace hsm {

// A level path or short map name of the procedural block maps ("Map_Procedural_Block_Test_*", folder "ProceduralBlocksTest").
inline bool IsSoloLevel(const std::string& level) {
    return level.find("Map_Procedural_Block_") != std::string::npos || level.find("/ProceduralBlocksTest/") != std::string::npos;
}

}  // namespace hsm

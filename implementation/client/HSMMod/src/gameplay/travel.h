// Host-driven travel. Blueprint level changes call UGameplayStatics::OpenLevel natively (not via ProcessEvent),
// so the C++ implementation is hooked directly. The session decides per call whether to let it through.
#pragma once
#include "hsm_protocol.h"
#include "ue/engine.h"
#include <functional>
#include <string>
#include <vector>

namespace hsm::game {

// Return false to swallow the level change (e.g. a lobby client trying to leave the menu on its own).
using OpenLevelFilter = std::function<bool(const std::string& level, bool absolute, const std::string& options)>;

bool InstallTravelHook(OpenLevelFilter filter);
// Opens a level from mod code; bypasses the filter. Game thread only.
void OpenLevel(ue::UObject* worldContext, const std::string& level, bool absolute, const std::string& options);

// GI_Settings fields that define what a level load means (mode, foes, progression event...).
std::vector<Setting> CaptureGameSettings();
size_t ApplyGameSettings(const std::vector<Setting>& settings);   // returns how many were applied
// The names of `want`'s settings whose value in the game instance differs now ("A, B"; empty: all the same).
std::string ChangedGameSettings(const std::vector<Setting>& want);

}  // namespace hsm::game

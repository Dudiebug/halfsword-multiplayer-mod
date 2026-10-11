// Spectating while dead: the local camera follows another living fighter, or returns to our own pawn.
// Game thread only. Which fighter to follow is decided by shared/match/hsm_spectate.h; this only moves the camera.
#pragma once
#include "gameplay/adapter.h"
#include "ue/engine.h"

namespace hsm::game {

// APlayerController::SetViewTargetWithBlend on the local player, blending over blendSeconds (0 cuts). False when
// the world, the local controller or the call is unavailable.
bool SetViewTarget(Adapter& a, ue::UObject* target, float blendSeconds);

// Points the local camera back at the local player's pawn with no blend. False when there is no pawn to return to.
bool ResetViewToPawn(Adapter& a);

}  // namespace hsm::game

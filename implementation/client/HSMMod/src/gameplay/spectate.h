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

// The spectator's camera: a CameraActor of ours kept behind and above `target`'s head (Half Sword's own camera is first
// person: the view target set to another fighter looked out from inside their head, a black screen in the owner's test).
// `snap` places it at once (a new target), otherwise it follows smoothly; walls pull it in. Returns the camera to make
// the view target, nullptr when it could not be spawned.
ue::UObject* FollowCamera(Adapter& a, ue::UObject* target, bool snap, float dtSeconds);

// Destroys the follow camera (spectating is over, or the level is going).
void EndFollowCamera(Adapter& a);

}  // namespace hsm::game

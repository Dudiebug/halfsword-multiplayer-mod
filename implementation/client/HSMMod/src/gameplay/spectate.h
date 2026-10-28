// Spectating while dead: the local camera follows another living fighter, or returns to our own pawn.
// Game thread only. Which fighter to follow is decided by shared/match/hsm_spectate.h; this only moves the camera.
#pragma once
#include "gameplay/adapter.h"
#include "ue/engine.h"
#include <string>

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

// PlayerCameraManager::StopCameraFade on the local camera: a fade to black the game's death flow starts would otherwise
// stay over the follow camera (public #8, #10: a black screen while spectating after a death; joining mid-round, without
// a death, showed the fight).
void ClearCameraFade(Adapter& a);

// The classes of the widgets in the viewport, comma separated ("none"): what covers the screen, for the log.
std::string WidgetsOnScreen();

// The game's vignette widgets (UI_Vignette, UI_VignetteDark, UI_VignetteDamage) darken the screen with the local fighter's
// consciousness, health and pain; a dead fighter's stay dark over the follow camera (public #8, #10: spectating after a
// death is black, after a rejoin without dying it is not). Hidden while spectating (call again now and then: a new HUD
// brings new ones), put back as they were by RestoreDeathOverlays.
// The cause in the decompile is UI_HUD's own BLACK, Vignette_WakeUp and Vignette images (UI_HUD.cpp:77, 86-87: opaque once
// Wake Up Rate falls to 0 in the death timeline, Willie_BP.cpp:8351), hidden here as well. `own`: the dead fighter, for the log line only.
void HideDeathOverlays(ue::UObject* own, bool log);
void RestoreDeathOverlays(Adapter& a);

}  // namespace hsm::game

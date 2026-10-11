// Dismemberment and blood glue between the protocol (session/hsm_combat.h) and the Willie's own Blueprint
// functions. Game thread only. Each function that cannot be backed by a verified game function returns false
// and logs why, so the caller can keep the event out of the simulation instead of guessing.
#pragma once
#include "session/hsm_combat.h"
#include "ue/engine.h"
#include <cstdint>

namespace hsm::game {

// Host: severed-parts bitmask (bit per Enum_DismembermentPart value, 0..14) read from the Willie's
// "Dismembered Parts Map". Returns 0 when the property is missing or does not look like the expected TMap.
uint32_t ReadSeveredMask(ue::UObject* willie);

// Client: replay a dismemberment of `parts` through the Willie's "Dismember Function Initiate".
// Currently always returns false: that function needs the live Attach Marker, VP boxes and weapon that
// Constraint_Weapon_Stuck_BP builds during the cut, and those cannot be synthesized safely.
bool ApplyDismember(ue::UObject* willie, uint32_t parts);

// Client: blood / vertex paint for a CombatEvent of kind kCombatBlood, through the Willie's "Paint Blood Splash".
bool ApplyBlood(ue::UObject* willie, const hsm::CombatEvent& e);

// Host: the Enum_DismembermentPart index whose bone is closest to a world point (for blood events).
uint8_t NearestPartBone(ue::UObject* willie, const ue::FVector& at);

}  // namespace hsm::game

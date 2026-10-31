// Host damage (match/hsm_damage.h has the rule). While a session is on, the game instance's Player Damage Rate is the
// host's and its NPC Damage Rate the stock one, on every machine: the victim's game works the damage out, so this is
// what makes a hit hurt everyone alike. The game's own Save Settings is hooked to write the player's own values (it
// saves every setting on any change, and every level load reads them back: BP_HalfSwordGameMode.cpp:163), so the
// player's Settings save never holds the session's; they are back in the game instance when the session is over.
// Fails closed: without the VM hook nothing is pinned (a pinned value could then reach the save). Game thread only.
#pragma once
#include "match/hsm_damage.h"
#include "hsm_protocol.h"
#include <array>
#include "ue/engine.h"
#include <cstdint>

namespace hsm::game {

// Every tick. active: in a session (joined); stored: the host's MatchConfig::damagePct (kDamageStock: the stock rate);
// host: this machine chose it (inside its slider's range), a client takes it as sent.
void TickDamagePin(bool active, uint16_t stored, bool host);
// Gore Rate, Bone Snapping and Dead Weight Scale (hsm_protocol.h kPinnedPhysicsNames) the same way, every tick after
// TickDamagePin: a client passes the host's MatchInfo.physics (-1: that one stays its own), the host all -1. The
// fighters' cached copies (Willie_BP.cpp:7680) are written too, so gore, bone snaps and fallen weight match everywhere.
void TickPhysicsPin(const std::array<float, kPinnedPhysics>& forced);
// This machine's own values, for the host's MatchInfo (-1 where the game instance has none yet).
std::array<float, kPinnedPhysics> OwnPhysics();
// A fighter caches its damage rate (Damagr Rste Var) when it spawns, before its Player flag is right. Writes the
// pinned one now (no-op outside a session); TickDamagePin also rewrites every fighter's twice a second and on a change.
void RefreshDamageCache(ue::UObject* willie);
// The game's own damage slider (UI_GameSettings' Slider_DamagePlayer), read from its template once it is loaded;
// DamageRange's defaults until then.
const DamageRange& VanillaDamageRange();

}  // namespace hsm::game

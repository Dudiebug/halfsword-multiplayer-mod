// Host damage (match/hsm_damage.h has the rule). While a session is on, the game instance's Player Damage Rate is the
// host's and its NPC Damage Rate the stock one, on every machine: the victim's game works the damage out, so this is
// what makes a hit hurt everyone alike. The game's own Save Settings is hooked to write the player's own values (it
// saves every setting on any change, and every level load reads them back: BP_HalfSwordGameMode.cpp:163), so the
// player's Settings save never holds the session's; they are back in the game instance when the session is over.
// Fails closed: without the VM hook nothing is pinned (a pinned value could then reach the save). Game thread only.
#pragma once
#include "match/hsm_damage.h"
#include <cstdint>

namespace hsm::game {

// Every tick. active: in a session (joined); stored: the host's MatchConfig::damagePct (kDamageStock: the stock rate).
void TickDamagePin(bool active, uint16_t stored);
// The game's own damage slider (UI_GameSettings' Slider_DamagePlayer), read from its template once it is loaded;
// DamageRange's defaults until then.
const DamageRange& VanillaDamageRange();

}  // namespace hsm::game

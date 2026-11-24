// The Biting row: whether a player can bite another fighter. The game's bite (Willie_BP Bite Event, the talk key pressed
// twice) traces from the head and spawns a BP_Constraint_Bite_C that ties the biter's head to what it hit; each tick it
// deals damage to a living thing or feeds the biter from food. With the row Off, a bite that a player's fighter makes on
// a fighter (another player or a bot) is stopped on the machine that simulates the biter (its own machine under owner sync,
// the host under host physics, which plays every player's keys on its copy) before its constraint is made: no hold, no
// damage. Bites on food (eating) and the bots' own bites run as usual.
#pragma once

namespace hsm {

// The rule is in force: a lobby fight (not Progression, whose row it is not) whose host set the row Off.
inline bool BiteOffHere(bool inLobby, bool progression, bool biteOff) { return inLobby && !progression && biteOff; }

// A bite constraint starting here: stopped when the rule is Off, the biter is a player's fighter simulated here (not a bot)
// and the bitten actor is a fighter.
inline bool BlockBite(bool off, bool byPlayer, bool onFighter) { return off && byPlayer && onFighter; }

}  // namespace hsm

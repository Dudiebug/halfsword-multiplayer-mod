// A weapon that left the fighter's own hand without the player letting go (#249, a flail that flew out of the host's hand).
//
// In Willie_BP a weapon leaves a hand on purpose in a few ways: the grab is released (R/L Grab Pressed goes false and
// Dropping R/L is set, Willie_BP.cpp:10434-10443 / 10638-10646), the weapon is sheathed (it appears in a slot variable),
// it is swapped for another, or it is destroyed. A weapon that is gone from the hand with none of those, while the grab is
// still pressed, is the physics (a joint that broke, a hitch) rather than the player, and the log line says so. "Gone" is
// either the hand's weapon variable going null, or the weapon being farther from the hand than a grip reaches while the
// variable still holds it (a flail that flew out). Diagnostic only: it changes nothing in the game. Pure, no engine headers.
#pragma once

namespace hsm::combat {

struct HandLossInput {
    bool ownLive = false;      // the fighter is the player's own, live (not a stand-in): its rig streams frames
    bool wasHeld = false;      // the hand held a real weapon (not fists) at the last read
    bool gone = false;         // the hand's weapon variable is null now (not swapped), or the weapon is out of the hand's reach
    bool grabPressed = false;  // the hand's "Grab Pressed" is still true
    bool dropping = false;     // the hand's "Dropping" flag is set (the game's release path)
    bool sheathed = false;     // the weapon is in one of the fighter's slot variables now
    bool destroyed = false;    // the weapon actor is gone
};

// True when the line "left the hand ... no release pressed" is due.
constexpr bool LeftHandUnasked(const HandLossInput& in) {
    return in.ownLive && in.wasHeld && in.gone && in.grabPressed && !in.dropping && !in.sheathed && !in.destroyed;
}

}  // namespace hsm::combat

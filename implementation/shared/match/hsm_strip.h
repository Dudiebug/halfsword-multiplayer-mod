// The Stripping row (#1931): whether a downed fighter's armour can be pulled off. The game's strip (Willie_BP Strip Armor
// From Bone, run when a hand lets go of a fallen fighter) calls Remove Armor on the fighter being stripped and puts the
// piece in the stripper's hand; the same call can also take a weapon from that fighter's hands or sheaths. With the row
// Off, the Remove Armor that a strip makes is blocked on the stripper's machine and the owner refuses a strip request,
// so the armour stays on. Weapons can still be taken.
#pragma once

namespace hsm {

// The rule is in force: a lobby fight (not Progression, whose row it is not) whose host set the row Off.
inline bool StripOffHere(bool inLobby, bool progression, bool stripOff) { return inLobby && !progression && stripOff; }

// A Remove Armor call made while a strip runs: blocked when the rule is Off and it acts on the fighter being stripped
// (stripping: that strip's Stripped Willie, null outside a strip). The fighter's other armour changes are left alone.
inline bool BlockStripArmor(bool off, const void* stripping, const void* removedFrom) {
    return off && stripping != nullptr && stripping == removedFrom;
}

}  // namespace hsm

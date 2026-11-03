// Ground clear (lobby setting "Clear ground", and the Duel new-round / rematch clear): which weapon actors lying about are
// "dropped" and go. Pure rule; gameplay/adapter.cpp DestroyDroppedWeapons applies it to every weapon actor.
//
// The game's own "Last Parent" cannot tell a drop from the level's weapons: ModularWeaponBP clears it by itself once the weapon
// is 300 uu from its last holder (a 0.2 s poll) and on a re-grab, so almost every real drop (thrown, kicked, a holder that
// died and respawned) has none. The rule goes by what a weapon is now:
//   held / sheathed   a Parent Actor (Willie_BP "Weapon R/L" and the sheath slots all keep it) -> kept
//   fists, feet       the fighter's body weapons -> kept
//   level start       a weapon the level placed that still lies at its spot (the map reset's record) -> kept
//   anything else     a dropped weapon -> cleared. That includes a level weapon someone picked up and put down elsewhere;
//                     the map reset respawns it on its spot at the next round, and without a map reset it is a drop like any.
//
// The host's /clear [all|weapons|armor|rocks] (rev 35, GroundClear 59): one reliable message, and every machine clears its own copy of
// the ground, like the timer's clear. "weapons" and "armor" are the passes above; "rocks" is any loose level prop that has been moved
// (rocks, chairs, buckets, loose planks...) and every object spawned during the match. Scenery that was never moved stays, and so do
// weapons, armour, multi-part structures (bridges, chains) and anything a hand holds.
#pragma once
#include "hsm_protocol.h"
#include <cstdint>
#include <string>

namespace hsm {

// How far a level weapon may lie from its recorded spot (uu, 1.5 m) and still count as untouched: settling on a rack or a
// nudge by a passing body is not a drop; a kick or a throw is.
constexpr double kLevelWeaponRestUu = 150.0;

inline bool AtLevelStart(double distanceSquaredUu) {
    return distanceSquaredUu >= 0.0 && distanceSquaredUu <= kLevelWeaponRestUu * kLevelWeaponRestUu;
}

struct DropCandidate {
    bool hasParent = false;       // "Parent Actor" set: in a hand or sheathed on a body
    bool inHand = false;          // with hasParent: the parent's Weapon R / Weapon L is this weapon (else it is sheathed)
    bool bodyWeapon = false;      // fists or feet
    bool levelStart = false;      // a recorded level weapon still at its start spot
    bool beingDestroyed = false;  // already going
    bool modOwned = false;        // an actor the mod spawned and keeps (Capture the flag's flags): never a drop, whatever it lies on
};

enum class DropVerdict : uint8_t { Clear, KeepHeld, KeepSheathed, KeepLevel, KeepOther };

inline DropVerdict ClearableDrop(const DropCandidate& c) {
    if (c.bodyWeapon || c.beingDestroyed || c.modOwned) return DropVerdict::KeepOther;
    if (c.hasParent) return c.inHand ? DropVerdict::KeepHeld : DropVerdict::KeepSheathed;
    if (c.levelStart) return DropVerdict::KeepLevel;
    return DropVerdict::Clear;
}

// ---------------------------------------------------------------- /clear
constexpr uint8_t kClearWeapons = 1, kClearArmor = 2, kClearRocks = 4, kClearAll = kClearWeapons | kClearArmor | kClearRocks;

// The word after /clear (lower case or not): a mask, 0 for a word that is none of them. No word is "all".
inline uint8_t ParseClearMask(const std::string& word) {
    std::string w;
    for (char c : word) w += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (w.empty() || w == "all") return kClearAll;
    if (w == "weapons" || w == "weapon") return kClearWeapons;
    if (w == "armor" || w == "armour") return kClearArmor;
    if (w == "rocks" || w == "rock") return kClearRocks;
    return 0;
}

// "weapons, armour, rocks" for the chat line.
inline std::string ClearMaskText(uint8_t mask) {
    std::string out;
    auto add = [&](uint8_t bit, const char* name) { if (mask & bit) { if (!out.empty()) out += ", "; out += name; } };
    add(kClearWeapons, "weapons"); add(kClearArmor, "armour"); add(kClearRocks, "rocks");
    return out;
}

// Wire: 4 header, u8 seq, u8 mask. A machine ignores the seq it already applied.
struct GroundClearMsg {
    uint8_t seq = 0;
    uint8_t mask = 0;     // kClear* bits; never 0
};
inline std::vector<uint8_t> Encode(const GroundClearMsg& m) {
    Writer w; w.header(MsgType::GroundClear); w.u8(m.seq); w.u8(m.mask); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, GroundClearMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::GroundClear) return false;
    m.seq = r.u8(); m.mask = r.u8();
    return r.done() && m.mask != 0 && (m.mask & ~kClearAll) == 0;
}

// What the rocks pass does with one tracked prop (gameplay/object_sync.h Prop).
struct PropCandidate {
    bool level = false;           // a level prop (not a dropped weapon)
    bool spawned = false;         // an object spawned during the match or the level's start-up
    bool haveStart = false;       // a level prop: its start transform was recorded
    bool unmoved = false;         // a level prop: every body is still where it started
    bool bornInMatch = false;     // a spawned object: it appeared after the match baseline
    bool held = false;            // a hand holds it, or it hangs on another object
    bool denied = false;          // held here while another machine holds it
    bool sub = false;             // a body that came off another object
    bool structure = false;       // constraint-built (bridge, chain): hsm_structure.h IsStructure
};
inline bool ClearableProp(const PropCandidate& c) {
    if (c.held || c.denied || c.sub || c.structure) return false;
    if (c.level) return c.haveStart && !c.unmoved;
    if (c.spawned) return c.bornInMatch;
    return false;
}

}  // namespace hsm

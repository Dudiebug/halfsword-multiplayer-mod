// Constraint-built level props, "structures" (0.8.0-pre18, #230 "bridge is falling over and over", #85). A level prop that is one
// actor with several physics bodies joined by physics constraints (a hanging bridge, a chain, a plank walkway) is a structure.
// Its joints are held by the game's own physics, and a hanging bridge never reads as at rest (hsm_lease.h PropAtRest), so up to
// 0.8.0-pre17 it was streamed like a loose prop at sixty states a second and the receiving copy moved every body by teleport,
// each from a different packet. A teleport ignores the joints, which took huge impulses, snapped, and the bridge fell on that
// machine only. sync_structures (HSMMod.ini [net]) picks what is done instead:
//
//   soft  (default) The host simulates the structure and streams all its bodies in one go, every kStructureStreamMs while any
//                   of them moves. A receiver never teleports a body of it: it steers each one toward the host's pose with
//                   velocity (sync/hsm_soft_correct.h) and places the whole structure at once only when it is badly off. Nobody
//                   leases it (a machine's fighter pushes its own copy; the host's copy is pushed by that fighter's stand-in),
//                   and the map reset leaves it as it is.
//   local           Never streamed, never driven, never leased, never reset: every machine simulates its own copy and the copies
//                   start alike. Breaks still travel as ObjectEvents from whichever machine sees them.
//   full            As 0.8.0-pre17: nothing is a structure; it is streamed, teleported, leased and reset like any loose prop.
//
// A body that detaches is an object of its own (one body, no constraints: not a structure) and streams as any prop in every mode.
// The rule is by shape and does not change when a joint breaks; bodies still joined after a break stay a structure. Weapons and
// spawned objects are never structures. Game-independent.
//
// Bandwidth of the soft stream (BodyState is 62 bytes, one per body): bodies x 62 B x 20 Hz. A 10-body bridge sends 12.4 KB/s
// (about 100 kbit/s) to each client while it swings; at rest the same 620 B once a second. That is the price of "every machine
// sees the same bridge"; the old 60 Hz stream was three times it.
#pragma once
#include "session/hsm_lease.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace hsm {

enum class StructureMode : uint8_t { Soft = 0, Local = 1, Full = 2 };

// While any body of a soft structure moves, the host sends all its bodies this often (20 Hz); at rest every kFreeIdleMs.
constexpr double kStructureStreamMs = 50.0;

// The INI value. "soft", "local" and "full" by name (any case); the old on/off values keep their meaning of "not pre17" and
// "pre17": "0" is soft, "1" is full. Anything else (or nothing) is soft.
inline StructureMode ParseStructureMode(const std::string& v) {
    std::string s;
    for (char c : v) if (c != ' ' && c != '\t') s += char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    if (s == "local") return StructureMode::Local;
    if (s == "full" || s == "1") return StructureMode::Full;
    return StructureMode::Soft;
}
inline const char* StructureModeName(StructureMode m) {
    return m == StructureMode::Local ? "local" : m == StructureMode::Full ? "full" : "soft";
}

// The shape that makes a structure: a level prop that is not a detached body, with two or more bodies and a constraint.
inline bool IsStructure(uint64_t propId, bool detachedBody, size_t bodies, size_t constraints) {
    return IsLevelProp(propId) && !IsWeaponId(propId) && !detachedBody && bodies >= 2 && constraints >= 1;
}

// local: this machine keeps its own copy and nothing about the prop is streamed, driven, claimed or reset.
inline bool StructureLocal(StructureMode mode, uint64_t propId, bool detachedBody, size_t bodies, size_t constraints) {
    return mode == StructureMode::Local && IsStructure(propId, detachedBody, bodies, constraints);
}

// soft: the host streams it as one snapshot, a receiver steers its copy (never teleports a body).
inline bool StructureSoft(StructureMode mode, uint64_t propId, bool detachedBody, size_t bodies, size_t constraints) {
    return mode == StructureMode::Soft && IsStructure(propId, detachedBody, bodies, constraints);
}

// Nobody leases it, claims a blow on it or resets it: local and soft (every machine's own physics acts on its copy).
inline bool StructureOwnCopy(StructureMode mode, uint64_t propId, bool detachedBody, size_t bodies, size_t constraints) {
    return mode != StructureMode::Full && IsStructure(propId, detachedBody, bodies, constraints);
}

// The host sends a soft structure's bodies when this is true: lastSentMs far in the past means never sent. atRest: none of
// its bodies moves (hsm_lease.h PropAtRest on each).
inline bool StructureStreamDue(double lastSentMs, bool atRest, double nowMs) {
    return nowMs - lastSentMs >= (atRest ? kFreeIdleMs : kStructureStreamMs);
}

}  // namespace hsm

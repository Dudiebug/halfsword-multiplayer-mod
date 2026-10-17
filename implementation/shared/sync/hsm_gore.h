// Per-stand-in dislocation + sever-fallback decisions, driven by the owner's Vitals (severed/dislocated
// bitmasks), a steady clock, and (for severed parts) a notice that the native cut already played locally.
// Pure: no engine types, no allocation, no exceptions -- the game-thread side (standin_gore.h) owns one
// GoreTracker per stand-in entity and turns its GoreActions into engine calls.
//
// Severed-part bits are Enum_DismembermentPart values 0..14, the same numbering as hsm::kCombatPartBits /
// combat_sync.cpp's kPartBones (session/hsm_combat.h, implementation/client/HSMMod/src/gameplay/combat_sync.cpp).
// Not included from here: sync/ must not depend on session/, so kGoreSeveredBits duplicates that constant
// and must be kept in sync with it by hand.
//
// Dislocated-joint bits are the six dislocatable joints on Willie_BP
// (re/out/decomp/Character/Blueprints/Willie_BP.cpp:1250-1255, the *_Dislocated bool fields):
//   0 Arm R   1 Arm L   2 Leg R   3 Leg L   4 Neck   5 Spine (the "Back" constraint/field pair)
// The native game only ever sets one of these true, from a tick-driven detection event
// (Event_Check_Bone_Dislocation_Status, Willie_BP.cpp:8875+) that compares the physical bone against the
// animated one and requires live physical-animation strain ("Bone Snapping > 0.1") -- both unreachable on a
// neutralised stand-in whose Actor tick is blocked, which is exactly why this header exists: the owner
// detects its own dislocations natively and streams the bits, and this is the stand-in side's reaction.
// A grep of the whole decomp tree found no "X Dislocated = false" anywhere, so Relocate() exists
// defensively/symmetrically for when the owner's bit clears, not for anything the vanilla game is observed
// to do (see standin_gore.h for what little it is safe to apply for either transition).
//
// The severed-subtree exclusion (GoreDeadBones) and "a severed part never regrows within one body" (severedSeen_, reset
// only when a seen bit clears) follow Cyrex0/HalfSword-MP mods/HSMPAvatars/Scripts/body_injury.lua (omit, select_mask),
// Copyright (c) 2026 Cyrex0 and the HalfSword-MP contributors, MIT (see THIRD_PARTY_NOTICES.md).
#pragma once
#include "sync/hsm_pose.h"
#include <cstdint>

namespace hsm::sync {

constexpr uint32_t kGoreSeveredBits = 15;                              // mirrors hsm::kCombatPartBits
constexpr uint32_t kGoreSeveredMask = (1u << kGoreSeveredBits) - 1u;
constexpr uint32_t kGoreJoints = 6;
constexpr uint32_t kGoreJointMask = (1u << kGoreJoints) - 1u;

// A severed part's cosmetic fallback (hide + physics-inert, standin_gore.cpp) fires this long after the
// owner's Vitals first reports the bit, unless NoteNativeSever() for that part arrives first. Vitals is
// unreliable at 15 Hz (~66 ms/packet); 400 ms covers several dropped packets plus the travel time of the
// reliable sever claim that replays the native cut on this stand-in (the sever path in session.cpp calls
// NoteNativeSever after Delayed ran and the native part map confirms completion), so on any normal connection the
// native cut wins the race and this fallback is the exception path, not the common one.
constexpr double kSeverGraceMs = 400.0;

// After a respawn notice (AwaitWhole) samples carrying gore are ignored until the first whole one, or until this long
// after the first ignored one. Vitals is unreliable and unordered: the old body's last samples can still be in flight
// (or reordered) when the respawn notice lands, and taking them would dislocate or cut the new body. A new body that is
// hurt before any whole sample got through still gets its gore, this late. The wait cannot tell a reordered old sample
// that arrives after the first whole one from the new body's own gore (VitalsMsg carries no life counter); StandInGore's
// per-actor GoreRecord keeps that from doing anything twice, not from doing it once.
constexpr double kAwaitWholeMs = 1500.0;

// What a severed part takes out of the stand-in's own physics, as pose bones (sync/hsm_pose.h numbering): the root
// of the subtree whose bodies are gone. -1: nothing is taken out.
//   kGoreFallbackRoot: the cosmetic fallback hides the part's spawn bone (Willie_BP.cpp:3269 "Spawn Bone", the same
//     table as hsm::combat::SeverSpawnBone), so the whole limb below it goes; the pelvis (part 2) cannot be hidden
//     without hiding the whole body, so the fallback does nothing for it.
//   kGoreNativeRoot: the game's own cut hides "Hide Bone Local" (Willie_BP.cpp:3270) on the parent parts, the bone
//     one step below the cut, with PBO_Term (HideBoneByName(.., 1)); the cut surface itself is runtime vertex paint.
// The test cross-checks both tables against those switch statements.
constexpr int kGoreFallbackRoot[kGoreSeveredBits] = {6, 6, -1, 14, 15, 16, 10, 11, 12, 20, 21, 22, 17, 18, 19};
constexpr int kGoreNativeRoot[kGoreSeveredBits] = {-1, -1, -1, 15, 16, -1, 11, 12, -1, 21, 22, -1, 18, 19, -1};

// The pose bones (bit per sync::kPoseBones index) whose bodies the stand-in no longer has: everything at or below
// a root of a part the fallback hid (fallbackParts) or a part the game's own cut ran for (nativeParts). The servo
// and the blow detector must not trust those bones (physics bodies terminated, bone transforms of a hidden
// subtree), so the session clears their FighterNow::valid. Only the loop's part range is read: stray high bits are ignored.
inline uint32_t GoreDeadBones(uint32_t fallbackParts, uint32_t nativeParts) {
    uint32_t roots = 0;
    for (uint32_t part = 0; part < kGoreSeveredBits; ++part) {
        if (((fallbackParts >> part) & 1u) && kGoreFallbackRoot[part] >= 0) roots |= 1u << kGoreFallbackRoot[part];
        if (((nativeParts >> part) & 1u) && kGoreNativeRoot[part] >= 0) roots |= 1u << kGoreNativeRoot[part];
    }
    uint32_t dead = 0;
    for (int bone = 0; bone < kPoseBones; ++bone)   // parents come first; the pelvis is its own parent, not dead yet when reached
        if (((roots >> bone) & 1u) || ((dead >> kPoseParent[bone]) & 1u)) dead |= 1u << bone;
    return dead;
}

// One decision batch: every field already deduplicated against what this tracker last reported (never the
// same transition twice) and masked to the valid bit ranges above (an out-of-range bit is ignored, not an
// error -- a Vitals sample with one stray high bit must not be thrown away).
struct GoreActions {
    uint32_t showFallback = 0;   // severed-part bits: ShowSeverFallback(part) is due this call
    uint32_t dislocate = 0;      // joint bits: Dislocate(joint) newly true this call
    uint32_t relocate = 0;       // joint bits: Relocate(joint) newly false this call
    bool reset = false;         // see OnVitals: this stand-in's state (and only its state) was just cleared
};

// Per-stand-in state. standin_gore.h keeps one instance per entity, for the life of that entity's feed
// (it survives a respawn's actor swap; OnVitals detects the respawn itself -- see below).
class GoreTracker {
public:
    // Forget everything: used by AwaitWhole (below) and by OnVitals when it detects a respawn itself. Clears every
    // pending grace timer and every joint/part bit already reported, without producing a GoreActions -- when a new
    // actor or a respawn notice arrives the old actor is typically already gone, so there is nothing left to tell to
    // un-show a fallback or un-dislocate a joint on.
    void Reset() { *this = GoreTracker{}; }

    // Reset, then ignore samples with any gore bit until the first whole one, or until kAwaitWholeMs after the first
    // ignored one (see there). StandInGore arms it on a respawn notice (StandInGore::Forget) and whenever an entity's
    // stand-in becomes a new actor (StandInGore::Bind: first attach, respawn, level change, rebuild); Reset() alone
    // never waits.
    void AwaitWhole() {
        Reset();
        awaiting_ = true;
    }

    // One Vitals sample (hsm::VitalsMsg::severed / ::dislocated, unpacked by the caller so this header does
    // not need to depend on protocol/hsm_owner.h). nowMs: the caller's steady clock (must be monotonic and
    // shared with the Tick() calls below; which clock doesn't matter, as in hsm_servo.h's ServoState).
    //
    // Detects a respawn: a part bit this tracker previously saw severed is now clear. The native game never
    // un-severs a part on a live body (see the file comment), so that can only mean a fresh body, and this
    // resets before applying the new masks -- otherwise a fresh body's first (all-zero) Vitals would read as
    // "every previously-severed part just got un-severed" and silently swallow the respawn.
    // VitalsMsg carries no sequence number, so a stale sample reordered by the transport looks the same as a
    // respawn here; the caller (StandInGore) therefore keeps its own per-actor record of what it already did,
    // which makes the repeat actions that follow such a spurious reset no-ops.
    GoreActions OnVitals(uint32_t severed, uint32_t dislocated, double nowMs) {
        severed &= kGoreSeveredMask;
        dislocated &= kGoreJointMask;
        GoreActions out;
        if (awaiting_ && (severed | dislocated)) {
            if (!awaitStarted_) { awaitStarted_ = true; awaitSinceMs_ = nowMs; }
            if (nowMs - awaitSinceMs_ < kAwaitWholeMs) return out;
        }
        awaiting_ = false;
        if ((severedSeen_ & severed) != severedSeen_) {
            Reset();
            out.reset = true;
        }
        for (uint32_t part = 0; part < kGoreSeveredBits; ++part) {
            const uint32_t bit = 1u << part;
            if ((severed & bit) && !(severedSeen_ & bit) && !(nativeApplied_ & bit)) { pendingSinceMs_[part] = nowMs; pending_ |= bit; }
        }
        severedSeen_ |= severed;
        out.dislocate = dislocated & ~dislocatedSeen_;
        out.relocate = dislocatedSeen_ & ~dislocated;
        dislocatedSeen_ = dislocated;
        out.showFallback = DueNow(nowMs);
        return out;
    }

    // The game's own cut already played on this stand-in for `part` (session.cpp's sever-claim replay, not the
    // legacy host CombatEvent/ApplyDismember path, which never runs in owner sync and replays nothing anyway).
    // Cancels this part's pending grace timer so the fallback never fires for it, and remembers that for any
    // later OnVitals so a part reported severed only after its native cut cannot restart one either. Out-of-
    // range parts are ignored like everywhere else in this header.
    void NoteNativeSever(uint8_t part) {
        if (part >= kGoreSeveredBits) return;
        nativeApplied_ |= 1u << part;
        pending_ &= ~(1u << part);
    }

    // Call once per frame (independent of Vitals' 15 Hz) so a pending grace period fires on time instead of
    // waiting for the next Vitals packet to happen to land after it.
    GoreActions Tick(double nowMs) {
        GoreActions out;
        out.showFallback = DueNow(nowMs);
        return out;
    }

private:
    uint32_t DueNow(double nowMs) {
        uint32_t due = 0;
        for (uint32_t part = 0; part < kGoreSeveredBits; ++part) {
            const uint32_t bit = 1u << part;
            if (!(pending_ & bit)) continue;
            if (nowMs - pendingSinceMs_[part] >= kSeverGraceMs) {
                due |= bit;
                pending_ &= ~bit;                  // fires at most once per part (idempotent)
            }
        }
        return due;
    }

    uint32_t severedSeen_ = 0;      // every severed bit ever reported to this tracker (never cleared except by Reset)
    uint32_t dislocatedSeen_ = 0;   // the dislocated mask as of the last OnVitals (for edge-triggering dislocate/relocate)
    uint32_t nativeApplied_ = 0;    // parts NoteNativeSever has fired for: never start (or restart) a grace timer
    uint32_t pending_ = 0;          // parts whose grace timer is running; a mask, not a time sentinel, so any clock origin works
    double pendingSinceMs_[kGoreSeveredBits] = {};   // when each pending part was first reported (valid only while its pending_ bit is set)
    bool awaiting_ = false;         // AwaitWhole: ignoring samples with gore until a whole one or the deadline
    bool awaitStarted_ = false;     // the first ignored sample has set awaitSinceMs_ (a flag, so any clock origin works)
    double awaitSinceMs_ = 0;
};

// What an entity is owed, from its tracker's decisions: the parts whose fallback came due and the joints its owner reports
// dislocated now. It outlives the stand-in actor (StandInGore keeps one per entity), so a decision made while no actor
// was attached is still owed to the next one. A reset drops everything owed before it, then takes the batch's own bits.
struct GoreOwed {
    uint32_t due = 0;          // severed-part bits whose fallback is due
    uint32_t dislocated = 0;   // joint bits the owner reports dislocated
    void Apply(const GoreActions& a) {
        if (a.reset) { due = 0; dislocated = 0; }
        due |= a.showFallback;
        dislocated = (dislocated | a.dislocate) & ~a.relocate;
    }
};

// What has been done to one stand-in actor (StandInGore keeps one per attached actor, dropped with it): parts hidden by
// the fallback, parts the game's own cut ran for, joints freed, and what was given up after the retries. A part or joint
// in here is never done again on that actor, whatever the tracker owes (a reordered sample reads as a respawn there).
struct GoreRecord {
    uint32_t hidden = 0;         // severed-part bits the fallback hid
    uint32_t native = 0;         // severed-part bits the game's own cut ran for
    uint32_t gaveUp = 0;         // severed-part bits whose fallback failed every retry
    uint32_t freed = 0;          // joint bits freed
    uint32_t jointsGaveUp = 0;   // joint bits whose freeing failed every retry
    uint32_t PartsToShow(uint32_t due) const { return due & kGoreSeveredMask & ~(hidden | native | gaveUp); }
    uint32_t JointsToFree(uint32_t dislocated) const { return dislocated & kGoreJointMask & ~(freed | jointsGaveUp); }
    uint32_t DeadBones() const { return GoreDeadBones(hidden, native); }
};

}  // namespace hsm::sync

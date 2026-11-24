// The world context a mod call hands the engine (public #855). UGameplayStatics' functions (OpenLevel, PlaySound2D,
// BeginDeferredActorSpawnFromClass, ...) find their world through a context object: GetWorldFromContextObject calls its
// virtual GetWorld(). The mod keeps the game mode of the level it resolved (Session's lastGameMode_, the adapter's world_)
// and passes it as that context. After a level change that cached mode is freed, and the mod's tick runs its lobby and
// host-migration parts BEFORE it resolves the new level: on the new level's first tick a host migration that gave up
// opened the menu with the old mode (ReturnToMenuAfterHostLoss), and a member's join played its sound with it
// (onPresence -> PlayUiSound). Both crashed on the freed object's vtable.
//
// The rule: the mod tick records the game mode whose ReceiveTick runs (alive by definition) with its object-array slot
// before anything else. A call then gets the context it holds only when that is the recorded mode and the slot still
// holds it; a context from an earlier level is replaced by the recorded mode (the call still runs, in the level that
// is open); with no live mode known (none ticked yet, or it was freed during a load) the call gets no context, which
// the engine treats as "no world" and does nothing with.
#pragma once
#include <cstdint>

namespace hsm {

enum class WorldContextPick : uint8_t {
    Wanted,   // the caller's context is the live game mode: used as it is
    Live,     // the caller holds an earlier level's mode: the live mode instead
    None,     // no live mode known, or the caller holds none: no context (the engine call does nothing)
};

// wanted: the context the caller holds. live: the game mode that ticked last (recorded at the top of the mod tick).
// liveInSlot: the object array's slot recorded for `live` still holds it and it is not garbage (it was not freed).
constexpr WorldContextPick PickWorldContext(const void* wanted, const void* live, bool liveInSlot) {
    if (!wanted || !live || !liveInSlot) return WorldContextPick::None;
    return wanted == live ? WorldContextPick::Wanted : WorldContextPick::Live;
}

// The pointer a pick stands for.
constexpr const void* WorldContextFor(WorldContextPick pick, const void* wanted, const void* live) {
    return pick == WorldContextPick::Wanted ? wanted : pick == WorldContextPick::Live ? live : nullptr;
}

}  // namespace hsm

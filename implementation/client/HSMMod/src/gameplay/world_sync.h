// Game-side glue for world sync of loose weapons (dropped/thrown/disarmed). All wire format and selection
// logic lives in session/hsm_world.h; this file only knows how to read/write a weapon actor through the adapter.
#pragma once
#include "gameplay/adapter.h"
#include "session/hsm_world.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace hsm::game {

// Host, per tick: every weapon actor in the world not currently held by a fighter. entityOfFighter maps a
// Willie actor (the weapon's last holder) to its protocol entity id.
std::vector<hsm::WorldSample> CollectLooseWeapons(Adapter& a, const std::function<uint8_t(ue::UObject*)>& entityOfFighter);

// Client: moves a weapon actor to the host-reported pose. actor must already exist (see FindClientWeapon).
bool ApplyWorldObject(Adapter& a, ue::UObject* actor, const hsm::WorldObject& o);

// Client: locates the weapon at weaponIndex belonging to ownerEntity's fighter, among weapons already spawned
// with that fighter's loadout (puppets carry the same weapons in the same order as their passport).
ue::UObject* FindClientWeapon(Adapter& a, uint8_t ownerEntity, int weaponIndex,
                               const std::function<ue::UObject*(uint8_t)>& actorOfEntity);

}  // namespace hsm::game

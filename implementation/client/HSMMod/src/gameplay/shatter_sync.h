// Shattering objects in owner sync (plan M0 extension, 0.8.0-pre2): Chaos geometry collections (UGeometryCollectionComponent,
// e.g. Assets/Traps/WaspNests/Destructible/GC_SM_Trap_WaspNest_B_001_Destructible). They are objects like any other
// (object_sync.h: the walk counts a geometry collection as a simulating body, so its actor gets an id and a streamer), but
// its body is never streamed or driven: what travels is the BREAK, as an ObjectEvent Fracture (session/hsm_object_event.h).
//   The machine that streams the object (its lessee, the host while it is FREE): damage from collisions on, notifications on
//   (SetNotifyBreaks), and every 250 ms (ObjectSync::PollEvents cadence) IsRootBroken is asked of each collection; the
//   first time it says yes a Fracture is sent (rate capped per object, FractureRate).
//   Every other machine: SetEnableDamageFromCollision(false) on its copy, so it never shatters on its own and differently.
//   On a Fracture it applies the same break (ApplyExternalStrain on the item at the place, with the sender's strain) and, if
//   the root is still whole 1.5 s later, CrumbleCluster on that item.
//   The host keeps a FractureLog (session/hsm_fracture.h) and sends it, in order, to a machine that joins late.
// Not covered (pre2): the loose chunks' physics (debris is local, each machine simulates its own); breaks of inner clusters
// after the root (IsRootBroken is the only cluster state a reflected call exposes; OnChaosBreakEvent is a multicast delegate,
// not a UFunction anyone can hook).
// ObjectSync's members for this live in shatter_sync.cpp. Game thread only.
#pragma once
#include "session/hsm_fracture.h"
#include "ue/engine.h"
#include <cstdint>
#include <map>
#include <vector>

namespace hsm::game {

// A component of class GeometryCollectionComponent (or derived). Cached per class; false until the class exists.
bool IsGeometryCollection(const ue::UObject* comp);

struct ShatterBody {                   // one geometry collection component: a body of its object
    ue::UObject* comp = nullptr;
    int32_t index = -1;                // its slot in the object array (StillValid)
    uint8_t body = 0;                  // its BodyIndex under the object (ObjectEvent::body)
    int32_t rootItem = -1;             // GetRootIndex: the cluster whose break is the shattering
    bool notify = false;               // SetNotifyBreaks(true) was called
    bool damageOff = false;            // SetEnableDamageFromCollision(false) was called (this machine does not stream it)
    std::vector<uint16_t> broken;      // items whose break was sent from here or applied here
    double verifyMs = 0;               // an applied break is checked at this time (0: none pending)
    uint16_t verifyItem = 0;
};

struct ShatterObject {
    ue::UObject* actor = nullptr;      // the actor this scan was made for
    int32_t index = -1;
    std::vector<ShatterBody> gcs;      // empty for an object with no geometry collection (kept so it is scanned once)
};

struct ShatterState {
    std::map<uint64_t, ShatterObject> objs;
    FractureLog log;                   // host: what broke, for a late joiner
    FractureRate rate;
    bool breakablesLogged = false;     // this level's breakables line was written
    uint64_t found = 0, sent = 0, applied = 0, crumbled = 0, alreadyBroken = 0, rateDropped = 0;
};

}  // namespace hsm::game

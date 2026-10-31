// Stand-in hands (the weapon half of owner sync, moved out of prop_sync.cpp unchanged): what an owned fighter held, what a
// stand-in's hands hold against its owner's pose (sync/hsm_handmatch.h), the drops heard of and the weapons a stand-in let go
// of. The functions are members of the prop class (weapon_hands.cpp); these are the records they keep.
#pragma once
#include "sync/hsm_handmatch.h"
#include "sync/hsm_late_drop.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>

namespace hsm::game::hands {

constexpr double kResolveWindowMs = 1500.0;   // a drop's Grant waits this long for the owner's pose to show the hand empty
constexpr int kRestoreTicks = 3;              // the rig's full neutralise may re-apply its body policy once before it lets go
constexpr double kRetryMs = 250.0;            // a stand-in hand's attach or release waits this long before it is tried again,
constexpr double kRetryMaxMs = 4000.0;        // doubling up to this, until the owner's pose for that hand changes
constexpr double kLooseKeepMs = 10000.0;      // a weapon a stand-in let go of here waits this long for its drop to be heard of
constexpr double kWeaponsRefreshMs = 10000.0; // the attach's list of weapon actors (WeaponActors walks every object) is read
                                              // again at most this often; props and loose_ are always looked at
constexpr double kSpawnAfterMs = 1500.0;      // a pose showing a class no weapon here has for this long gets one spawned in
                                              // the stand-in's hand (round start: the loadout fills the hand within 0.2 s)
constexpr double kClassWalkMs = 10000.0;      // the loaded weapon classes are walked again (on a miss) at most this often
constexpr uint64_t kLogFirst = 10;            // the first attaches, releases, self-drops and adoptions are logged in full
constexpr uint8_t kWorldStatic = 0, kWorldDynamic = 1, kPawn = 2, kPhysicsBody = 5, kBlock = 2;   // ECollisionChannel / ECollisionResponse

struct Hands {                         // what an owned fighter held as of the previous tick
    ue::UObject* willie = nullptr;
    std::array<ue::UObject*, 2> weapon{};
    std::array<int32_t, 2> index{{-1, -1}};
};
struct StandInHand {                   // one hand of a stand-in, as the matcher last left it
    sync::HandMatch match;
    ue::UObject* held = nullptr;       // the real weapon in it (not fists, for the left not the right's again)
    int32_t heldIndex = -1;
    uint32_t heldClass = 0;
    double episodeMs = -1;             // the pose's since-time this hand's attach retries belong to
    double retryMs = 0, backoffMs = 0;
    double reattachMs = -1e18;         // the last time a weapon that left by itself was taken back
    bool selfDropLogged = false;       // its first loss since the stand-in was bound (left by itself, or gone) is logged
    // A lease change from the owner let this hand go (NoticeDrop) while the owner's pose still showed that hand in the
    // episode that began at leaseLetGoPoseMs: until the pose moves on the hand takes nothing (it would take the weapon
    // just dropped).
    bool leaseLetGo = false;
    double leaseLetGoPoseMs = 0;
};
struct StandInHands {
    ue::UObject* willie = nullptr;
    int32_t index = -1;
    double boundMs = 0;                // when this stand-in was bound (a respawn binds a new one)
    std::array<StandInHand, 2> hand{};
};
struct PendingDrop {                   // a drop heard of (its Touch Grant), not resolved yet
    double heardMs = 0;
    double deadlineMs = 0;
    ue::UObject* witness = nullptr;    // the weapon that hand of the stand-in held when it was heard (sync::DropFacts)
    int32_t witnessIndex = -1;
    uint32_t heardClass = 0;           // the class the owner's hand showed when it was heard (0: not known): GiveUp's late match
};
struct LateDrop {                      // a drop given up on: its stream will say where the weapon is (sync/hsm_late_drop.h)
    double givenUpMs = 0;
    double triedMs = 0;
    uint32_t classHint = 0;
};
struct Loose {                         // a weapon a stand-in let go of here that is not a prop (yet)
    ue::UObject* actor = nullptr;
    int32_t index = -1;
    ue::UObject* root = nullptr;
    int32_t rootIndex = -1;
    uint8_t entity = 0;                // the stand-in's, 0 once that fighter is gone
    int hand = 0;
    uint32_t classHash = 0;
    double atMs = 0;
    int restoreTicks = 0;
};
struct Candidate {                     // a weapon actor the attach may take (WeaponActors, refreshed now and then)
    ue::UObject* actor = nullptr;
    int32_t index = -1;
    uint32_t classHash = 0;
};
enum class DropResult : uint8_t { Dropped, Skipped, Failed };

}  // namespace hsm::game::hands

// Weapons drawn and sheathed, mirrored from each fighter's owner (owner sync, protocol rev 10, combat/hsm_equip.h).
//
// A stand-in's tick is blocked, so it never draws or sheathes by itself: the owner's drawn weapon stayed on the
// stand-in's belt and its hand stayed empty. The owner polls its own fighters (hands, sheath slots, their constraints)
// and reports each one's state as an Equip message when it changes (and every kEquipResendMs). Every other machine
// reads its stand-in the same way and runs the game's own steps to match (combat::PlanEquip):
//   Draw     "Set Up Right/Left Hand Weapon" with the slot's weapon (the hand grabs it while still sheathed), then what
//            Check Scabbard Unsheath State does once it is out: the slot's constraint broken and destroyed, the weapon's
//            "Unsheathed Mode", the slot variable cleared (Willie_BP.cpp:2324);
//   Sheathe  the slot's socket ("R/L Sheath Slot Name") and scabbard ("Current Scabbard R/L") set as Assign R/L Weapon
//            Slot sets them, "Create Sheath Constraint R/L", then the hand let go with "Set Up ... Hand Weapon" and no
//            weapon (Willie_BP.cpp:2874, 17801).
// Pickups and drops stay with prop_sync. Game thread only.
#pragma once
#include "combat/hsm_equip.h"
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hsm::game {

class EquipSync {
public:
    struct Env {
        std::function<void(const combat::EquipStateMsg&)> send;   // a state of a fighter simulated here (the session routes it)
    };
    void Start(Adapter& adapter, StandIns& standIns, Env env);
    void Stop();
    // Polls the fighters simulated here (entity, Willie) and sends each state that is due.
    void Tick(double nowMs, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned);
    void Apply(const combat::EquipStateMsg& m);                     // a fighter simulated elsewhere: its stand-in matches it
    void Forget(uint8_t entity);
    std::string HealthLine();                                        // since the previous call

private:
    struct Counters {
        uint64_t sent = 0, readFailed = 0, applied = 0, noStandIn = 0, draws = 0, sheathes = 0, skipped = 0, failed = 0;
    };
    struct Read { combat::EquipSnapshot snap; std::array<ue::UObject*, combat::kEquipSlots> slots{}; std::array<ue::UObject*, 2> hands{}; };
    bool ReadFighter(ue::UObject* willie, Read& out) const;
    bool Draw(ue::UObject* standIn, const Read& r, int hand, int slot);
    bool Sheathe(ue::UObject* standIn, const Read& r, int hand, int slot);
    bool LetGo(ue::UObject* standIn, int hand);

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    Env env_;
    double nextPollMs_ = 0;
    uint16_t seq_ = 0;
    combat::EquipSender sender_;
    std::map<uint8_t, combat::EquipTracker> own_, standIn_;          // per entity: fighters simulated here, stand-ins
    Counters n_, reported_;
};

}  // namespace hsm::game

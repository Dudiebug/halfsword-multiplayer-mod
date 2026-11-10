// Agent command "twin" (and -hsmtwin): the drift twin bench (protocol rev 42, sync/hsm_twin.h; docs/PLAN_COMPETITIVE_NETCODE.md 7,
// Phase 0's key experiment and the Phase 4-5 prototype). No second player needed. A second, LIVE fighter dressed from
// the player's own passport stands offsetUu to the player's left and is driven by the player's own input frames, sent
// as InputBatch over a simulated link (sync/hsm_inputline.h: due times, prediction with decay) and played through the
// host-physics drive path. With "correct", the player's pose stream crosses the same kind of link and the twin is
// pulled onto it by error-offset reconciliation (sync/hsm_reconcile.h) added on top of its muscles' motion. Once a second
// it logs how far the twin's pelvis, hands and blade tip are from the player's own, shifted by the spawn offset, at the
// matching time. Offline and in fight levels only. Game thread.
//   twin on <delayMs> <jitterMs> <lossPct> [inputDelayMs=0] [correct] [offsetUu=250]
//                  inputDelayMs: the input delay the inputs are stamped with (the twin is compared with the player
//                  that much earlier, as both machines would play the same frame at the same match time)
//   twin stats     the totals since "on" (JSON)
//   twin off       destroy the twin
#pragma once
#include "gameplay/adapter.h"
#include "gameplay/standin_rig.h"
#include "hsm_input.h"
#include "sync/hsm_inputline.h"
#include "sync/hsm_linksim.h"
#include "sync/hsm_reconcile.h"
#include "sync/hsm_twin.h"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hsm::game {

class Twin {
public:
    using Drive = std::function<void(InputApplier& in, ue::UObject* willie, float dt)>;   // the session's DriveFighter

    std::string Command(const std::vector<std::string>& args, Adapter& adapter, bool online, bool fightLevel);   // JSON
    void Feed(const InputFrame& f);                     // the player's input frame, at the input rate
    void Tick(Adapter& adapter, float dt, const Drive& drive);   // every game-mode tick, before the physics step
    void OnLevel();                                     // the twin went with the old level
    bool Active() const { return active_; }

private:
    static constexpr uint8_t kEntity = 1;               // any player entity: the batch's
    struct InPacket { std::array<uint8_t, 1200> bytes{}; uint16_t size = 0; };
    void Stop(const char* why);
    double Clock(double nowMs) const { return nowMs - startMs_ + 1000.0; }   // the player's "owner clock", as OwnerStreams'
    bool TruthAt(double ts, sync::PoseFrame& out) const;
    void InstallGuard();
    std::string StatsJson() const;

    bool active_ = false, correct_ = false;
    sync::LinkConfig linkCfg_{};
    double offsetUu_ = 250.0, startMs_ = 0, nextLogMs_ = 0;
    uint16_t inputDelayMs_ = 0;
    sync::V3 offset_{};
    ue::UObject* twin_ = nullptr;
    int32_t twinIndex_ = -1;
    Adapter* adapter_ = nullptr;                        // for the damage guard (FighterOf)
    FighterRig meRig_, twinRig_;
    InputApplier applier_;
    uint32_t seq_ = 0;
    sync::InputBatchSender sender_;
    sync::InputTimeline timeline_;
    sync::Reconciler reconciler_;
    std::unique_ptr<sync::LinkSim<InPacket, 256>> inputLink_;
    std::unique_ptr<sync::LinkSim<sync::PoseFrame, 128>> poseLink_;
    std::deque<sync::PoseFrame> truth_;                 // the player's own frames, unshifted, for the comparison
    std::deque<sync::PoseFrame> pending_;               // poses that arrived ahead of the twin's history (input delay)
    sync::TwinWindow window_, total_;
    uint64_t guardBlocked_ = 0, decodeFailed_ = 0;
};

}  // namespace hsm::game

// Agent command "mirror": the stand-in test bench, no second player needed. The local fighter's own pose goes the
// whole receive path -- FighterRig::Read, EncodePose, a simulated link (delay, jitter, loss), DecodePose, the jitter
// buffer, StandInDriver -- into a neutralised stand-in dressed from the player's own passport, standing offsetUu to
// the player's right. Once a second it logs how far the stand-in stands from the target it was servoed to (pelvis,
// hands, blade tips), how far that buffered target is from the truth it was made from, and the buffer's health.
// Offline and in fight levels only. Game thread.
//   mirror on <delayMs> <jitterMs> <lossPct> [offsetUu=250] [keeptick]
//                      keeptick: let the stand-in's own Willie tick run (A/B against the block)
//   mirror stats       the totals since "on" (JSON)
//   mirror off         destroy the stand-in (with its weapons and armour)
#pragma once
#include "gameplay/adapter.h"
#include "gameplay/standin_rig.h"
#include "sync/hsm_standin.h"
#include "sync/hsm_track.h"
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace hsm::game {

class Mirror {
public:
    std::string Command(const std::vector<std::string>& args, Adapter& adapter, bool online, bool fightLevel);   // JSON
    void Tick(Adapter& adapter, float dt);   // every game-mode tick, before the physics step
    void OnLevel();                          // the stand-in went with the old level
    bool Active() const { return active_; }
    // The stand-in the Willie tick block applies to (nullptr when none, or while "keeptick").
    ue::UObject* TickBlocked() const { return keepTick_ ? nullptr : standIn_; }
    bool IsStandIn(const ue::UObject* willie) const { return willie && willie == standIn_; }

private:
    struct Truth { uint32_t tsMs; sync::V3 pelvis, hand[2]; };
    struct Window {
        sync::ErrorStats pelvis, hands, tip, buffer;
        uint64_t frames = 0, targeted = 0, extrapolated = 0, stale = 0, cuts = 0, snaps = 0, struck = 0, placed = 0;
        double delaySum = 0, j95Max = 0, costSumMs = 0, costMaxMs = 0;
    };
    void Stop(const char* why);
    void Record(const sync::DriverStep& s, const sync::FighterNow& now, const sync::PoseFrame& target, double costMs);
    bool BufferError(const sync::PoseFrame& target, uint32_t playTsMs, double& out) const;
    void LogWindow(const Window& w, const char* label) const;
    std::string StatsJson() const;
    void InstallGuard();

    bool active_ = false, keepTick_ = false;
    sync::LinkConfig link_{};
    double offsetUu_ = 250.0;
    sync::V3 offset_{};                       // fixed at "on": the stand-in stays on the player's right as spawned
    ue::UObject* standIn_ = nullptr;
    int32_t standInIndex_ = -1;
    FighterRig me_, rig_;
    std::unique_ptr<sync::PosePipe> pipe_;
    sync::StandInDriver driver_;
    Adapter* adapter_ = nullptr;             // for the damage guard (FighterOf)
    double startMs_ = 0, nextSendMs_ = 0, nextLogMs_ = 0, bindDeadlineMs_ = 0, prevDtS_ = 0;
    std::deque<Truth> truth_;
    sync::V3 lastTargetPelvis_{};
    bool haveLastTarget_ = false;
    Window window_, total_;
    uint64_t guardBlocked_ = 0;
};

}  // namespace hsm::game

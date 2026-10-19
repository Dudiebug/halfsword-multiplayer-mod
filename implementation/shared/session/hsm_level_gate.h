// Owner sync, client side: when the level's own start-up is done with the fighters, and how often a lost stand-in may
// be re-created. Pure logic; the DLL supplies what it reads from the level.
//
// BP_LevelManager::ReceiveBeginPlay runs Clean Up Map twice (after 0.1 s, then after Level Load Delay as part of Clean
// Up and Spawn Actors). Clean Up Map clears "Player Spawned" and destroys every fighter without the Persistent tag and
// every weapon and armour piece (BP_LevelManager.cpp:132-230); the stock spawner then possesses a fresh player and sets
// "Player Spawned" (BP_SpawnerPoint_Willies.cpp:155). A fighter the mod spawns before that is wiped, so the client
// holds its stand-ins and its own dressed respawn until the gate opens.
#pragma once
#include <cstdint>
#include <map>

namespace hsm {

struct LevelProbe {
    bool manager = false;         // a BP_LevelManager_C exists in the level
    bool playerSpawned = false;   // its "Player Spawned"
    bool pawn = false;            // local player 0 has a fighter
};

class LevelSpawnGate {
public:
    static constexpr double kNoManagerMs = 5000;   // a level without a level manager has no start-up cleanup to wait for
    static constexpr double kMaxWaitMs = 15000;    // never wait longer than this (logged as a timeout)
    enum class Why { Closed, LevelSpawned, NoManager, TimedOut };
    enum class Waiting { Nothing, Manager, PlayerSpawnedCleared, PlayerSpawned, Pawn };

    void OnLevel(double nowMs) { *this = LevelSpawnGate{}; started_ = true; startMs_ = nowMs; }
    // Opens once the level manager's "Player Spawned" has been seen cleared and then set while player 0 has a fighter;
    // or, without a level manager, kNoManagerMs after the level started; or at kMaxWaitMs regardless.
    // Returns true on the update that opens the gate.
    bool Update(double nowMs, const LevelProbe& p) {
        if (why_ != Why::Closed) return false;
        last_ = p;
        if (!started_) { started_ = true; startMs_ = nowMs; }
        const double elapsed = nowMs - startMs_;
        if (p.manager) {
            managerSeen_ = true;
            if (!p.playerSpawned) sawCleared_ = true;
            else if (sawCleared_ && p.pawn) why_ = Why::LevelSpawned;
        }
        if (why_ == Why::Closed && !managerSeen_ && elapsed >= kNoManagerMs) why_ = Why::NoManager;
        if (why_ == Why::Closed && elapsed >= kMaxWaitMs) why_ = Why::TimedOut;
        return why_ != Why::Closed;
    }
    bool Open() const { return why_ != Why::Closed; }
    Why why() const { return why_; }
    // What kept the gate closed as of the last probe, in the order the open rule needs it (for the timeout log).
    Waiting waitingFor() const {
        if (why_ == Why::LevelSpawned || why_ == Why::NoManager) return Waiting::Nothing;
        if (!managerSeen_ || !last_.manager) return Waiting::Manager;
        if (!sawCleared_) return Waiting::PlayerSpawnedCleared;
        if (!last_.playerSpawned) return Waiting::PlayerSpawned;
        if (!last_.pawn) return Waiting::Pawn;
        return Waiting::Nothing;
    }
    const LevelProbe& lastProbe() const { return last_; }      // the last probe; frozen once the gate opens

private:
    bool started_ = false, managerSeen_ = false, sawCleared_ = false;
    double startMs_ = 0;
    Why why_ = Why::Closed;
    LevelProbe last_;
};

// How long a fight level may go without a local fighter before the mod spawns one (EnsureLocalFighter). Returns false
// while it must not spawn one at all, else sets graceMs.
constexpr double kLocalFighterGraceMs = 15000;   // host, solo, host-sync client: longer than the stock spawner's delay
constexpr double kAnnouncedGraceMs = 2000;       // owner-sync client once the level gate is open
inline bool LocalFighterGrace(bool ownerClient, bool gateOpen, double& graceMs) {
    if (ownerClient && !gateOpen) return false;   // the gate's exit spawns the announced fighter (ClientGateOpen)
    graceMs = ownerClient ? kAnnouncedGraceMs : kLocalFighterGraceMs;
    return true;
}

// At most one re-creation attempt per entity per kRetryMs.
class RecreateLimiter {
public:
    static constexpr double kRetryMs = 2000;
    bool Allow(uint8_t entity, double nowMs) {
        auto it = last_.find(entity);
        if (it != last_.end() && nowMs - it->second < kRetryMs) return false;
        last_[entity] = nowMs;
        return true;
    }
    void Clear() { last_.clear(); }

private:
    std::map<uint8_t, double> last_;
};

}  // namespace hsm

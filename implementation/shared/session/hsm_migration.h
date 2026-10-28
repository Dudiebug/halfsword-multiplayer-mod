// Host migration (0.7.0-pre5), pure parts. When a Steam lobby's host leaves, Steam makes another member the lobby owner.
// Instead of everyone going back to the main menu, that member becomes the host of the same lobby, and every other client
// connects to it as it would join: the new host opens the current map again with the lobby's settings (scores start
// over), and the clients follow its Travel. The scores do not start over: every client holds the old host's last
// MatchInfo, and ScoreCarryFrom turns it into the scores by SteamID that the new host's Match::Start takes in. If Steam
// names no new owner in time, or the new host never welcomes us, the client returns to the menu as before.
#pragma once
#include "match/hsm_match.h"
#include "protocol/hsm_protocol.h"
#include <cstdint>

namespace hsm {

constexpr uint32_t kMigrationOwnerWaitMs = 30000;   // Steam drops a crashed host from the lobby only after its own timeout
constexpr uint32_t kMigrationWelcomeMs = 20000;     // following a new host: its Welcome must come within this
constexpr uint32_t kMigrationStartDelayMs = 3000;   // a new host waits this long before opening the map, so most clients
                                                    // are connected and load it once

// The scores of the match the old host ran, by player, from the last MatchInfo a client heard. Empty (nothing to hand over)
// when no match was running or it was over: the rematch starts at zero anyway. Only fighters with a SteamID carry over;
// bots come back with the next wave.
inline ScoreCarry ScoreCarryFrom(const MatchInfo& m) {
    ScoreCarry b;
    const auto phase = MatchPhase(m.phase);
    if (phase == MatchPhase::Idle || phase == MatchPhase::Results || m.format >= kFormatCount) return b;
    b.format = Format(m.format);
    const bool own = b.format == Format::FreeForAll || b.format == Format::Duel;   // scored per entity; else per team
    for (const ScoreEntry& s : m.scores) if (!own) b.teams[s.key] = s.score;
    for (const FighterEntry& f : m.fighters) {
        if (!f.steamId) continue;
        PlayerScore p; p.id = f.steamId; p.kills = f.kills; p.deaths = f.deaths;
        if (own) for (const ScoreEntry& s : m.scores) if (s.key == f.entity) p.score = s.score;
        b.players.push_back(p);
    }
    return b;
}

enum class MigrationStep : uint8_t {
    Idle,         // no migration under way
    WaitOwner,    // the host is gone; Steam has not named a new owner yet
    BecomeHost,   // this machine is the new owner: become the host now (once)
    FollowHost,   // another member is the new owner: connect to it now (once)
    Following,    // connected to the new host; waiting for its Welcome
    Hosting,      // became the host; done
    GiveUp,       // back to the menu (once)
};

class HostMigration {
public:
    // The host left the lobby, or its connection went silent for good. oldHost: its SteamID. Ignored while one is under way.
    void Start(uint64_t oldHost, uint32_t nowMs) {
        if (step_ != MigrationStep::Idle) return;
        step_ = MigrationStep::WaitOwner; oldHost_ = oldHost; since_ = nowMs; newHost_ = 0;
    }
    // Called every tick while active. owner: the lobby's owner as Steam reports it now (0 when unknown); me: our SteamID;
    // inLobby: we are still a member; enabled: host_migration=1. Returns the step to act on; BecomeHost, FollowHost and
    // GiveUp are returned once, then the state moves on (Hosting, Following, Idle).
    MigrationStep Tick(uint64_t owner, uint64_t me, bool inLobby, bool enabled, uint32_t nowMs) {
        switch (step_) {
            case MigrationStep::WaitOwner:
                if (!enabled || !inLobby) return Finish(MigrationStep::GiveUp);
                if (owner && owner != oldHost_) {
                    newHost_ = owner; since_ = nowMs;
                    step_ = owner == me ? MigrationStep::Hosting : MigrationStep::Following;
                    return owner == me ? MigrationStep::BecomeHost : MigrationStep::FollowHost;
                }
                if (nowMs - since_ >= kMigrationOwnerWaitMs) return Finish(MigrationStep::GiveUp);
                return MigrationStep::WaitOwner;
            case MigrationStep::Following:
                if (!inLobby || nowMs - since_ >= kMigrationWelcomeMs) return Finish(MigrationStep::GiveUp);
                if (owner && owner != newHost_) {   // the new host left too: start over from it
                    oldHost_ = newHost_; newHost_ = 0; since_ = nowMs; step_ = MigrationStep::WaitOwner;
                    return MigrationStep::WaitOwner;
                }
                return MigrationStep::Following;
            default:
                return step_;
        }
    }
    // The new host's Welcome arrived: the migration is over.
    void Welcomed() { if (step_ == MigrationStep::Following) step_ = MigrationStep::Idle; }
    // Hosting: the new host's map may open now (kMigrationStartDelayMs after it took over). Returns true once.
    bool StartDue(uint32_t nowMs) {
        if (step_ != MigrationStep::Hosting || nowMs - since_ < kMigrationStartDelayMs) return false;
        step_ = MigrationStep::Idle;
        return true;
    }
    bool Active() const { return step_ != MigrationStep::Idle; }
    MigrationStep step() const { return step_; }
    uint64_t oldHost() const { return oldHost_; }
    uint64_t newHost() const { return newHost_; }
    void Clear() { *this = HostMigration{}; }

private:
    MigrationStep Finish(MigrationStep s) { step_ = MigrationStep::Idle; return s; }
    MigrationStep step_ = MigrationStep::Idle;
    uint64_t oldHost_ = 0, newHost_ = 0;
    uint32_t since_ = 0;
};

}  // namespace hsm

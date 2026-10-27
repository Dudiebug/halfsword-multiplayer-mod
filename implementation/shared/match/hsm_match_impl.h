// Implementation of hsm_match.h (header-only, included at its end).
#pragma once
#include <algorithm>
#include <set>

namespace hsm {

inline MatchConfig Sanitize(MatchConfig c) {
    if (uint8_t(c.format) >= kFormatCount) c.format = Format::FreeForAll;
    c.scoreLimit = std::clamp<uint16_t>(c.scoreLimit, 1, 100);
    c.timeLimitS = std::min<uint16_t>(c.timeLimitS, 3600);
    c.respawnDelayS = std::min<uint16_t>(c.respawnDelayS, 60);
    if (uint8_t(c.gear) >= kGearCount || (c.gear == Gear::Fists && c.format != Format::FreeForAll)) c.gear = Gear::Own;
    c.tier = std::min<uint8_t>(c.tier, 7);
    if (c.gear == Gear::Fists) c.tier = 0;                              // Brawl is tier 0: the lowest build, no gear budget
    return c;
}

inline Regear RegearFor(const MatchConfig& c, RespawnCause cause) {
    if (IsBotFormat(c.format) || c.gear == Gear::Fists) return Regear::None;   // gear is a PvP option; Brawl strips at the spawn instead
    if (c.gear == Gear::RandomTier) return Regear::Each;
    if (c.gear == Gear::Mirror && (cause == RespawnCause::RoundReset || cause == RespawnCause::Rematch)) return Regear::Shared;
    return Regear::None;
}

inline std::map<uint8_t, int8_t> ResolveTeams(const std::vector<TeamRequest>& members, Format f) {
    std::map<uint8_t, int8_t> out;
    int teams = TeamCount(f);
    if (f == Format::FreeForAll || f == Format::Duel || IsBotFormat(f)) {
        for (const TeamRequest& m : members) out[m.slot] = IsBotFormat(f) ? kHumanTeam : kNoTeam;
        return out;
    }
    std::vector<int> size(size_t(teams), 0);
    std::vector<uint8_t> unassigned;
    auto valid = [&](int8_t t) { return t >= 0 && t < teams; };
    std::vector<TeamRequest> sorted = members;
    std::sort(sorted.begin(), sorted.end(), [](const TeamRequest& a, const TeamRequest& b) { return a.slot < b.slot; });
    for (const TeamRequest& m : sorted) {
        int8_t t = valid(m.hostOverride) ? m.hostOverride : valid(m.pick) ? m.pick : kNoTeam;
        if (t == kNoTeam) { unassigned.push_back(m.slot); continue; }
        out[m.slot] = t; ++size[size_t(t)];
    }
    for (uint8_t slot : unassigned) {
        size_t best = size_t(std::min_element(size.begin(), size.end()) - size.begin());   // ties: lowest index
        out[slot] = int8_t(best); ++size[best];
    }
    return out;
}

inline Rgba TeamColor(int team) {
    static const Rgba kPalette[] = {{0.70f, 0.05f, 0.04f, 1}, {0.04f, 0.12f, 0.65f, 1}, {0.06f, 0.45f, 0.08f, 1}, {0.85f, 0.62f, 0.05f, 1}};
    return team >= 0 && team < 4 ? kPalette[team] : Rgba{};
}

inline void KillLog::OnHit(uint8_t victim, uint8_t attacker, uint32_t nowMs) {
    if (attacker == 0 || attacker == victim) return;
    last_[victim] = {attacker, nowMs};
}
inline uint8_t KillLog::KillerOf(uint8_t victim, uint32_t nowMs) const {
    auto it = last_.find(victim);
    return it != last_.end() && nowMs - it->second.at <= kWindowMs ? it->second.attacker : 0;
}
inline void KillLog::Forget(uint8_t entity) { last_.erase(entity); }
inline void KillLog::ForgetAttacker(uint8_t entity) { std::erase_if(last_, [&](const auto& kv) { return kv.second.attacker == entity; }); }

inline void Match::Start(const MatchConfig& cfg, const std::vector<FighterInfo>& fighters, uint32_t nowMs) {
    cfg_ = Sanitize(cfg); fighters_.clear(); score_.clear(); winner_ = kNoWinner; endedPending_ = false; ResetModeState();
    for (const FighterInfo& f : fighters) AddFighter(f);
    phase_ = MatchPhase::Countdown; phaseStart_ = nowMs;
}

inline void Match::AddFighter(const FighterInfo& f) {
    F x; x.team = f.team; x.human = f.human; fighters_[f.entity] = x;
    if (!f.human && spawnOwed_ > 0) --spawnOwed_;
}
inline void Match::RemoveFighter(uint8_t entity) {
    fighters_.erase(entity); skipVotes_.erase(entity); gone_.erase(entity);
    if (cfg_.format == Format::FreeForAll || cfg_.format == Format::Duel) score_.erase(int(entity));   // teams keep their score
}

inline bool Match::Leave(uint8_t entity, uint32_t nowMs) {
    auto it = fighters_.find(entity);
    if (it == fighters_.end() || !CombatPhase(phase_)) return false;
    const bool wasAlive = it->second.alive;
    if (wasAlive) OnDeath(entity, 0, nowMs);
    gone_.insert(entity);
    return wasAlive;
}
inline void Match::Return(uint8_t entity) { gone_.erase(entity); }
inline bool Match::AwaitsRespawn(uint8_t e) const {
    auto it = fighters_.find(e);
    return it != fighters_.end() && it->second.human && !it->second.alive && !gone_.count(e) && CombatPhase(phase_);
}

inline int Match::ScoreKeyOf(uint8_t entity) const { return cfg_.format == Format::FreeForAll || cfg_.format == Format::Duel ? int(entity) : int(TeamOf(entity)); }

inline void Match::AddPoints(int key, int points) { score_[key] += points; }

inline void Match::End(int winnerKey, uint32_t nowMs) {
    winner_ = winnerKey; phase_ = MatchPhase::Results; phaseStart_ = nowMs; endedPending_ = true;
}

inline int Match::UniqueLeader() const {
    std::set<int> keys;                                                // fighters who never scored still count, at 0
    for (const auto& [key, s] : score_) keys.insert(key);
    if (!IsBotFormat(cfg_.format))
        for (const auto& [e, f] : fighters_) keys.insert(ScoreKeyOf(e));
    int best = kNoWinner, bestScore = 0; bool tie = false;
    for (int key : keys) {
        if (IsBotFormat(cfg_.format) && key == kBotTeam) continue;
        int s = Score(key);
        if (best == kNoWinner || s > bestScore) { best = key; bestScore = s; tie = false; }
        else if (s == bestScore) tie = true;
    }
    return tie ? kNoWinner : best;
}

inline void Match::ResolveDuelRound(uint32_t nowMs) {
    if (roundOver_) return;
    int alive = 0; uint8_t last = 0;
    for (const auto& [e, f] : fighters_) if (f.alive) { ++alive; last = e; }
    if (alive > 1) return;
    roundOver_ = true; roundEnd_ = nowMs; roundWinner_ = last;                 // 0: nobody standing, a drawn round
    if (alive == 1) {
        AddPoints(ScoreKeyOf(last), 1);
        if (Score(ScoreKeyOf(last)) >= int(cfg_.scoreLimit)) End(ScoreKeyOf(last), nowMs);
        else if (phase_ == MatchPhase::SuddenDeath) { int leader = UniqueLeader(); if (leader != kNoWinner) End(leader, nowMs); }
    }
}

inline void Match::OnDeath(uint8_t victim, uint8_t killer, uint32_t nowMs) {
    auto it = fighters_.find(victim);
    if (it == fighters_.end() || !it->second.alive) return;          // unknown or already dead (repeat report)
    F& v = it->second;
    // K7: two kills in one host tick (a trade at the score limit): the first one ended the match, the second still counts.
    // Level on the limit, neither wins yet: sudden death, the next kill decides.
    if (phase_ == MatchPhase::Results && endedPending_ && nowMs == phaseStart_ && winner_ != kNoWinner &&
        (cfg_.format == Format::FreeForAll || TeamCount(cfg_.format) > 0) && !IsBotFormat(cfg_.format)) {
        v.alive = false; v.diedAt = nowMs; ++v.deaths;
        auto k = fighters_.find(killer);
        if (killer == victim || k == fighters_.end() || (cfg_.format != Format::FreeForAll && k->second.team == v.team)) return;
        AddPoints(ScoreKeyOf(killer), 1); ++k->second.kills;
        const int key = ScoreKeyOf(killer);
        if (key == winner_) return;
        if (Score(key) > Score(winner_)) winner_ = key;
        else if (Score(key) == Score(winner_)) { phase_ = MatchPhase::SuddenDeath; phaseStart_ = nowMs; winner_ = kNoWinner; endedPending_ = false; }
        return;
    }
    v.alive = false; v.diedAt = nowMs;
    bool scoring = phase_ == MatchPhase::Live || phase_ == MatchPhase::SuddenDeath;
    if (!scoring) return;
    ++v.deaths;
    bool pvb = IsBotFormat(cfg_.format);
    auto kit = fighters_.find(killer);
    if (cfg_.format == Format::Duel) {                     // rounds go to the last one standing, not to kills
        if (kit != fighters_.end() && killer != victim) ++kit->second.kills;
        ResolveDuelRound(nowMs);
        return;
    }
    if (killer == victim) { if (!pvb) AddPoints(ScoreKeyOf(victim), -1); }                    // suicide
    else if (kit != fighters_.end()) {
        bool teamkill = cfg_.format != Format::FreeForAll && kit->second.team == v.team;
        if (teamkill) { if (!pvb) AddPoints(ScoreKeyOf(killer), -1); }
        else if (pvb) { if (kit->second.human) ++kit->second.kills; }   // survival scores waves, not kills
        else { AddPoints(ScoreKeyOf(killer), 1); ++kit->second.kills; }
    }
    if (pvb) {
        bool anyHumanAlive = false;
        for (const auto& [e, f] : fighters_) anyHumanAlive = anyHumanAlive || (f.human && f.alive);
        if (!anyHumanAlive) { End(kBotTeam, nowMs); return; }
    }
    if (pvb) return;                                       // survival is endless: no score limit
    for (const auto& [key, s] : score_) {
        if (s >= int(cfg_.scoreLimit)) { End(key, nowMs); return; }
    }
    if (phase_ == MatchPhase::SuddenDeath) { int leader = UniqueLeader(); if (leader != kNoWinner) End(leader, nowMs); }
}

inline bool Match::Forfeit(uint8_t entity, uint32_t nowMs) {
    if (cfg_.format != Format::Duel || roundOver_) return false;
    if (phase_ != MatchPhase::Live && phase_ != MatchPhase::SuddenDeath) return false;
    if (!Alive(entity)) return false;
    OnDeath(entity, 0, nowMs);                             // as a fall: the round to whoever is left, no kill
    return true;
}

inline bool Match::GiveUpFight(uint8_t entity, uint32_t nowMs) {
    if (!IsBotFormat(cfg_.format) || !CombatPhase(phase_)) return false;
    auto it = fighters_.find(entity);
    if (it == fighters_.end() || !it->second.human || !it->second.alive) return false;
    OnDeath(entity, 0, nowMs);
    return true;
}

inline bool Match::RoundInPlay() const {
    return cfg_.format == Format::Duel && !roundOver_ && (phase_ == MatchPhase::Live || phase_ == MatchPhase::SuddenDeath);
}

inline bool Match::Draw(uint32_t nowMs) {
    if (!RoundInPlay()) return false;
    roundOver_ = true; roundEnd_ = nowMs; roundWinner_ = 0;
    return true;
}

inline bool GiveUpHold::Update(bool down, uint32_t nowMs) {
    if (!down) { holding_ = fired_ = false; progress_ = 0.0f; return false; }
    if (fired_) return false;
    if (!holding_) { holding_ = true; since_ = nowMs; progress_ = 0.0f; return false; }
    uint32_t held = nowMs - since_;
    if (held >= kHoldMs) { fired_ = true; progress_ = 0.0f; return true; }
    progress_ = float(held) / float(kHoldMs);
    return false;
}

inline size_t Match::LoadoutsAhead() const {
    RespawnCause cause;
    if (phase_ == MatchPhase::Results) cause = RespawnCause::Rematch;
    else if (roundOver_ && (phase_ == MatchPhase::Live || phase_ == MatchPhase::SuddenDeath)) cause = RespawnCause::RoundReset;
    else return 0;
    const Regear r = RegearFor(cfg_, cause);
    if (r == Regear::Shared) return 1;
    if (r != Regear::Each) return 0;
    size_t humans = 0;
    for (const auto& [e, f] : fighters_) humans += f.human ? 1 : 0;
    return humans;
}

inline size_t StartLoadoutsAhead(const MatchConfig& c, size_t humans) {
    const Regear r = RegearFor(c, RespawnCause::Rematch);
    if (r == Regear::Shared) return humans ? 1 : 0;
    return r == Regear::Each ? humans : 0;
}

inline void GearPool::Use(uint8_t tier) {
    if (tier == tier_) return;
    blobs_.clear();
    tier_ = tier;
}
inline bool GearPool::Wants(uint8_t tier, size_t ahead) { Use(tier); return blobs_.size() < ahead; }
inline void GearPool::Put(uint8_t tier, std::vector<uint8_t> blob) {
    Use(tier);
    if (!blob.empty()) blobs_.push_back(std::move(blob));
}
inline std::vector<uint8_t> GearPool::Take(uint8_t tier) {
    Use(tier);
    if (blobs_.empty()) return {};
    std::vector<uint8_t> b = std::move(blobs_.front());
    blobs_.erase(blobs_.begin());
    return b;
}

inline bool DropClearWatch::Observe(Format f, uint8_t phase, uint16_t round) {
    const bool same = any_ && f == format_;
    const uint8_t was = phase_;
    any_ = true; format_ = f; phase_ = phase;
    if (!same) seen_ = 0;
    switch (DropClearFor(f)) {
    case DropClear::EachRound: {
        if (round == 0) { seen_ = 0; return false; }
        bool moved = seen_ != 0 && round != seen_;
        seen_ = round;
        return moved;
    }
    case DropClear::EachMatch:
        return same && was == uint8_t(MatchPhase::Results) &&
               (phase == uint8_t(MatchPhase::Countdown) || phase == uint8_t(MatchPhase::Live));
    case DropClear::AfterIdle: break;
    }
    return false;
}

inline std::vector<size_t> LootPicks(size_t pieces, size_t max, uint32_t seed) {
    std::vector<size_t> idx(pieces);
    for (size_t i = 0; i < pieces; ++i) idx[i] = i;
    const size_t n = std::min(pieces, max);
    uint32_t x = seed * 0x9E3779B9u + 0x7F4A7C15u;
    for (size_t i = 0; i < n; ++i) {
        x ^= x >> 16; x *= 0x85EBCA6Bu; x ^= x >> 13; x *= 0xC2B2AE35u; x ^= x >> 16;   // murmur3 finaliser
        std::swap(idx[i], idx[i + x % (pieces - i)]);
    }
    idx.resize(n);
    return idx;
}

inline Match::Events Match::Tick(uint32_t nowMs) {
    Events ev = Advance(nowMs);
    if (!ev.respawn.empty() || ev.rematch) {
        RespawnCause cause = ev.rematch ? RespawnCause::Rematch : ev.roundReset ? RespawnCause::RoundReset
                           : ev.waveCleared ? RespawnCause::WaveCleared : RespawnCause::Individual;
        ev.regear = RegearFor(cfg_, cause);
    }
    return ev;
}

inline Match::Events Match::Advance(uint32_t nowMs) {
    Events ev;
    uint32_t elapsed = nowMs - phaseStart_;
    if (endedPending_) { ev.ended = true; endedPending_ = false; }    // before the rematch branch can return
    if (phase_ == MatchPhase::Countdown && elapsed >= kCountdownMs) {
        phase_ = MatchPhase::Live; phaseStart_ = nowMs; ev.live = true;
        if (IsBotFormat(cfg_.format)) StartWave(nowMs, ev);
    } else if (phase_ == MatchPhase::Live && cfg_.timeLimitS && elapsed >= uint32_t(cfg_.timeLimitS) * 1000u) {
        int leader = UniqueLeader();
        if (leader != kNoWinner) End(leader, nowMs);
        else { phase_ = MatchPhase::SuddenDeath; phaseStart_ = nowMs; }
    } else if (phase_ == MatchPhase::Results && elapsed >= kResultsMs) {
        score_.clear(); winner_ = kNoWinner; ResetModeState();
        std::erase_if(fighters_, [](const auto& kv) { return !kv.second.human; });   // last run's bots are gone
        for (auto& [e, f] : fighters_) { f.alive = true; f.kills = 0; f.deaths = 0; }
        phase_ = MatchPhase::Countdown; phaseStart_ = nowMs; ev.rematch = true;
        return ev;
    }
    if (endedPending_) { ev.ended = true; endedPending_ = false; }    // ended during this Tick (time limit)
    if (phase_ == MatchPhase::Idle || phase_ == MatchPhase::Results) return ev;
    if (cfg_.format == Format::Duel) {
        if (phase_ == MatchPhase::Countdown)                            // F2: a fall before the fight would never see a round reset
            for (auto& [e, f] : fighters_)
                if (!f.alive && f.human && !gone_.count(e) && nowMs - f.diedAt >= 1000u) { f.alive = true; ev.respawn.push_back(e); }
        if (roundOver_ && nowMs - roundEnd_ >= uint32_t(cfg_.respawnDelayS) * 1000u) {
            roundOver_ = false; roundWinner_ = 0; ++round_; ev.roundReset = true;
            for (auto& [e, f] : fighters_) { if (gone_.count(e)) continue; f.alive = true; ev.respawn.push_back(e); }
        }
        if (!roundOver_ && !gone_.empty() && CombatPhase(phase_)) ResolveDuelRound(nowMs);   // the opponent left: the round is the survivor's
        return ev;
    }
    if (IsBotFormat(cfg_.format)) {
        if (phase_ == MatchPhase::Countdown || ev.waveStarted) return ev;   // spawning starts on the next tick
        if (inBreak_) {
            const size_t needed = SkipVotesNeeded();
            if (nowMs - breakStart_ >= BreakMs() || (needed > 0 && skipVotes_.size() >= needed)) StartWave(nowMs, ev);
            return ev;
        }
        int alive = AliveBots();
        if (remaining_ == 0 && spawnOwed_ == 0 && alive == 0) {          // wave cleared: downed players return
            AddPoints(kHumanTeam, 1); ++wave_; ev.waveCleared = true;
            if (phase_ == MatchPhase::SuddenDeath) {                      // past the time limit the wave wins the match
                End(kHumanTeam, nowMs); endedPending_ = false; ev.ended = true;
                return ev;
            }
            inBreak_ = true; breakStart_ = nowMs;
            for (const auto& [e, f] : fighters_) if (f.human && !f.alive && !gone_.count(e)) ev.respawn.push_back(e);
            return ev;
        }
        int room = kMaxBotsAlive - alive - spawnOwed_;
        int n = std::min({remaining_, std::max(room, 0), kBotsPerTick});
        if (n > 0) { remaining_ -= n; spawnOwed_ += n; ev.spawnBots = n; ev.botRank = waveRank_; }
        return ev;
    }
    for (const auto& [e, f] : fighters_)
        if (!f.alive && f.human && !gone_.count(e) && nowMs - f.diedAt >= uint32_t(cfg_.respawnDelayS) * 1000u) ev.respawn.push_back(e);
    return ev;
}

inline void Match::MarkRespawned(uint8_t entity) { auto it = fighters_.find(entity); if (it != fighters_.end()) it->second.alive = true; }

inline void Match::Finish(int winnerKey, uint32_t nowMs) {
    if (phase_ == MatchPhase::Live || phase_ == MatchPhase::SuddenDeath || phase_ == MatchPhase::Countdown) End(winnerKey, nowMs);
}

inline uint32_t Match::RoundGapLeftMs(uint32_t nowMs) const {
    if (!roundOver_ || (phase_ != MatchPhase::Live && phase_ != MatchPhase::SuddenDeath)) return 0;   // the last round has no gap
    uint32_t gap = uint32_t(cfg_.respawnDelayS) * 1000u, elapsed = nowMs - roundEnd_;
    return gap > elapsed ? gap - elapsed : 0;
}

inline uint32_t Match::BreakLeftMs(uint32_t nowMs) const {
    if (!IsBotFormat(cfg_.format) || !inBreak_ || (phase_ != MatchPhase::Live && phase_ != MatchPhase::SuddenDeath)) return 0;
    uint32_t elapsed = nowMs - breakStart_;
    return BreakMs() > elapsed ? BreakMs() - elapsed : 0;
}
inline size_t Match::SkipVotesNeeded() const {
    size_t n = 0;
    for (const auto& [e, f] : fighters_) n += f.human;
    return n;
}
inline bool Match::VoteSkip(uint8_t entity, uint32_t nowMs) {
    auto it = fighters_.find(entity);
    if (BreakLeftMs(nowMs) == 0 || it == fighters_.end() || !it->second.human) return false;
    return skipVotes_.insert(entity).second;
}

inline uint32_t Match::TimeLeftMs(uint32_t nowMs) const {
    uint32_t elapsed = nowMs - phaseStart_, total = 0;
    if (phase_ == MatchPhase::Countdown) total = kCountdownMs;
    else if (phase_ == MatchPhase::Results) total = kResultsMs;
    else if (phase_ == MatchPhase::Live) total = uint32_t(cfg_.timeLimitS) * 1000u;
    return total > elapsed ? total - elapsed : 0;
}

inline Wave WaveFor(Format f, int waveIndex, int humans, int tier) {
    Wave w;
    waveIndex = std::max(waveIndex, 0); humans = std::max(humans, 0); tier = std::clamp(tier, 0, 7);
    if (f == Format::Zombies) {
        w.count = std::min(6 * humans + 4 * waveIndex, 60);
        w.rank = std::min(7, tier + waveIndex / 3);
    } else {
        w.count = std::min(std::max(humans, 1) + waveIndex, 30);
        w.rank = std::min(7, tier + waveIndex / 2);
    }
    w.count = std::max(w.count, 1);
    return w;
}
inline int Match::AliveBots() const {
    int n = 0;
    for (const auto& [e, f] : fighters_) n += !f.human && f.alive;
    return n;
}
inline void Match::SpawnFailed(int n) { n = std::clamp(n, 0, spawnOwed_); spawnOwed_ -= n; remaining_ += n; }
inline int Match::botsLeft() const { return remaining_ + spawnOwed_ + AliveBots(); }
inline std::vector<uint8_t> Match::StaleBotCorpses(uint32_t nowMs, uint32_t ageMs) const {
    std::vector<uint8_t> out;
    for (const auto& [e, f] : fighters_) if (!f.human && !f.alive && nowMs - f.diedAt >= ageMs) out.push_back(e);
    return out;
}
inline void Match::StartWave(uint32_t nowMs, Events& ev) {
    int humans = 0;
    for (const auto& [e, f] : fighters_) humans += f.human;
    Wave w = WaveFor(cfg_.format, wave_, humans, cfg_.tier);
    remaining_ = w.count; waveRank_ = w.rank; spawnOwed_ = 0; inBreak_ = false; breakStart_ = nowMs; ev.waveStarted = true;
    skipVotes_.clear();
}
inline void Match::ResetModeState() {
    wave_ = 0; remaining_ = 0; waveRank_ = 0; spawnOwed_ = 0; inBreak_ = false; clearedPending_ = false; breakStart_ = 0; skipVotes_.clear();
    revive_.clear(); gone_.clear(); roundOver_ = false; roundEnd_ = 0; roundWinner_ = 0; round_ = cfg_.format == Format::Duel ? 1 : 0;
}

inline std::vector<int8_t> PartitionSides(const std::vector<Point3>& points, int teams) {
    std::vector<int8_t> side(points.size(), 0);
    if (teams <= 1 || points.size() < 2) return side;
    double cx = 0, cy = 0;
    for (const Point3& p : points) { cx += p.x; cy += p.y; }
    cx /= double(points.size()); cy /= double(points.size());
    std::vector<std::pair<double, size_t>> byAngle;
    for (size_t i = 0; i < points.size(); ++i) byAngle.push_back({std::atan2(points[i].y - cy, points[i].x - cx), i});
    std::sort(byAngle.begin(), byAngle.end());
    // Start the arcs right after the widest angular gap so each side is one contiguous cluster.
    size_t n = byAngle.size(), start = 0; double widest = -1;
    for (size_t k = 0; k < n; ++k) {
        double gap = k == 0 ? byAngle[0].first + 6.283185307179586 - byAngle[n - 1].first : byAngle[k].first - byAngle[k - 1].first;
        if (gap > widest + 1e-9) { widest = gap; start = k; }
    }
    for (size_t k = 0; k < n; ++k) side[byAngle[(start + k) % n].second] = int8_t(k * size_t(teams) / n);
    return side;
}

inline size_t PickRespawn(const std::vector<Point3>& points, const std::vector<int8_t>& sides, int team, const std::vector<Point3>& enemies) {
    bool teamHasSide = false;
    for (size_t i = 0; i < points.size() && i < sides.size(); ++i) teamHasSide = teamHasSide || sides[i] == team;
    size_t best = 0; double bestScore = -1;
    for (size_t i = 0; i < points.size(); ++i) {
        if (teamHasSide && (i >= sides.size() || sides[i] != team)) continue;
        double nearest = enemies.empty() ? 0 : INFINITY;
        for (const Point3& e : enemies) nearest = std::fmin(nearest, Dist2(points[i], e));
        if (nearest > bestScore) { bestScore = nearest; best = i; }
    }
    return best;
}

}  // namespace hsm

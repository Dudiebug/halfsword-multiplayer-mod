// A client's own pawn while the host keeps that client out of the fight (public #4257). The host gives a benched fighter no
// body, but the client still stands in a pawn of its own, which nothing on the host's side can take away. It goes down once
// (gameplay/session.cpp BenchedBodyTick), so the spectator camera takes over and nothing of it fights. Two benches:
//   - Tournament and Pit Bridge Duels (IsQueueFormat), from the countdown on: everyone not in the pairing being fought. Between two
//     rounds a fighter of the pairing is down too, so the queue's own roles decide (RoleInQueue), not the down flag alone.
//   - Duel: a third player who joins a fought Duel (Match::AddFighter) sits out, down, until the match ends. The wire has no queue
//     for it, and a Duel fighter is down between rounds as well; what tells the bench apart is the body: a benched joiner still has
//     the level's stock pawn, which no Spawn from the host has replaced, while a fighter's pawn was the host's (and a fighter that
//     waits for its next round's body has a dead one).
// Game-independent and unit-tested (match/test_hsm_bench_pawn.cpp).
#pragma once
#include "match/hsm_hud_text.h"
#include "match/hsm_match.h"
#include <cstdint>

namespace hsm {

// m: the host's last MatchInfo; me: this client's entity (0: none yet); hostCountsDown: m has me down (FighterEntry::alive false);
// stockPawn: the pawn I stand in is still the level's, no Spawn from the host has given me my body in this level (and the level's
// start-up is over, so a Spawn would have been applied).
inline bool BenchedPawnGoesDown(const MatchInfo& m, uint8_t me, bool hostCountsDown, bool stockPawn) {
    if (!me || !hostCountsDown) return false;
    const Format f = Format(m.format);
    const MatchPhase ph = MatchPhase(m.phase);
    if (IsQueueFormat(f)) return (ph == MatchPhase::Countdown || CombatPhase(ph)) && RoleInQueue(m, me) != QueueRole::Fighting;
    if (f == Format::Duel) return CombatPhase(ph) && stockPawn;   // the Duel bench is only made while it is fought
    return false;
}

// For the log line.
inline const char* BenchName(Format f) {
    return f == Format::Tournament ? "tournament" : f == Format::PitBridgeDuel ? "pit bridge duels" : f == Format::Duel ? "duel: joined a fought Duel" : "?";
}

}  // namespace hsm

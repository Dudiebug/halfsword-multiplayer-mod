// Match HUD text, built from the wire MatchInfo so host and clients show the same thing. Game-independent.
#pragma once
#include "hsm_protocol.h"
#include "match/hsm_match.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace hsm {

inline std::string TeamName(int team) {
    static const char* kNames[] = {"RED", "BLUE", "GREEN", "GOLD"};
    return team >= 0 && team < 4 ? kNames[team] : "-";
}

inline std::string NameOf(const MatchInfo& m, uint8_t entity) {
    for (const FighterEntry& f : m.fighters) if (f.entity == entity && !f.name.empty()) return f.name;
    return "#" + std::to_string(entity);
}

inline int ScoreOf(const MatchInfo& m, int key) {
    for (const ScoreEntry& s : m.scores) if (s.key == key) return s.score;
    return 0;
}

inline std::string Seconds(uint32_t ms) { return std::to_string((ms + 999) / 1000); }

inline std::string Clock(uint32_t ms) {
    uint32_t s = ms / 1000;
    std::string sec = std::to_string(s % 60);
    return std::to_string(s / 60) + ":" + (sec.size() < 2 ? "0" : "") + sec;
}

inline std::string Upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }

inline std::string TeamScores(const MatchInfo& m) {
    std::string s;
    for (int t = 0; t < TeamCount(Format(m.format)); ++t) s += (t ? "   " : "") + TeamName(t) + " " + std::to_string(ScoreOf(m, t));
    return s;
}

// MatchInfo `m` as of `nowMs`, when it arrived at `receivedAtMs` (tick counts, wrap-safe): its clocks run down by
// the time since, stopping at 0. A receive time ahead of now (a clock that stepped back) counts as no time.
inline MatchInfo Extrapolate(const MatchInfo& m, uint32_t receivedAtMs, uint32_t nowMs) {
    uint32_t elapsed = nowMs - receivedAtMs;
    if (elapsed > 0x80000000u) elapsed = 0;
    MatchInfo e = m;
    e.timeLeftMs = m.timeLeftMs > elapsed ? m.timeLeftMs - elapsed : 0;
    e.roundGapLeftMs = m.roundGapLeftMs > elapsed ? m.roundGapLeftMs - elapsed : 0;
    return e;
}

inline bool InRoundGap(const MatchInfo& m) {
    return Format(m.format) == Format::Duel && m.roundGapLeftMs && (m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath));
}

// The bot formats' break between waves: MatchInfo carries its time left in roundGapLeftMs and the players' votes to
// skip it in roundWinner (Duel-only fields before: older clients ignore them outside Duel).
inline bool InWaveBreak(const MatchInfo& m) {
    return IsBotFormat(Format(m.format)) && m.roundGapLeftMs && (m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath));
}

inline std::string StatusLine(const MatchInfo& m, uint8_t myEntity) {
    Format f = Format(m.format);
    if (InWaveBreak(m)) {                                  // "Wave 2 cleared   next wave in 45   hold G to skip (1/2 ready)"
        int players = 0;
        for (const FighterEntry& e : m.fighters) players += e.team == kHumanTeam;
        return "Wave " + std::to_string(ScoreOf(m, kHumanTeam)) + " cleared   next wave in " + Seconds(m.roundGapLeftMs) +
               "   hold G to skip (" + std::to_string(m.roundWinner) + "/" + std::to_string(players) + " ready)";
    }
    if (InRoundGap(m)) {                                   // "Round 2 to Ann   next round in 3"
        std::string who = !m.roundWinner ? " drawn" : m.roundWinner == myEntity ? " to you" : " to " + NameOf(m, m.roundWinner);
        return "Round " + std::to_string(m.round) + who + "   next round in " + Seconds(m.roundGapLeftMs);
    }
    std::string limit = "(first to " + std::to_string(m.scoreLimit) + ")";
    switch (MatchPhase(m.phase)) {
        case MatchPhase::Idle: return {};
        case MatchPhase::Countdown: return "Match starts in " + Seconds(m.timeLeftMs);
        case MatchPhase::Results: {
            std::string who;
            if (f == Format::Zombies) who = "OVERRUN - survived " + std::to_string(ScoreOf(m, kHumanTeam)) + " waves";
            else if (f == Format::PlayersVsBots) who = m.winner == kBotTeam ? "BOTS WIN" : "PLAYERS WIN";
            else if (f == Format::FreeForAll || f == Format::Duel) who = m.winner == int(myEntity) ? "YOU WIN" : Upper(NameOf(m, uint8_t(m.winner))) + " WINS";
            else who = TeamName(m.winner) + " WINS";
            return who + "   rematch in " + Seconds(m.timeLeftMs);
        }
        case MatchPhase::SuddenDeath:
            return "SUDDEN DEATH   " + (f == Format::FreeForAll ? std::string("next kill wins") : TeamScores(m));
        case MatchPhase::Live: break;
    }
    std::string s;
    if (IsBotFormat(f)) {
        int cleared = ScoreOf(m, kHumanTeam);
        s = "Wave " + std::to_string(cleared + 1) + "   " + std::to_string(cleared) + " cleared";
    } else if (f == Format::FreeForAll || f == Format::Duel) {
        int mine = ScoreOf(m, myEntity), best = 0; uint8_t rival = 0;
        for (const ScoreEntry& e : m.scores) if (e.key != myEntity && (rival == 0 || e.score > best)) { best = e.score; rival = uint8_t(e.key); }
        if (f == Format::Duel && !rival)
            for (const FighterEntry& e : m.fighters) if (e.entity != myEntity) { rival = e.entity; break; }
        std::string scores = "You " + std::to_string(mine) + (rival ? "   " + NameOf(m, rival) + " " + std::to_string(best) : "");
        s = f == Format::Duel ? "Rounds   " + scores + "   (best of " + std::to_string(2 * m.scoreLimit - 1) + ")" : scores + "   " + limit;
    } else s = TeamScores(m) + "   " + limit;
    if (m.timeLeftMs) s += "   " + Clock(m.timeLeftMs);
    return s;
}

// The line under a dead player. Duel: the round is over and everybody returns together after the gap; the bot
// formats: the downed return when the wave is cleared; otherwise the respawn delay counts down.
inline std::string RespawnLine(const MatchInfo& m, uint32_t sinceDeathMs) {
    if (m.phase == uint8_t(MatchPhase::Idle) || m.phase == uint8_t(MatchPhase::Results)) return {};
    Format f = Format(m.format);
    if (f == Format::Duel) return m.roundGapLeftMs ? "Round over   next round in " + Seconds(m.roundGapLeftMs) : "Round over";
    if (IsBotFormat(f)) return "Down   back next wave";
    uint32_t delay = uint32_t(m.respawnDelayS) * 1000u;
    return sinceDeathMs < delay ? "Respawning in " + Seconds(delay - sinceDeathMs) : "Respawning...";
}

inline std::string KillLine(const MatchInfo& m, const Kill& k) {
    std::string victim = NameOf(m, k.victim);
    if (k.flags & 8) return victim + " gave up";
    if (k.flags & 2) return victim + " killed themselves";
    if ((k.flags & 4) || k.killer == 0) return victim + " died";
    return NameOf(m, k.killer) + ((k.flags & 1) ? " killed teammate " : " killed ") + victim;
}

inline std::string Scoreboard(const MatchInfo& m) {
    std::vector<FighterEntry> rows = m.fighters;
    std::stable_sort(rows.begin(), rows.end(), [](const FighterEntry& a, const FighterEntry& b) { return a.kills > b.kills; });
    std::string s = "Player            Team     K / D\n";
    for (const FighterEntry& f : rows) {
        std::string name = f.name.empty() ? "#" + std::to_string(f.entity) : f.name;
        name.resize(18, ' ');
        std::string team = TeamName(f.team); team.resize(9, ' ');
        s += name + team + std::to_string(f.kills) + " / " + std::to_string(f.deaths) + "\n";
    }
    return s;
}

// Lobby panel labels and cycling.
inline std::string FormatName(Format f) {
    switch (f) {
        case Format::TwoTeams: return "2 teams";
        case Format::ThreeTeams: return "3 teams";
        case Format::PlayersVsBots: return "Survival";
        case Format::Zombies: return "Zombies";
        case Format::Duel: return "Duel";
        default: return "Free-for-all";
    }
}
// The value after `cur` in `options`, wrapping; the first option when `cur` is not one of them.
inline int NextOf(const std::vector<int>& options, int cur) {
    if (options.empty()) return cur;
    auto it = std::find(options.begin(), options.end(), cur);
    return it == options.end() || it + 1 == options.end() ? options.front() : *(it + 1);
}
// Auto -> team 0 -> ... -> last team -> Auto.
inline int8_t NextTeamPick(int8_t cur, int teams) { return cur + 1 >= 0 && cur + 1 < teams ? int8_t(cur + 1) : kNoTeam; }
inline std::string TeamLabel(int team) {
    static const char* kNames[] = {"Red", "Blue", "Green", "Gold"};
    return team >= 0 && team < 4 ? kNames[team] : "Auto";
}
// The lobby's gear setting, with when it changes (RegearFor). The bot formats have no gear option: own loadouts.
inline std::string GearLabel(Gear g, Format f) {
    if (IsBotFormat(f)) return "Own loadouts";
    if (g == Gear::RandomTier) return f == Format::Duel ? "Random by tier, new each round" : "Random by tier, new each life";
    if (g == Gear::Mirror) return f == Format::Duel ? "Everyone identical, new each round" : "Everyone identical";
    return "Own loadouts";
}
inline std::string TimeLabel(uint16_t seconds) { return seconds ? std::to_string(seconds / 60) + " min" : "off"; }

}  // namespace hsm

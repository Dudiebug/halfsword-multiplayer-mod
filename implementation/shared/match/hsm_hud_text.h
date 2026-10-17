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

inline std::string StatusLine(const MatchInfo& m, uint8_t myEntity) {
    Format f = Format(m.format);
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
            return "SUDDEN DEATH   " + (f == Format::FreeForAll ? std::string("next kill wins") : f == Format::Duel ? std::string("next round wins") : TeamScores(m));
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

inline std::string KillLine(const MatchInfo& m, const Kill& k) {
    std::string victim = NameOf(m, k.victim);
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
inline std::string TimeLabel(uint16_t seconds) { return seconds ? std::to_string(seconds / 60) + " min" : "off"; }

}  // namespace hsm

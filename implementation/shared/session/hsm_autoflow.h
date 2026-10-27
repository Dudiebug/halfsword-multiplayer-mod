// role=server: the lobby-less match loop. Idle until server_min_players have joined, a countdown with chat
// announcements, then a map from the rotation, the match, a results hold, the next map. Back to the menu when the server
// has been empty for a while. Game-independent, unit-tested; session.cpp feeds it the clock and acts on its answers.
#pragma once
#include "match/hsm_maps.h"
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hsm {

// server_format -> Format ("teams" = two teams, "survival" = players vs bots). False for an unknown word.
inline bool ParseServerFormat(std::string_view s, Format* out) {
    struct E { const char* name; Format f; };
    static constexpr E kF[] = {{"ffa", Format::FreeForAll}, {"duel", Format::Duel}, {"teams", Format::TwoTeams},
                               {"survival", Format::PlayersVsBots}, {"zombies", Format::Zombies}};
    for (const E& e : kF)
        if (map_detail::EqualsNoCase(s, e.name)) { *out = e.f; return true; }
    return false;
}

namespace flow_detail {
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}
inline std::string NoSpaces(std::string_view s) { std::string o; for (char c : s) if (c != ' ') o += c; return o; }
inline int MapByName(std::string_view n) {
    const std::string flat = NoSpaces(n);
    for (size_t i = 0; i < MapCount(); ++i)
        if (map_detail::EqualsNoCase(NoSpaces(kMaps[i].display), flat)) return int(i);
    if (const MapInfo* m = FindMap(n)) return int(m - kMaps);
    return -1;
}
}  // namespace flow_detail

// server_maps -> indices into kMaps, in the listed order ("all" = catalogue order). Names are display names, short names
// or paths, any case; maps the format refuses are dropped; unknown names go to *unknown. Nothing usable left (or an
// empty spec): every map the format allows.
inline std::vector<size_t> ParseServerMaps(std::string_view spec, Format f, std::vector<std::string>* unknown = nullptr) {
    std::vector<size_t> out;
    auto add = [&](size_t i) {
        if (!MapAllowsFormat(kMaps[i], f)) return;
        for (size_t j : out) if (j == i) return;
        out.push_back(i);
    };
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t comma = spec.find(',', pos);
        if (comma == std::string_view::npos) comma = spec.size();
        const std::string_view tok = flow_detail::Trim(spec.substr(pos, comma - pos));
        pos = comma + 1;
        if (tok.empty()) continue;
        if (map_detail::EqualsNoCase(tok, "all")) { for (size_t i = 0; i < MapCount(); ++i) add(i); continue; }
        const int i = flow_detail::MapByName(tok);
        if (i < 0) { if (unknown) unknown->push_back(std::string(tok)); continue; }
        add(size_t(i));
    }
    if (out.empty()) for (size_t i = 0; i < MapCount(); ++i) add(i);
    return out;
}

struct AutoFlowConfig {
    int minPlayers = 2;
    uint32_t countdownMs = 30000;
    uint32_t announceMs = 10000;      // chat cadence of the countdown
    uint32_t resultsHoldMs = 15000;   // fallback: the match's own results phase (10 s) normally ends it through ResultsOver()
    uint32_t emptyGraceMs = 10000;    // nobody connected this long: back to the menu
    uint32_t travelRetryMs = 20000;   // no match on the new map by then: ask again
};

enum class FlowPhase : uint8_t { None = 0, Active = 1, Results = 2 };   // the session's Match: no match / countdown..live / results
struct FlowInput { uint32_t nowMs = 0; int clients = 0; FlowPhase phase = FlowPhase::None; };
struct FlowAction {
    enum Kind : uint8_t { None, Announce, Travel, ToMenu } kind = None;
    int secondsLeft = 0;   // Announce
    size_t map = 0;        // Travel: index into kMaps
};

class AutoFlow {
public:
    AutoFlow(const AutoFlowConfig& cfg, std::vector<size_t> maps) : cfg_(cfg), maps_(std::move(maps)) {}

    enum class State : uint8_t { Idle, Waiting, Traveling, InMatch, Results };
    State state() const { return state_; }

    // The match's results phase ended: the next map comes up on the next Tick.
    void ResultsOver() { if (state_ == State::Results) resultsOver_ = true; }

    FlowAction Tick(const FlowInput& in) {
        if (maps_.empty()) return {};
        if (in.clients <= 0) {
            if (!empty_) { empty_ = true; emptySince_ = in.nowMs; }
            if (state_ != State::Idle && in.nowMs - emptySince_ >= cfg_.emptyGraceMs) {
                const bool menu = onMap_;
                state_ = State::Idle; onMap_ = false; resultsOver_ = false;
                if (menu) return {FlowAction::ToMenu};
            }
            if (state_ == State::Waiting) state_ = State::Idle;
            return {};
        }
        empty_ = false;
        switch (state_) {
            case State::Idle:
                if (in.clients < cfg_.minPlayers) return {};
                state_ = State::Waiting; waitStart_ = lastAnnounce_ = in.nowMs;
                if (cfg_.countdownMs == 0) return Begin(in.nowMs);
                return {FlowAction::Announce, SecondsLeft(in.nowMs)};
            case State::Waiting: {
                if (in.clients < cfg_.minPlayers) { state_ = State::Idle; return {}; }
                if (in.nowMs - waitStart_ >= cfg_.countdownMs) return Begin(in.nowMs);
                if (in.nowMs - lastAnnounce_ >= cfg_.announceMs) { lastAnnounce_ = in.nowMs; return {FlowAction::Announce, SecondsLeft(in.nowMs)}; }
                return {};
            }
            case State::Traveling:
                if (in.phase == FlowPhase::Active) { state_ = State::InMatch; return {}; }
                if (in.nowMs - travelAt_ >= cfg_.travelRetryMs) { travelAt_ = in.nowMs; return {FlowAction::Travel, 0, maps_[(next_ + maps_.size() - 1) % maps_.size()]}; }
                return {};
            case State::InMatch:
                if (in.phase == FlowPhase::Results) { state_ = State::Results; resultsAt_ = in.nowMs; resultsOver_ = false; }
                else if (in.phase == FlowPhase::None) { state_ = State::Waiting; waitStart_ = lastAnnounce_ = in.nowMs; }   // the match was torn down
                return {};
            case State::Results:
                if (resultsOver_ || in.nowMs - resultsAt_ >= cfg_.resultsHoldMs) { resultsOver_ = false; return Begin(in.nowMs); }
                return {};
        }
        return {};
    }

private:
    int SecondsLeft(uint32_t now) const {
        const uint32_t el = now - waitStart_;
        return el >= cfg_.countdownMs ? 0 : int((cfg_.countdownMs - el + 999) / 1000);
    }
    FlowAction Begin(uint32_t now) {
        state_ = State::Traveling; travelAt_ = now; onMap_ = true;
        const size_t m = maps_[next_ % maps_.size()];
        next_ = (next_ + 1) % maps_.size();
        return {FlowAction::Travel, 0, m};
    }
    AutoFlowConfig cfg_;
    std::vector<size_t> maps_;
    State state_ = State::Idle;
    size_t next_ = 0;
    uint32_t waitStart_ = 0, lastAnnounce_ = 0, travelAt_ = 0, resultsAt_ = 0, emptySince_ = 0;
    bool empty_ = false, onMap_ = false, resultsOver_ = false;
};

}  // namespace hsm

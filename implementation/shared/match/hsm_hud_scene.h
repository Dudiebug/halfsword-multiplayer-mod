// Match HUD scene (MATCH_ROUNDS_UI_CHAT_PLAN 3.2): what the in-fight HUD shows, built from the wire MatchInfo, this
// machine's state and the time, so host and clients show the same thing. The client maps the scene to widgets and
// decides nothing; animations are functions of age. Game-independent.
//   score strip   top centre: you and the best rival, one block per team, or survivors against bots
//   time bar      under it, with the clock, while a time limit runs
//   pill          the phase: ROUND 3, FINAL ROUND, SUDDEN DEATH, WAVE 4 · 2 LEFT (FIGHT · 2 LEFT in a Progression
//                 fight), the winner at results
//   banner        centre: the countdown digits, or the last event (FIGHT, ROUND TO ANN, NEW GEAR, BLUE WINS, ...)
//   dead panel    lower third while this machine's fighter is down
//   kill feed     top right: the newest four rows, each fading on its own
//   scoreboard    on Tab, and at results
#pragma once
#include "match/hsm_hud_style.h"
#include "match/hsm_hud_text.h"
#include <algorithm>
#include <string>
#include <vector>

namespace hsm {

// ---------------------------------------------------------------- animation
struct Look { float opacity = 1, scale = 1; };

// A banner pops from 1.25x to 1x over kBannerPopMs, holds until kBannerFadeAtMs, then fades out by kBannerEndMs.
inline constexpr uint32_t kBannerPopMs = 250, kBannerFadeAtMs = 1750, kBannerEndMs = 2150;
inline Look BannerLook(uint32_t ageMs) {
    if (ageMs >= kBannerEndMs) return {0, 1};
    Look l;
    if (ageMs < kBannerPopMs) l.scale = 1.25f - 0.25f * float(ageMs) / float(kBannerPopMs);
    if (ageMs > kBannerFadeAtMs) l.opacity = 1 - float(ageMs - kBannerFadeAtMs) / float(kBannerEndMs - kBannerFadeAtMs);
    return l;
}

// A kill-feed row shows for kFeedShowMs, then fades over kFeedFadeMs. Rows not naming this machine's fighter are
// dimmer.
inline constexpr uint32_t kFeedShowMs = 5000, kFeedFadeMs = 1000;
inline constexpr size_t kFeedRows = 4;
inline constexpr float kFeedOthersOpacity = 0.8f;
inline float FeedOpacity(uint32_t ageMs) {
    if (ageMs <= kFeedShowMs) return 1;
    if (ageMs >= kFeedShowMs + kFeedFadeMs) return 0;
    return 1 - float(ageMs - kFeedShowMs) / float(kFeedFadeMs);
}

// Time since `atMs` (tick counts, wrap-safe); a time ahead of now (a clock that stepped back) counts as no time.
inline uint32_t AgeMs(uint32_t atMs, uint32_t nowMs) {
    uint32_t age = nowMs - atMs;
    return age > 0x80000000u ? 0 : age;
}

// ---------------------------------------------------------------- names and inks
inline const FighterEntry* FighterOf(const MatchInfo& m, uint8_t entity) {
    for (const FighterEntry& f : m.fighters) if (f.entity == entity) return &f;
    return nullptr;
}

// A fighter's name as text: team formats, its team's ink; the bot formats, bots in blood and players in bone;
// free-for-all and Duel, bone.
inline Rgba FighterInk(const MatchInfo& m, uint8_t entity) {
    const FighterEntry* f = FighterOf(m, entity);
    if (!f) return style::kBone;
    Format fmt = Format(m.format);
    if (IsBotFormat(fmt)) return f->team == kBotTeam ? style::kBloodInk : style::kBone;
    if (fmt == Format::TwoTeams || fmt == Format::ThreeTeams) return style::TeamInk(f->team);
    return style::kBone;
}

// Free-for-all and Duel: the other fighter with the best score, else (nobody else has scored) the first other
// fighter; 0 when alone.
inline uint8_t TopRival(const MatchInfo& m, uint8_t me) {
    uint8_t rival = 0; int best = 0;
    for (const ScoreEntry& e : m.scores)
        if (e.key != me && e.key > 0 && e.key < 256 && (rival == 0 || e.score > best)) { best = e.score; rival = uint8_t(e.key); }
    if (!rival)
        for (const FighterEntry& f : m.fighters) if (f.entity != me) return f.entity;
    return rival;
}

// Duel: the round being fought decides the match (both leaders one round from winning).
inline bool FinalRound(const MatchInfo& m) {
    int first = 0, second = 0;
    for (const ScoreEntry& e : m.scores) {
        if (e.score > first) { second = first; first = e.score; }
        else if (e.score > second) second = e.score;
    }
    return first == m.scoreLimit - 1 && second == m.scoreLimit - 1;
}

inline std::string RoundName(const MatchInfo& m) { return FinalRound(m) ? "FINAL ROUND" : "ROUND " + std::to_string(m.round); }

// ---------------------------------------------------------------- banners
struct BannerShot { std::string text, sub; Rgba color = style::kBone; uint32_t atMs = 0; };

// The results banner (and the pill while results show).
inline BannerShot ResultShot(const MatchInfo& m, uint8_t me) {
    Format f = Format(m.format);
    if (f == Format::Zombies) return {"OVERRUN", "SURVIVED " + std::to_string(ScoreOf(m, kHumanTeam)) + " WAVES", style::kBloodInk};
    if (m.winner == Match::kNoWinner) return {"DRAW", "", style::kIron};
    if (f == Format::PlayersVsBots) return m.winner == kBotTeam ? BannerShot{"BOTS WIN", "", style::kBloodInk} : BannerShot{"PLAYERS WIN", "", style::kOk};
    if (f == Format::FreeForAll || f == Format::Duel)
        return m.winner == int(me) ? BannerShot{"YOU WIN", "", style::kWarn} : BannerShot{Upper(NameOf(m, uint8_t(m.winner))) + " WINS", "", style::kBloodInk};
    return {TeamName(m.winner) + " WINS", "", style::TeamInk(m.winner)};
}

// Fires banners from successive MatchInfo (feed it each one as it is made or arrives): FIGHT when the countdown
// ends and when a Duel round starts, ROUND TO X when a Duel round is taken, SUDDEN DEATH, WAVE CLEARED, the
// winner at results. The session fires the rest (NEW GEAR). One at a time: the newest wins. The first MatchInfo,
// and the first of another format, only record: joining a running match fires nothing.
class BannerTracker {
public:
    void Observe(const MatchInfo& m, uint8_t me, uint32_t nowMs) {
        bool gap = m.roundGapLeftMs > 0;
        int cleared = ScoreOf(m, kHumanTeam);
        if (seen_ && m.format == format_) {
            Format f = Format(m.format);
            MatchPhase p = MatchPhase(m.phase), prev = MatchPhase(phase_);
            bool fighting = p == MatchPhase::Live || p == MatchPhase::SuddenDeath;
            if (p != prev) {
                if (p == MatchPhase::Live && prev == MatchPhase::Countdown) Fire("FIGHT", f == Format::Duel ? RoundName(m) : "", style::kBloodInk, nowMs);
                else if (p == MatchPhase::SuddenDeath)
                    Fire("SUDDEN DEATH", f == Format::FreeForAll ? "NEXT KILL WINS" : f == Format::Duel ? "NEXT ROUND WINS"
                                         : IsBotFormat(f) ? "CLEAR THE WAVE TO WIN" : "NEXT POINT WINS", style::kBloodInk, nowMs);
                else if (p == MatchPhase::Results) { shot_ = ResultShot(m, me); shot_.atMs = nowMs; }
            } else if (fighting && f == Format::Duel) {
                if (gap && !gap_) {
                    if (!m.roundWinner) Fire("ROUND DRAWN", "ROUND " + std::to_string(m.round), style::kIron, nowMs);
                    else if (m.roundWinner == me) Fire("ROUND TO YOU", "ROUND " + std::to_string(m.round), style::kOk, nowMs);
                    else Fire("ROUND TO " + Upper(NameOf(m, m.roundWinner)), "ROUND " + std::to_string(m.round), style::kBloodInk, nowMs);
                } else if (m.round > round_) Fire("FIGHT", RoundName(m), style::kBloodInk, nowMs);
            } else if (fighting && IsBotFormat(f) && cleared > cleared_) {
                Fire("WAVE CLEARED", std::to_string(cleared) + " CLEARED", style::kOk, nowMs);
            }
        }
        seen_ = true; format_ = m.format; phase_ = m.phase; round_ = m.round; gap_ = gap; cleared_ = cleared;
    }
    void Fire(std::string text, std::string sub, Rgba color, uint32_t nowMs) { shot_ = {std::move(text), std::move(sub), color, nowMs}; }
    const BannerShot& shot() const { return shot_; }
    void Reset() { *this = BannerTracker{}; }

private:
    BannerShot shot_;
    bool seen_ = false, gap_ = false;
    uint8_t format_ = 0, phase_ = 0;
    uint16_t round_ = 0;
    int cleared_ = 0;
};

// ---------------------------------------------------------------- kill feed
struct Span { std::string text; Rgba color = style::kBone; };
struct FeedRow { std::vector<Span> spans; bool mine = false; };   // mine: names this machine's fighter
struct FeedItem { uint32_t atMs = 0; FeedRow row; };

// The words of KillLine, the names in their inks and the rest in iron. Made when the kill arrives, so a fighter
// who leaves later keeps their name in the feed.
inline FeedRow MakeFeedRow(const MatchInfo& m, const Kill& k, uint8_t me) {
    FeedRow r;
    r.mine = k.victim == me || (k.killer == me && k.killer != 0);
    Span victim{NameOf(m, k.victim), FighterInk(m, k.victim)};
    if (k.flags & (16 | 32)) {
        if (k.killer == 0) r.spans = {victim, {DrawnWhy(k), style::kIron}};
        else r.spans = {victim, {" and ", style::kIron}, {NameOf(m, k.killer), FighterInk(m, k.killer)}, {DrawnWhy(k), style::kIron}};
    } else if (k.flags & 8) r.spans = {victim, {" gave up", style::kIron}};
    else if (k.flags & 2) r.spans = {victim, {" killed themselves", style::kIron}};
    else if ((k.flags & 4) || k.killer == 0) r.spans = {victim, {" died", style::kIron}};
    else r.spans = {{NameOf(m, k.killer), FighterInk(m, k.killer)}, {(k.flags & 1) ? " killed teammate " : " killed ", style::kIron}, victim};
    return r;
}

// ---------------------------------------------------------------- the scene
struct LocalHud {
    uint8_t me = 0;                  // this machine's fighter
    uint8_t host = 1;                // the host's fighter (HOST badge)
    uint32_t infoAtMs = 0;           // when the MatchInfo was made (host) or arrived (client): its clocks run on
    bool dead = false;
    uint32_t diedAtMs = 0;
    bool tab = false;                // the scoreboard key is held
    std::string map;                 // display name, for the scoreboard title
    bool oneFight = false;           // a Progression fight (Free Mode off): the host plays it as one fight, no waves
    BannerShot banner;               // BannerTracker::shot()
    std::vector<FeedItem> feed;      // oldest first
    float giveUp = 0;                // Duel / wave break: how far the G hold is (GiveUpHold::progress), 0 = not held
};

struct SceneText { bool visible = false; std::string text; Rgba color = style::kBone; float opacity = 1, scale = 1; };
struct ScoreBlock { std::string label; int score = 0; Rgba color = style::kBone; bool mine = false; };
struct FeedView { FeedRow row; float opacity = 1; };
struct DeadPanel { bool visible = false; std::string title, detail, hint; bool bar = false; float barFrac = 0; };
struct BoardRow {
    enum class Kind : uint8_t { Band, Fighter };
    Kind kind = Kind::Fighter;
    std::string name;
    Rgba swatch = style::kIron, ink = style::kBone;
    int kills = 0, deaths = 0, score = 0;
    uint16_t pingMs = 0;
    bool host = false, local = false, dead = false;
    bool down = false;               // alive, and the host's draw check has it down (Duel)
};
struct Board { bool visible = false; std::string title, footer; std::vector<BoardRow> rows; };

struct HudScene {
    bool visible = false;
    std::vector<ScoreBlock> strip;
    bool timeBar = false; float timeFrac = 0; std::string clock;
    SceneText pill, banner, bannerSub;
    DeadPanel dead;
    std::vector<FeedView> feed;
    Board board;
};

namespace hud_detail {
inline bool TeamFormat(Format f) { return f == Format::TwoTeams || f == Format::ThreeTeams; }

inline std::vector<ScoreBlock> Strip(const MatchInfo& m, uint8_t me) {
    Format f = Format(m.format);
    const FighterEntry* mine = FighterOf(m, me);
    if (IsBotFormat(f)) {
        int humans = 0, bots = 0;
        for (const FighterEntry& e : m.fighters) if (e.alive) ++(e.team == kBotTeam ? bots : humans);
        return {{"SURVIVORS", humans, style::kBone, true}, {"BOTS", bots, style::kBloodInk, false}};
    }
    std::vector<ScoreBlock> s;
    if (TeamFormat(f)) {
        for (int t = 0; t < TeamCount(f); ++t) s.push_back({TeamName(t), ScoreOf(m, t), style::TeamInk(t), mine && mine->team == t});
        return s;
    }
    s.push_back({"YOU", ScoreOf(m, me), style::kBone, true});
    if (uint8_t rival = TopRival(m, me)) s.push_back({Upper(NameOf(m, rival)), ScoreOf(m, rival), style::kIron, false});
    return s;
}

inline SceneText Pill(const MatchInfo& m, uint8_t me, bool oneFight) {
    Format f = Format(m.format);
    SceneText p; p.visible = true;
    switch (MatchPhase(m.phase)) {
        case MatchPhase::Idle: return {};
        case MatchPhase::Countdown: p.text = "MATCH STARTS IN"; return p;
        case MatchPhase::Results: { BannerShot r = ResultShot(m, me); p.text = r.text; p.color = r.color; return p; }
        default: break;
    }
    if (InRoundGap(m)) { p.text = "NEXT ROUND IN " + Seconds(m.roundGapLeftMs); return p; }
    if (InWaveBreak(m)) { p.text = "NEXT WAVE IN " + Seconds(m.roundGapLeftMs); return p; }
    if (m.phase == uint8_t(MatchPhase::SuddenDeath)) { p.text = "SUDDEN DEATH"; p.color = style::kBloodInk; return p; }
    if (f == Format::Duel) { p.text = RoundName(m); if (FinalRound(m)) p.color = style::kWarn; return p; }
    if (IsBotFormat(f)) {
        int bots = 0;
        for (const FighterEntry& e : m.fighters) if (e.team == kBotTeam && e.alive) ++bots;
        std::string left = bots ? " \xC2\xB7 " + std::to_string(bots) + " LEFT" : "";
        if (oneFight) p.text = "FIGHT" + left;                    // a Progression fight has no waves
        else p.text = "WAVE " + std::to_string(ScoreOf(m, kHumanTeam) + 1) + left;
        return p;
    }
    p.text = "FIRST TO " + std::to_string(m.scoreLimit);
    return p;
}

inline DeadPanel Dead(const MatchInfo& m, const LocalHud& l, uint32_t nowMs) {
    DeadPanel d;
    if (!l.dead || (m.phase != uint8_t(MatchPhase::Live) && m.phase != uint8_t(MatchPhase::SuddenDeath))) return d;
    d.visible = true;
    Format f = Format(m.format);
    if (f == Format::Duel) {                     // the round is over; everybody returns together after the gap
        d.title = "ROUND OVER";
        if (m.roundGapLeftMs) {
            d.detail = "next round in " + Seconds(m.roundGapLeftMs);
            uint32_t gap = uint32_t(m.respawnDelayS) * 1000u;
            d.bar = true; d.barFrac = gap ? std::clamp(float(m.roundGapLeftMs) / float(gap), 0.f, 1.f) : 0.f;
        }
    } else if (IsBotFormat(f)) {
        d.title = "DOWN"; d.detail = l.oneFight ? "out until the fight ends" : "back next wave";
    } else {
        d.title = "RESPAWNING";
        uint32_t delay = uint32_t(m.respawnDelayS) * 1000u, since = AgeMs(l.diedAtMs, nowMs);
        if (since < delay) { d.detail = "in " + Seconds(delay - since); d.bar = true; d.barFrac = float(delay - since) / float(delay); }
    }
    for (const FighterEntry& e : m.fighters)
        if (e.entity != l.me && e.alive) { d.hint = "LEFT / RIGHT   watch another fighter"; break; }
    return d;
}

inline std::vector<FeedView> Feed(const LocalHud& l, uint32_t nowMs) {
    std::vector<FeedView> v;
    for (const FeedItem& it : l.feed) {
        uint32_t age = AgeMs(it.atMs, nowMs);
        if (age >= kFeedShowMs + kFeedFadeMs) continue;
        v.push_back({it.row, FeedOpacity(age) * (it.row.mine ? 1.f : kFeedOthersOpacity)});
    }
    if (v.size() > kFeedRows) v.erase(v.begin(), v.end() - kFeedRows);
    return v;
}

inline Board MakeBoard(const MatchInfo& m, const LocalHud& l) {
    Board b;
    if (m.phase == uint8_t(MatchPhase::Idle) || (!l.tab && m.phase != uint8_t(MatchPhase::Results))) return b;
    b.visible = true;
    Format f = Format(m.format);
    std::string limit = f == Format::Duel ? "BEST OF " + std::to_string(2 * m.scoreLimit - 1)
                      : IsBotFormat(f) ? "WAVE " + std::to_string(ScoreOf(m, kHumanTeam) + 1)
                      : "FIRST TO " + std::to_string(m.scoreLimit);
    bool oneFight = l.oneFight && IsBotFormat(f);               // Progression: back to the Tavern after the results
    b.title = (oneFight ? std::string("PROGRESSION FIGHT") : Upper(FormatName(f)) + " \xC2\xB7 " + limit)
            + (l.map.empty() ? "" : " \xC2\xB7 " + Upper(l.map));
    if (m.phase == uint8_t(MatchPhase::Results)) b.footer = (oneFight ? "BACK TO THE TAVERN IN " : "REMATCH IN ") + Seconds(m.timeLeftMs);

    // Fighters by score, then kills (free-for-all and Duel score per fighter; elsewhere a fighter's score is kills).
    bool perEntity = f == Format::FreeForAll || f == Format::Duel;
    std::vector<std::pair<int8_t, BoardRow>> rows;               // with the team, for grouping
    for (const FighterEntry& e : m.fighters) {
        if (IsBotFormat(f) && e.team == kBotTeam) continue;     // a wave of "Bot" rows says nothing
        BoardRow r;
        r.name = e.name.empty() ? "#" + std::to_string(e.entity) : e.name;
        r.swatch = TeamFormat(f) ? TeamColor(e.team) : style::kIron;
        r.ink = e.alive ? style::kBone : style::kIron;
        r.kills = e.kills; r.deaths = e.deaths; r.score = perEntity ? ScoreOf(m, e.entity) : e.kills;
        r.pingMs = e.pingMs; r.host = e.entity == l.host; r.local = e.entity == l.me; r.dead = !e.alive;
        r.down = e.alive && e.down;
        rows.push_back({e.team, std::move(r)});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& c) {
        return a.second.score != c.second.score ? a.second.score > c.second.score : a.second.kills > c.second.kills;
    });
    if (!TeamFormat(f)) {
        for (auto& [team, r] : rows) b.rows.push_back(std::move(r));
        return b;
    }
    // Team formats: a band per team (its name, colour and score), its fighters under it; anyone outside the teams last.
    int teams = TeamCount(f);
    for (int t = 0; t <= teams; ++t) {
        if (t < teams) {
            BoardRow band;
            band.kind = BoardRow::Kind::Band; band.name = TeamName(t); band.swatch = TeamColor(t); band.ink = style::TeamInk(t);
            band.score = ScoreOf(m, t);
            b.rows.push_back(std::move(band));
        }
        for (auto& [team, r] : rows)
            if (t < teams ? team == t : (team < 0 || team >= teams)) b.rows.push_back(r);
    }
    return b;
}
}  // namespace hud_detail

inline HudScene BuildHudScene(const MatchInfo& info, const LocalHud& l, uint32_t nowMs) {
    HudScene s;
    if (info.phase == uint8_t(MatchPhase::Idle)) return s;
    const MatchInfo m = Extrapolate(info, l.infoAtMs, nowMs);
    s.visible = true;
    s.strip = hud_detail::Strip(m, l.me);
    if (m.timeLimitS && m.phase == uint8_t(MatchPhase::Live) && !InRoundGap(m)) {
        s.timeBar = true;
        s.timeFrac = std::clamp(float(m.timeLeftMs) / (float(m.timeLimitS) * 1000.f), 0.f, 1.f);
        s.clock = Clock(m.timeLeftMs);
    }
    s.pill = hud_detail::Pill(m, l.me, l.oneFight);
    const bool fought = m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath);
    // Item 9c: down by the draw check (fallen, knocked out, or the pelvis low over the feet), not just the game's flags.
    if (const FighterEntry* mine = FighterOf(m, l.me); mine && mine->alive && mine->down && fought && !InRoundGap(m) && !l.dead)
        s.pill = {true, "DOWN - GET UP", style::kBloodInk, 1, 1};
    if (l.giveUp > 0 && Format(m.format) == Format::Duel && fought && !InRoundGap(m))
        s.pill = {true, "GIVING UP " + std::to_string(int(std::clamp(l.giveUp, 0.f, 1.f) * 100.f)) + "%", style::kBloodInk, 1, 1};
    if (l.giveUp > 0 && InWaveBreak(m))          // the same G hold votes to skip the loot break
        s.pill = {true, "SKIPPING THE BREAK " + std::to_string(int(std::clamp(l.giveUp, 0.f, 1.f) * 100.f)) + "%", style::kOk, 1, 1};
    if (m.phase == uint8_t(MatchPhase::Countdown)) {
        if (m.timeLeftMs) {                      // the digits, each popping as it appears
            Look look = BannerLook((1000 - m.timeLeftMs % 1000) % 1000);
            s.banner = {true, Seconds(m.timeLeftMs), style::kBone, look.opacity, look.scale};
            if (Format(m.format) == Format::Duel) s.bannerSub = {true, "HOLD G TO GIVE UP A ROUND", style::kIron, look.opacity, 1};
        }
    } else if (!l.banner.text.empty()) {
        uint32_t age = AgeMs(l.banner.atMs, nowMs);
        if (age < kBannerEndMs) {
            Look look = BannerLook(age);
            s.banner = {true, l.banner.text, l.banner.color, look.opacity, look.scale};
            if (!l.banner.sub.empty()) s.bannerSub = {true, l.banner.sub, l.banner.color, look.opacity, look.scale};
        }
    }
    s.dead = hud_detail::Dead(m, l, nowMs);
    s.feed = hud_detail::Feed(l, nowMs);
    s.board = hud_detail::MakeBoard(m, l);
    return s;
}

}  // namespace hsm

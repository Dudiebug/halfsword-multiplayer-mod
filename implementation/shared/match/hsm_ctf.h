// Capture the flag: the rules, host-side, from fighter positions and states the host already has. Game-independent.
//   Ctf              - two flags (team 0's and team 1's), each Home, Carried or Dropped; Tick decides every take, drop,
//                      return and capture and says what happened (CtfEvent); the host scores a Captured with Match::AddCapture
//   DeriveBases      - where each team's base (and flag) stands, from the team spawn arcs (PartitionSides)
//   CtfEventLine     - the feed line for an event ("Ann captured the BLUE flag")
//   FlagStatusLine   - the HUD's top line for a flag ("RED FLAG: taken by Ann", "home", "dropped 14 s")
// Rules: an enemy touching a flag at its base or lying on the ground takes it. The carrier takes it to its own base; that
// counts only while its own flag is home (it counts as soon as the flag is). A carrier who dies, is downed for
// kDownDropMs, or leaves the match drops it where it stood. A dropped flag goes home after the return time, or at once
// when a teammate of its owner touches it (before an enemy's touch in the same tick). A fighter who just dropped a flag
// cannot take that same flag for kRetakeGuardMs.
#pragma once
#include "hsm_protocol.h"
#include "match/hsm_objective.h"
#include "session/hsm_spawn.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

inline constexpr double kFlagTouchCm = 160.0, kFlagTouchZCm = 220.0;   // a fighter touches a flag within this flat distance and height
inline constexpr double kBaseCaptureCm = 260.0;                        // a carrier captures within this flat distance of its base
inline constexpr double kMinBaseGapCm = 600.0;                         // bases closer than this share a touch: DeriveBases says so
inline constexpr uint32_t kDownDropMs = 500;                           // downed this long, a carrier drops the flag
inline constexpr uint32_t kRetakeGuardMs = 1500;

// One fighter as the host sees it this tick.
struct CtfFighter {
    uint8_t entity = 0;
    int8_t team = kNoTeam;
    Point3 at;
    bool alive = true;
    bool down = false;                  // fallen or knocked out (the host's reading of the fighter's vitals)
};

enum class CtfEventKind : uint8_t { Taken = 1, Dropped = 2, Returned = 3, Expired = 4, Captured = 5 };
// flag: the team whose flag it is (0 or 1). entity: who took, dropped, returned or captured it; 0 for Expired (it went home by itself).
struct CtfEvent {
    CtfEventKind kind = CtfEventKind::Taken;
    uint8_t flag = 0;
    uint8_t entity = 0;
    bool operator==(const CtfEvent&) const = default;
};
inline bool ValidCtfEvent(const CtfEvent& e) {
    const uint8_t k = uint8_t(e.kind);
    return k >= 1 && k <= 5 && e.flag <= 1 && (e.entity != 0 || e.kind == CtfEventKind::Expired);
}

// Wire: CtfEvent 62 (host -> client, reliable): what just happened to a flag, for the HUD's feed. MatchInfo carries the state.
struct CtfEventMsg { CtfEvent e; };
inline std::vector<uint8_t> Encode(const CtfEventMsg& m) {
    Writer w; w.header(MsgType::CtfEvent); w.u8(uint8_t(m.e.kind)); w.u8(m.e.flag); w.u8(m.e.entity); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, CtfEventMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::CtfEvent) return false;
    m.e.kind = CtfEventKind(r.u8()); m.e.flag = r.u8(); m.e.entity = r.u8();
    return r.done() && ValidCtfEvent(m.e);
}

class Ctf {
public:
    static constexpr int kTeams = 2;
    // A new match (and a rematch): both flags at their bases, nothing carried. returnMs: how long a dropped flag lies.
    void Start(uint32_t returnMs, const Point3& base0, const Point3& base1) {
        returnMs_ = returnMs; base_[0] = base0; base_[1] = base1;
        Reset();
    }
    void Reset() {
        for (int f = 0; f < kTeams; ++f) {
            flag_[f] = Flag{};
            flag_[f].at = base_[f];
        }
    }
    const Point3& base(int team) const { return base_[team & 1]; }
    uint32_t returnMs() const { return returnMs_; }
    ObjectiveState state(int flag) const { return flag_[flag & 1].state; }
    uint8_t carrier(int flag) const { return flag_[flag & 1].carrier; }
    // The flag a fighter carries (its owner's team), -1 when it carries none.
    int CarriedBy(uint8_t entity) const {
        for (int f = 0; f < kTeams; ++f) if (flag_[f].state == ObjectiveState::Carried && flag_[f].carrier == entity) return f;
        return -1;
    }
    // Time before a dropped flag goes home; 0 when it is not dropped.
    uint32_t LeftMs(int flag, uint32_t nowMs) const {
        const Flag& f = flag_[flag & 1];
        if (f.state != ObjectiveState::Dropped) return 0;
        const uint32_t lay = nowMs - f.since;
        return lay >= returnMs_ ? 0 : returnMs_ - lay;
    }
    // The host moved a dropped flag's place (to the floor under it). No effect on a flag that is not dropped.
    void MoveDropped(int flag, const Point3& to) { Flag& f = flag_[flag & 1]; if (f.state == ObjectiveState::Dropped) f.at = to; }

    // Both flags as objectives (the HUD's and MatchInfo's view).
    std::vector<Objective> Objectives(uint32_t nowMs) const {
        std::vector<Objective> out;
        for (int f = 0; f < kTeams; ++f) {
            const Flag& s = flag_[f];
            Objective o;
            o.owner = int8_t(f); o.state = s.state;
            o.carrier = s.state == ObjectiveState::Carried ? s.carrier : uint8_t(0);
            o.x = float(s.at.x); o.y = float(s.at.y); o.z = float(s.at.z);
            o.timerMs = std::min<uint32_t>(LeftMs(f, nowMs), kMaxObjectiveTimerMs);
            out.push_back(o);
        }
        return out;
    }

    // Once per host tick in a fought phase, with every fighter in the match. The events in the order they happened.
    std::vector<CtfEvent> Tick(uint32_t nowMs, const std::vector<CtfFighter>& fighters) {
        std::vector<CtfEvent> ev;
        for (int f = 0; f < kTeams; ++f) UpdateFlag(f, nowMs, fighters, ev);
        for (int f = 0; f < kTeams; ++f) Touch(f, nowMs, fighters, ev, true);    // a teammate returns before an enemy takes
        for (int f = 0; f < kTeams; ++f) Touch(f, nowMs, fighters, ev, false);
        for (int t = 0; t < kTeams; ++t) Capture(t, fighters, ev);
        return ev;
    }
    // A fighter left the match (or its slot was freed): it drops what it carries where it was last seen.
    std::vector<CtfEvent> Remove(uint8_t entity, uint32_t nowMs) {
        std::vector<CtfEvent> ev;
        for (int f = 0; f < kTeams; ++f)
            if (flag_[f].state == ObjectiveState::Carried && flag_[f].carrier == entity) Drop(f, flag_[f].at, nowMs, ev);
        return ev;
    }

private:
    struct Flag {
        ObjectiveState state = ObjectiveState::Home;
        uint8_t carrier = 0;
        Point3 at;                      // Home: the base; Carried: where the carrier was last seen; Dropped: where it lies
        uint32_t since = 0;             // Dropped: when it fell
        uint32_t downSince = 0;         // Carried: when the carrier went down (0: standing)
        uint8_t droppedBy = 0;          // the last carrier, kept for the retake guard
        uint32_t guardUntil = 0;
        bool downSeen = false;
    };
    static const CtfFighter* Find(const std::vector<CtfFighter>& fs, uint8_t e) {
        for (const CtfFighter& f : fs) if (f.entity == e) return &f;
        return nullptr;
    }
    static bool Near(const Point3& a, const Point3& b, double flatCm, double zCm) {
        return FlatDist2(a, b) <= flatCm * flatCm && std::fabs(a.z - b.z) <= zCm;
    }
    void Drop(int f, const Point3& at, uint32_t nowMs, std::vector<CtfEvent>& ev) {
        Flag& s = flag_[f];
        ev.push_back({CtfEventKind::Dropped, uint8_t(f), s.carrier});
        s.droppedBy = s.carrier; s.guardUntil = nowMs + kRetakeGuardMs;
        s.state = ObjectiveState::Dropped; s.carrier = 0; s.at = at; s.since = nowMs; s.downSeen = false; s.downSince = 0;
    }
    void Home(int f) {
        Flag& s = flag_[f];
        s.state = ObjectiveState::Home; s.carrier = 0; s.at = base_[f]; s.downSeen = false; s.downSince = 0;
    }
    void UpdateFlag(int f, uint32_t nowMs, const std::vector<CtfFighter>& fs, std::vector<CtfEvent>& ev) {
        Flag& s = flag_[f];
        if (s.state == ObjectiveState::Dropped) {
            if (nowMs - s.since >= returnMs_) { Home(f); ev.push_back({CtfEventKind::Expired, uint8_t(f), 0}); }
            return;
        }
        if (s.state != ObjectiveState::Carried) return;
        const CtfFighter* c = Find(fs, s.carrier);
        if (!c) { Drop(f, s.at, nowMs, ev); return; }                 // left the match: where it was last seen
        if (!c->alive) { Drop(f, c->at, nowMs, ev); return; }
        s.at = c->at;
        if (!c->down) { s.downSeen = false; return; }
        if (!s.downSeen) { s.downSeen = true; s.downSince = nowMs; }
        if (nowMs - s.downSince >= kDownDropMs) Drop(f, c->at, nowMs, ev);
    }
    // returns = true: teammates of the flag's owner touching it dropped (it goes home); false: enemies touching it take it.
    void Touch(int f, uint32_t nowMs, const std::vector<CtfFighter>& fs, std::vector<CtfEvent>& ev, bool returns) {
        Flag& s = flag_[f];
        if (s.state == ObjectiveState::Carried) return;
        if (returns && s.state != ObjectiveState::Dropped) return;
        for (const CtfFighter& c : fs) {
            if (!c.alive || c.down || c.team < 0 || c.team >= kTeams) continue;
            if ((c.team == f) != returns || !Near(c.at, s.at, kFlagTouchCm, kFlagTouchZCm)) continue;
            if (returns) { Home(f); ev.push_back({CtfEventKind::Returned, uint8_t(f), c.entity}); return; }
            if (s.droppedBy == c.entity && nowMs < s.guardUntil) continue;
            if (CarriedBy(c.entity) >= 0) continue;                     // one flag at a time
            s.state = ObjectiveState::Carried; s.carrier = c.entity; s.at = c.at; s.downSeen = false; s.downSince = 0;
            ev.push_back({CtfEventKind::Taken, uint8_t(f), c.entity});
            return;
        }
    }
    // The carrier of the enemy flag stands at its own base while its own flag is home.
    void Capture(int team, const std::vector<CtfFighter>& fs, std::vector<CtfEvent>& ev) {
        const int enemy = 1 - team;
        Flag& taken = flag_[enemy];
        if (taken.state != ObjectiveState::Carried || flag_[team].state != ObjectiveState::Home) return;
        const CtfFighter* c = Find(fs, taken.carrier);
        if (!c || !c->alive || c->team != team || FlatDist2(c->at, base_[team]) > kBaseCaptureCm * kBaseCaptureCm) return;
        ev.push_back({CtfEventKind::Captured, uint8_t(enemy), c->entity});
        Home(enemy);
    }
    Flag flag_[kTeams];
    Point3 base_[kTeams];
    uint32_t returnMs_ = 20000;
};

// ---------------------------------------------------------------- bases
struct CtfBases {
    Point3 at[2];
    bool ok = false;                    // both teams had spawn points and the bases are kMinBaseGapCm apart
    double apartCm = 0;
};
// Each team's base: the centroid of its spawn arc (`sides` from PartitionSides over `points`; a point with another side
// number than 0 or 1 is ignored). A team with no point gets the mirror of the other through the centroid of all points,
// and when there are none the two bases are put kMinBaseGapCm apart round `fallback`; ok is false then, or when the two
// come out closer than kMinBaseGapCm.
inline CtfBases DeriveBases(const std::vector<Point3>& points, const std::vector<int8_t>& sides, const Point3& fallback = Point3{}) {
    CtfBases b;
    double sx[2] = {0, 0}, sy[2] = {0, 0}, sz[2] = {0, 0}, ax = 0, ay = 0, az = 0;
    size_t n[2] = {0, 0}, all = 0;
    for (size_t i = 0; i < points.size() && i < sides.size(); ++i) {
        const Point3& p = points[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
        ax += p.x; ay += p.y; az += p.z; ++all;
        if (sides[i] != 0 && sides[i] != 1) continue;
        sx[sides[i]] += p.x; sy[sides[i]] += p.y; sz[sides[i]] += p.z; ++n[sides[i]];
    }
    const Point3 mid = all ? Point3{ax / double(all), ay / double(all), az / double(all)} : fallback;
    for (int t = 0; t < 2; ++t) if (n[t]) b.at[t] = {sx[t] / double(n[t]), sy[t] / double(n[t]), sz[t] / double(n[t])};
    if (n[0] && !n[1]) b.at[1] = {2 * mid.x - b.at[0].x, 2 * mid.y - b.at[0].y, b.at[0].z};
    else if (n[1] && !n[0]) b.at[0] = {2 * mid.x - b.at[1].x, 2 * mid.y - b.at[1].y, b.at[1].z};
    else if (!n[0] && !n[1]) { b.at[0] = {mid.x - kMinBaseGapCm / 2, mid.y, mid.z}; b.at[1] = {mid.x + kMinBaseGapCm / 2, mid.y, mid.z}; }
    b.apartCm = std::sqrt(FlatDist2(b.at[0], b.at[1]));
    b.ok = n[0] && n[1] && b.apartCm >= kMinBaseGapCm;
    return b;
}

// ---------------------------------------------------------------- text
// `names`: the name of an entity ("#4" when unknown). The feed line of an event: "Ann took the BLUE flag".
template <class NameFn>
inline std::string CtfEventLine(const CtfEvent& e, NameFn&& nameOf) {
    const std::string flag = TeamName(e.flag) + " flag", who = e.entity ? std::string(nameOf(e.entity)) : std::string();
    switch (e.kind) {
        case CtfEventKind::Taken: return who + " took the " + flag;
        case CtfEventKind::Dropped: return who + " dropped the " + flag;
        case CtfEventKind::Returned: return who + " returned the " + flag;
        case CtfEventKind::Expired: return "The " + flag + " went home";
        case CtfEventKind::Captured: return who + " captured the " + flag;
    }
    return {};
}

// The HUD's line for one flag: "RED FLAG: home", "RED FLAG: taken by Ann", "RED FLAG: dropped 14 s". `carrierName` for Carried.
inline std::string FlagStatusLine(const Objective& o, const std::string& carrierName) {
    std::string s = TeamName(o.owner) + " FLAG: ";
    switch (o.state) {
        case ObjectiveState::Home: return s + "home";
        case ObjectiveState::Carried: return s + "taken by " + carrierName;
        case ObjectiveState::Dropped: return s + "dropped " + std::to_string((o.timerMs + 999) / 1000) + " s";
    }
    return s;
}

}  // namespace hsm

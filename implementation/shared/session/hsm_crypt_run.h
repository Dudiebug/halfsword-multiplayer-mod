// The shared Crypts run's end and its results screen (protocol rev 50, docs/PLAN_CRYPTS_COOP.md M4).
//   CryptRun 77   one type, two shapes told apart by a kind byte:
//     kRunKit      client -> host, reliable: "I carry N items" (the kit's size in things, read when the kit is sent). The kit
//                  itself goes as LoadoutChange chunks (session/hsm_loadout_change.h); the host cannot read items out of a
//                  passport, so this is the number the results screen shows.
//     kRunResults  host -> client, reliable: the run is over: why, and one row per player (who got out, with how many items).
// RunBook is the host's record of a Grave Robbers run: who is inside, who got out, who is down. Each player's exit and death
// counts on its own; the run ends when nobody is still inside, when the time is up, or when the host leaves.
// Game-independent and unit-tested (session/test_hsm_crypt_run.cpp); the DLL (session.cpp) carries it out.
#pragma once
#include "match/hsm_crypts.h"
#include "protocol/hsm_protocol.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm::crypt {

constexpr uint8_t kRunKit = 0, kRunResults = 1;       // CryptRunMsg::kind
constexpr size_t kMaxRunRows = 21;                    // the fighter slots (host and 20 clients)
constexpr uint32_t kRunResultsMs = 20000;             // the results screen stays this long, then the lobby

// Why the run ended. The host decides.
enum class RunEnd : uint8_t { AllOut = 1, TimeUp = 2, HostLeft = 3 };
inline bool ValidRunEnd(uint8_t r) { return r >= uint8_t(RunEnd::AllOut) && r <= uint8_t(RunEnd::HostLeft); }
inline const char* RunEndName(RunEnd r) {
    switch (r) {
        case RunEnd::AllOut: return "everyone is out or down";
        case RunEnd::TimeUp: return "time is up";
        case RunEnd::HostLeft: return "the host left";
    }
    return "?";
}

// What became of one player's run.
//   Out     reached an exit with their kit
//   Down    fell in the Crypts; the last kit read alive is kept
//   Inside  still in the Crypts when the run ended (time up, or the host left)
//   Absent  never got in (joined too late, would not load)
//   Left    left the game during the run
enum class Outcome : uint8_t { Out = 1, Down = 2, Inside = 3, Absent = 4, Left = 5 };
inline bool ValidOutcome(uint8_t o) { return o >= uint8_t(Outcome::Out) && o <= uint8_t(Outcome::Left); }
inline const char* OutcomeName(Outcome o) {
    switch (o) {
        case Outcome::Out: return "got out";
        case Outcome::Down: return "went down";
        case Outcome::Inside: return "still inside";
        case Outcome::Absent: return "never got in";
        case Outcome::Left: return "left the game";
    }
    return "?";
}

struct RunRow { uint8_t slot = 0; Outcome outcome = Outcome::Absent; uint8_t items = 0; std::string name; };
struct CryptRunMsg {
    uint8_t kind = kRunKit;
    uint16_t seq = 0;                 // the gather's seq (never 0)
    uint8_t items = 0;                // kRunKit
    RunEnd reason = RunEnd::AllOut;   // kRunResults
    std::vector<RunRow> rows;         // kRunResults
};

inline std::vector<uint8_t> Encode(const CryptRunMsg& m) {
    Writer w; w.header(MsgType::CryptRun); w.u8(m.kind); w.u16(m.seq);
    if (m.kind == kRunKit) { w.u8(m.items); return w.buf; }
    w.u8(uint8_t(m.reason));
    const size_t n = std::min(m.rows.size(), kMaxRunRows);
    w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) { const RunRow& r = m.rows[i]; w.u8(r.slot); w.u8(uint8_t(r.outcome)); w.u8(r.items); w.str(r.name, kMaxPlayerName); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, CryptRunMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::CryptRun) return false;
    CryptRunMsg v;
    v.kind = r.u8(); v.seq = r.u16();
    if (!r.ok() || v.seq == 0) return false;
    if (v.kind == kRunKit) {
        v.items = r.u8();
        if (!r.done()) return false;
    } else if (v.kind == kRunResults) {
        const uint8_t reason = r.u8(), count = r.u8();
        if (!r.ok() || !ValidRunEnd(reason) || count > kMaxRunRows) return false;
        v.reason = RunEnd(reason);
        for (uint8_t i = 0; i < count; ++i) {
            RunRow row; row.slot = r.u8(); const uint8_t o = r.u8(); row.items = r.u8(); row.name = r.str(kMaxPlayerName);
            if (!r.ok() || !ValidOutcome(o)) return false;
            row.outcome = Outcome(o);
            v.rows.push_back(std::move(row));
        }
        if (!r.done()) return false;
    } else return false;
    m = std::move(v);
    return true;
}

// The host's record of one run.
class RunBook {
public:
    // A new run: everyone in the gather starts as Absent until they say they are in the Crypts.
    void Begin(uint16_t seq) { seq_ = seq; rows_.clear(); }
    uint16_t seq() const { return seq_; }
    void Add(uint8_t slot, const std::string& name) {
        if (Row* r = Find(slot)) { r->name = name; return; }
        if (rows_.size() >= kMaxRunRows) return;
        Row r; r.slot = slot; r.name = name; rows_.push_back(r);
    }
    // A player's note about their own run (the Gather message's notes). A run that has ended for the player stays ended.
    void OnNote(uint8_t slot, GatherNote n) {
        Row* r = Find(slot);
        if (!r || Ended(r->outcome)) return;
        switch (n) {
            case GatherNote::InCrypts: r->outcome = Outcome::Inside; break;
            case GatherNote::Extracted: r->outcome = Outcome::Out; break;
            case GatherNote::Died: r->outcome = Outcome::Down; break;
            case GatherNote::Waiting: r->outcome = Outcome::Absent; break;
            case GatherNote::Loading: break;
        }
    }
    void OnItems(uint8_t slot, uint8_t items) { if (Row* r = Find(slot)) r->items = items; }
    // A player left the game: a run still going is over for them; one that got out or went down keeps its outcome.
    void OnLeft(uint8_t slot) {
        Row* r = Find(slot);
        if (r && (r->outcome == Outcome::Inside || r->outcome == Outcome::Absent)) r->outcome = Outcome::Left;
    }
    Outcome Of(uint8_t slot) const { for (const Row& r : rows_) if (r.slot == slot) return r.outcome; return Outcome::Absent; }
    int Count(Outcome o) const { int n = 0; for (const Row& r : rows_) n += r.outcome == o ? 1 : 0; return n; }
    int GotOut() const { return Count(Outcome::Out); }
    int Players() const { return int(rows_.size()); }
    // Nobody is still in the Crypts (every player is out, down, gone or never got in) and somebody got in: the run is over.
    bool AllDone() const { return Count(Outcome::Inside) == 0 && Count(Outcome::Absent) < Players(); }

    // The results: who got out first, then who went down, still inside, the rest; each by slot.
    CryptRunMsg Results(RunEnd reason) const {
        CryptRunMsg m; m.kind = kRunResults; m.seq = seq_ ? seq_ : 1; m.reason = reason;
        for (const Row& r : rows_) m.rows.push_back({r.slot, r.outcome, r.items, r.name});
        std::stable_sort(m.rows.begin(), m.rows.end(), [](const RunRow& a, const RunRow& b) {
            return a.outcome != b.outcome ? uint8_t(a.outcome) < uint8_t(b.outcome) : a.slot < b.slot; });
        return m;
    }

private:
    struct Row { uint8_t slot = 0; std::string name; Outcome outcome = Outcome::Absent; uint8_t items = 0; };
    static bool Ended(Outcome o) { return o == Outcome::Out || o == Outcome::Down || o == Outcome::Left; }
    Row* Find(uint8_t slot) { for (Row& r : rows_) if (r.slot == slot) return &r; return nullptr; }
    uint16_t seq_ = 0;
    std::vector<Row> rows_;
};

// What a results row says: "Ann - got out - 7 items".
inline std::string RowText(const RunRow& r) {
    std::string s = r.name + " - " + OutcomeName(r.outcome);
    if (r.outcome == Outcome::Out || r.outcome == Outcome::Down) s += " - " + std::to_string(r.items) + (r.items == 1 ? " item" : " items");
    return s;
}
inline std::string ResultsTitle(RunEnd reason, int gotOut) {
    return "CRYPTS RESULTS - " + std::string(RunEndName(reason)) + ": " + std::to_string(gotOut) + " got out";
}
// The count of players who got out, from a results message.
inline int GotOutOf(const CryptRunMsg& m) { int n = 0; for (const RunRow& r : m.rows) n += r.outcome == Outcome::Out ? 1 : 0; return n; }

// A run is still going on this machine's clock? The results screen runs kRunResultsMs from when it came.
inline bool ResultsShowing(uint32_t now, uint32_t shownAt) { return shownAt != 0 && now - shownAt < kRunResultsMs; }

}  // namespace hsm::crypt

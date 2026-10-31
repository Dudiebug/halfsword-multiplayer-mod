// What the log writes (security audit M3: attacker-triggered log lines had no cap, and every line was flushed): a size cap
// for the session, a lines-per-second throttle, and a collapse of identical consecutive lines. Pure state, no file I/O:
// log.cpp asks Filter() for each line and writes what it says.
//
// The numbers leave a normal session untouched. A long session writes a few hundred KB an hour and the busiest second
// of a level load or a join a few hundred lines; the cap is 32 MB (about 100x a long session), the throttle 1000 lines
// a second, and only a line repeated verbatim more than twice in a row is collapsed.
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace hsm::util {

constexpr size_t kLogMaxBytes = 32u * 1024 * 1024;   // per session; the last three sessions' files are kept apart (HSMMod.prev.log, .prev2.log, .prev3.log)
constexpr int kLogMaxLinesPerSec = 1000;             // more are dropped, and counted in a note
constexpr int kLogRepeatInFull = 2;                  // an identical line is written this many times in a row, then collapsed
constexpr double kLogRepeatSummaryMs = 5000.0;       // a line repeating without end gets its tally at least this often

class LogGuard {
public:
    struct Verdict {
        bool write = false;   // write the line itself
        std::string note;     // a line of the guard's own to write first (empty: none); written whether or not the line is
    };

    explicit LogGuard(size_t maxBytes = kLogMaxBytes, int maxLinesPerSec = kLogMaxLinesPerSec)
        : maxBytes_(maxBytes), maxLinesPerSec_(maxLinesPerSec) {}

    // msg: the line without its time prefix. The caller adds bytes with Wrote().
    Verdict Filter(std::string_view msg, double nowMs) {
        Verdict v;
        if (capped_) return v;
        if (bytes_ >= maxBytes_) {
            capped_ = true;
            v.note = "log size limit of " + std::to_string(maxBytes_ / (1024 * 1024)) + " MB reached; nothing more is logged this session";
            return v;
        }
        if (haveLast_ && msg == last_) {
            if (++run_ > kLogRepeatInFull) {
                ++pending_;
                if (nowMs - lastSummaryMs_ >= kLogRepeatSummaryMs || nowMs < lastSummaryMs_) { v.note = Summary(); lastSummaryMs_ = nowMs; }
                return v;
            }
        } else {
            if (pending_) { v.note = Summary(); lastSummaryMs_ = nowMs; }
            run_ = 1;
        }
        if (nowMs - windowStartMs_ >= 1000.0 || nowMs < windowStartMs_) {   // a new one-second window
            if (windowDropped_) {
                if (!v.note.empty()) v.note += '\n';
                v.note += std::to_string(windowDropped_) + " log lines dropped by the throttle (more than " + std::to_string(maxLinesPerSec_) +
                          " a second)";
            }
            windowStartMs_ = nowMs; windowLines_ = 0; windowDropped_ = 0;
        }
        if (++windowLines_ > maxLinesPerSec_) {
            ++windowDropped_; ++droppedTotal_;
            run_ = 0;   // the line was not written, so it starts no run
            return v;
        }
        last_.assign(msg.data(), msg.size());
        haveLast_ = true;
        v.write = true;
        return v;
    }

    void Wrote(size_t bytes) { bytes_ += bytes; }
    size_t bytes() const { return bytes_; }
    bool capped() const { return capped_; }
    size_t droppedByThrottle() const { return droppedTotal_; }

private:
    std::string Summary() {
        std::string s = "(the line above repeated " + std::to_string(pending_) + " more time" + (pending_ == 1 ? "" : "s") + ")";
        pending_ = 0;
        return s;
    }

    size_t maxBytes_;
    int maxLinesPerSec_;
    size_t bytes_ = 0;
    bool capped_ = false;
    std::string last_;
    bool haveLast_ = false;
    int run_ = 0;                 // consecutive identical lines seen, this one included
    size_t pending_ = 0;          // identical lines collapsed since the last tally
    double lastSummaryMs_ = 0;
    double windowStartMs_ = 0;
    int windowLines_ = 0;
    size_t windowDropped_ = 0, droppedTotal_ = 0;
};

}  // namespace hsm::util

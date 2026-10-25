// Why a fighter's poses come slowly (pure, no engine types). Item 7a.
//
// The owner sends a pose every frame it is due (60 Hz players, 30 Hz bots). A frame longer than the cadence still sends
// one pose that frame, so a slow game on the owner's side thins the stream to its frame rate. The receiver tells the two
// causes apart by the owner's stream clock each pose carries: a gap past the slow gap in that clock means the owner's
// frames were long ("frame"); a gap in arrival time while the owner's clock advanced normally means the network held the
// poses back ("network"). SlowSenderMeter counts both over fixed windows and says when a window is worth a log line.
#pragma once
#include <algorithm>
#include <cstdint>

namespace hsm::sync {

constexpr double kSlowGapMs = 25.0;          // a pose gap past this (or 1.5 cadences, if longer) is slow
constexpr double kSlowWindowMs = 5000.0;     // the meter's window
constexpr double kSlowShare = 0.10;          // a window with this share of slow gaps is worth a log line
constexpr double kSlowRelogMs = 30000.0;     // the same cause logs again at most this often

enum class SlowCause : uint8_t { None, Frame, Network };

inline const char* SlowCauseName(SlowCause c) {
    return c == SlowCause::Frame ? "frame" : c == SlowCause::Network ? "network" : "none";
}

// The slow gap for a stream sent every cadenceMs.
inline double SlowGapFor(double cadenceMs) { return std::max(kSlowGapMs, cadenceMs * 1.5); }

// One gap between two poses: tsGapMs in the owner's stream clock, arrivalGapMs in the receiver's.
inline SlowCause ClassifyPoseGap(double tsGapMs, double arrivalGapMs, double slowGapMs) {
    if (tsGapMs > slowGapMs) return SlowCause::Frame;
    if (arrivalGapMs > slowGapMs) return SlowCause::Network;
    return SlowCause::None;
}

struct SlowReport {
    uint32_t gaps = 0, frame = 0, network = 0;   // in the window just closed
    double worstTsGapMs = 0, worstArrivalGapMs = 0;
    SlowCause cause = SlowCause::None;           // the larger share (frame on a tie: the network cannot speed frames up)
};

class SlowSenderMeter {
public:
    explicit SlowSenderMeter(double cadenceMs = 1000.0 / 60.0) : slowGapMs_(SlowGapFor(cadenceMs)) {}

    // Each accepted pose, oldest first: its owner clock and local arrival time. True when the window just closed is
    // worth a log line; Last() has it then.
    bool Push(uint32_t tsMs, double arrivalMs) {
        bool due = false;
        if (have_ && arrivalMs - windowStartMs_ >= kSlowWindowMs) {
            due = Close(arrivalMs);
            windowStartMs_ = arrivalMs;
        }
        if (!have_) windowStartMs_ = arrivalMs;
        if (have_) {
            const double tsGap = double(uint32_t(tsMs - lastTs_));   // the stream clock wraps like a sequence
            const double arrivalGap = arrivalMs - lastArrivalMs_;
            ++cur_.gaps;
            switch (ClassifyPoseGap(tsGap, arrivalGap, slowGapMs_)) {
                case SlowCause::Frame: ++cur_.frame; break;
                case SlowCause::Network: ++cur_.network; break;
                case SlowCause::None: break;
            }
            cur_.worstTsGapMs = std::max(cur_.worstTsGapMs, tsGap);
            cur_.worstArrivalGapMs = std::max(cur_.worstArrivalGapMs, arrivalGap);
        }
        have_ = true;
        lastTs_ = tsMs;
        lastArrivalMs_ = arrivalMs;
        return due;
    }

    const SlowReport& Last() const { return last_; }
    double SlowGapMs() const { return slowGapMs_; }

private:
    bool Close(double nowMs) {
        SlowReport r = cur_;
        cur_ = SlowReport{};
        const uint32_t slow = r.frame + r.network;
        if (r.gaps == 0 || double(slow) < kSlowShare * double(r.gaps)) return false;
        r.cause = r.frame >= r.network ? SlowCause::Frame : SlowCause::Network;
        if (r.cause == lastCause_ && haveLogged_ && nowMs - lastLogMs_ < kSlowRelogMs) return false;
        last_ = r;
        lastCause_ = r.cause;
        lastLogMs_ = nowMs;
        haveLogged_ = true;
        return true;
    }

    double slowGapMs_;
    bool have_ = false, haveLogged_ = false;
    uint32_t lastTs_ = 0;
    double lastArrivalMs_ = 0, windowStartMs_ = 0, lastLogMs_ = 0;
    SlowReport cur_{}, last_{};
    SlowCause lastCause_ = SlowCause::None;
};

// The owner's own side: how many of its frames ran past kSlowGapMs in a window. Poses still go every such frame.
class SlowFrameMeter {
public:
    // Each frame: its length and the local time. True when the window just closed is worth a log line.
    bool Push(double dtMs, double nowMs) {
        bool due = false;
        if (have_ && nowMs - windowStartMs_ >= kSlowWindowMs) {
            lastFrames_ = frames_; lastSlow_ = slow_; lastWorstMs_ = worstMs_;
            due = frames_ > 0 && double(slow_) >= kSlowShare * double(frames_) && (!haveLogged_ || nowMs - lastLogMs_ >= kSlowRelogMs);
            if (due) { lastLogMs_ = nowMs; haveLogged_ = true; }
            frames_ = slow_ = 0; worstMs_ = 0;
            windowStartMs_ = nowMs;
        }
        if (!have_) { windowStartMs_ = nowMs; have_ = true; }
        ++frames_;
        if (dtMs > kSlowGapMs) ++slow_;
        worstMs_ = std::max(worstMs_, dtMs);
        return due;
    }
    uint32_t Frames() const { return lastFrames_; }   // of the window just closed
    uint32_t Slow() const { return lastSlow_; }
    double WorstMs() const { return lastWorstMs_; }

private:
    bool have_ = false, haveLogged_ = false;
    uint32_t frames_ = 0, slow_ = 0, lastFrames_ = 0, lastSlow_ = 0;
    double worstMs_ = 0, lastWorstMs_ = 0, windowStartMs_ = 0, lastLogMs_ = 0;
};

}  // namespace hsm::sync

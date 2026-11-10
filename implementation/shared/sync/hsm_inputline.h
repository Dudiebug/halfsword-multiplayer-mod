// Rev 42, the input timeline of a remote fighter (competitive netcode Phases 3, 4 and 6, docs/PLAN_COMPETITIVE_NETCODE.md 6.1-6.3).
// Pure logic, unit-tested.
//
// Owner sync shows another player's fighter as a stand-in. Driven only by its pose stream, the stand-in is always in the
// past (the stream's latency plus its playout buffer). Driven by the owner's inputs, played through the game's own
// controls when they fall due, it moves with the owner's muscles at the owner's time -- as a rollback fighting game shows
// the opponent in the present -- and the pose stream only corrects it (sync/hsm_reconcile.h).
//
//   owner:    InputBatchSender  every input frame is stamped with when the owner's own fighter executes it: its press
//             time plus the owner's input delay D (both on the owner's pose stream clock), and the newest kInputBatchMax
//             frames go out in every packet (GGPO-style redundancy: one lost packet costs no input).
//   receiver: InputTimeline     buffers them, maps the owner's clock to its own with the pose stream's clock offset and
//             half the round trip, and hands out each frame when its execution time comes. A frame that is not there in
//             time is predicted (GGPO): the last one repeated, its mouse turns decayed 1, 2/3, 1/3, 0 (Rocket
//             League's input decay) so a predicted swing does not overshoot. When the real frame then arrives, the turns
//             the prediction already gave are taken back from it, so the stand-in's aim ends where the owner's did.
//
// When D covers the one-way latency, every frame arrives before it is due and nothing is predicted: both machines run
// both fighters from the same inputs at the same time (the hybrid delay + prediction model of modern fighting games).
#pragma once
#include "hsm_protocol.h"
#include "hsm_input.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace hsm::sync {

constexpr double kInputDecay[] = {1.0, 2.0 / 3.0, 1.0 / 3.0, 0.0};   // predicted frame k's share of the last turn
constexpr size_t kInputDecaySteps = sizeof(kInputDecay) / sizeof(kInputDecay[0]);
constexpr size_t kTimelineKeep = 64;          // buffered frames per fighter (a second at 60 Hz)
constexpr double kTimelineStaleMs = 500.0;    // no frame for this long: the fighter is idle, stop predicting turns

// The owner's pose stream clock at local time localMs: the stream's clock offset (sync/hsm_poseplay.h ClockState: the
// least arrival-minus-send, the clock difference plus the quickest one-way trip) less half the round trip.
inline double OwnerClockAt(double localMs, double offsetMs, double rttMs) {
    return localMs - offsetMs + std::max(0.0, rttMs) * 0.5;
}

// ---- owner side --------------------------------------------------------------------------------------------------
class InputBatchSender {
public:
    // A frame captured at streamNowMs (the owner's pose stream clock), executed delayMs later. Returns the batch to send.
    InputBatch Add(uint8_t entity, const InputFrame& f, uint32_t streamNowMs, uint16_t delayMs) {
        const uint16_t d = std::min<uint16_t>(delayMs, kInputDelayMaxMs);
        uint32_t exec = streamNowMs + d;
        if (!recent_.empty()) {
            const auto& last = recent_.back();
            if (f.seq != last.second.seq + 1u) recent_.clear();                        // a gap in seq (a new fighter): start over
            else if (int32_t(exec - last.first) < 0) exec = last.first;                // D went down: never execute before the last
        }
        recent_.push_back({exec, f});
        while (recent_.size() > kInputBatchMax) recent_.pop_front();
        InputBatch b; b.entity = entity; b.delayMs = d;
        b.frames.assign(recent_.begin(), recent_.end());
        return b;
    }
    void Clear() { recent_.clear(); }

private:
    std::deque<std::pair<uint32_t, InputFrame>> recent_;
};

// ---- receiver side -----------------------------------------------------------------------------------------------
// Two tracks, as the owner plays them (session.cpp's input hook): the look axes (mouse and stick turning, which also aim
// the arms) and the control rotation are never delayed, so they fall due at the press (execTsMs - delayMs); movement,
// arm axes, buttons and edges fall due at execTsMs.
struct TimelineStep {
    InputFrame frame{};          // what to feed this game frame (for an InputApplier); mouse turns are this frame's amount
    bool have = false;           // false: nothing ever arrived, feed nothing
    bool predicted = false;      // a track was not there in time: predicted
    bool mainPredicted = false;  // the delayed track (movement, arms, buttons) was not there in time: what the input delay covers
    bool lookPredicted = false;  // the look track: due at the press, so remotely always predicted for about the one-way trip
    uint32_t predictedFrames = 0;// predicted frames in a row (the longer of the two tracks)
};

class InputTimeline {
public:
    // Buffers a batch; ownerRxTs is OwnerClockAt() at its arrival. Each new frame's margin (its execution time minus its
    // arrival, owner clock; negative = it came after it was due: its main track was predicted) goes to `margins`. The look
    // track is due at the press, so it is always predicted for about the one-way trip: the input delay covers the rest.
    void Push(const InputBatch& b, double ownerRxTs, std::vector<double>* margins = nullptr) {
        for (const auto& [exec, f] : b.frames) {
            if (haveApplied_ && !SeqNewer(f.seq, appliedSeq_)) continue;   // already played
            bool dup = false;
            for (const Entry& q : queue_) if (q.frame.seq == f.seq) { dup = true; break; }
            if (dup) continue;
            const uint32_t lookAt = exec - std::min<uint32_t>(b.delayMs, kInputDelayMaxMs);
            if (margins) margins->push_back(double(int32_t(exec - uint32_t(std::llround(ownerRxTs)))));
            auto it = queue_.begin();
            while (it != queue_.end() && SeqNewer(f.seq, it->frame.seq)) ++it;
            queue_.insert(it, Entry{exec, lookAt, f, false});
            if (queue_.size() > kTimelineKeep) queue_.pop_front();
        }
        haveRx_ = true;
        delayMs_ = b.delayMs;
    }

    // The input for the game frame at ownerNowTs (the owner's clock now). Every frame due by then is folded into one: levels
    // and buttons from the newest, mouse turns summed, edges from the newest's toggles (hsm_input.h InputApplier replays the
    // edges between). A track with nothing due and its last real frame more than a cadence old is predicted.
    TimelineStep Step(double ownerNowTs, double cadenceMs) {
        TimelineStep s;
        const uint32_t now = uint32_t(std::llround(ownerNowTs));
        // Look track.
        bool lookDue = false;
        std::array<float, kAxisCount> look{};
        for (Entry& e : queue_) {
            if (e.lookDone || int32_t(e.lookAt - now) > 0) continue;
            for (size_t i = 0; i < kAxisCount; ++i) {
                if (!IsLookAxis(Axis(i))) continue;
                if (IsDeltaAxis(Axis(i))) look[i] += e.frame.axes[i]; else look[i] = e.frame.axes[i];
            }
            lastLook_ = e.frame; lastLookAt_ = e.lookAt; haveLook_ = true;
            e.lookDone = true; lookDue = true;
        }
        // Main track.
        bool mainDue = false;
        while (!queue_.empty() && int32_t(queue_.front().exec - now) <= 0) {
            const Entry& e = queue_.front();
            last_ = e.frame; lastExec_ = e.exec; haveLast_ = true;
            appliedSeq_ = e.frame.seq; haveApplied_ = true;
            if (!e.lookDone) { lastLook_ = e.frame; lastLookAt_ = e.lookAt; haveLook_ = true; lookDue = true;
                for (size_t i = 0; i < kAxisCount; ++i) if (IsLookAxis(Axis(i))) { if (IsDeltaAxis(Axis(i))) look[i] += e.frame.axes[i]; else look[i] = e.frame.axes[i]; } }
            queue_.pop_front();
            mainDue = true;
        }
        if (!haveLast_ && !haveLook_) return s;
        s.have = true;
        s.frame = haveLast_ ? last_ : lastLook_;
        // Main track prediction: the levels and buttons simply stay as last given (GGPO's repeat-last).
        if (!mainDue && haveLast_ && ownerNowTs - double(lastExec_) > cadenceMs * 1.5) { s.predicted = s.mainPredicted = true; ++mainRun_; } else if (mainDue) mainRun_ = 0;
        // Look track: a real turn pays back what a prediction already gave; a missing one is predicted, decaying.
        if (lookDue) {
            for (size_t i = 0; i < kAxisCount; ++i) {
                if (!IsLookAxis(Axis(i))) continue;
                s.frame.axes[i] = IsDeltaAxis(Axis(i)) ? Clamp(look[i] - owedTurn_[i]) : look[i];
            }
            owedTurn_.fill(0.f);
            lookRun_ = 0;
        } else {
            const bool late = haveLook_ && ownerNowTs - double(lastLookAt_) > cadenceMs * 1.5;
            const bool idle = !haveLook_ || ownerNowTs - double(lastLookAt_) > kTimelineStaleMs;
            for (size_t i = 0; i < kAxisCount; ++i) {
                if (!IsLookAxis(Axis(i))) continue;
                if (!IsDeltaAxis(Axis(i))) { s.frame.axes[i] = haveLook_ ? lastLook_.axes[i] : 0.f; continue; }
                float v = 0.f;
                if (late && !idle) {
                    v = float(lastLook_.axes[i] * kInputDecay[std::min<size_t>(lookRun_, kInputDecaySteps - 1)]);
                    owedTurn_[i] += v;
                }
                s.frame.axes[i] = v;
            }
            if (late) { s.predicted = s.lookPredicted = true; ++lookRun_; }
        }
        if (haveLook_) { s.frame.controlYaw = lastLook_.controlYaw; s.frame.controlPitch = lastLook_.controlPitch; }
        s.predictedFrames = std::max(mainRun_, lookRun_);
        return s;
    }

    void Clear() { queue_.clear(); haveLast_ = haveLook_ = haveApplied_ = haveRx_ = false; mainRun_ = lookRun_ = 0; owedTurn_.fill(0.f); }
    size_t Buffered() const { return queue_.size(); }
    uint16_t OwnerDelayMs() const { return delayMs_; }

private:
    struct Entry { uint32_t exec, lookAt; InputFrame frame; bool lookDone; };
    static bool SeqNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }
    static float Clamp(float v) { return std::clamp(v, -kAxisLimit, kAxisLimit); }

    std::deque<Entry> queue_;   // not yet played on the main track, seq order
    InputFrame last_{}, lastLook_{};
    bool haveLast_ = false, haveLook_ = false, haveApplied_ = false, haveRx_ = false;
    uint32_t appliedSeq_ = 0, lastExec_ = 0, lastLookAt_ = 0;
    uint32_t mainRun_ = 0, lookRun_ = 0;
    uint16_t delayMs_ = 0;
    std::array<float, kAxisCount> owedTurn_{};
};

}  // namespace hsm::sync

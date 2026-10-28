// Fractures of geometry collections (ObjectEvent kind Fracture, protocol rev 26, plan M0/pre2). Game-independent, unit-tested.
//   FractureLog   what has broken, per object, in the order it broke: deduped by (object, body, item), capped per object and
//                 overall. The host keeps one so a machine that joins late is sent the breaks in order (ObjectEvent each);
//                 every machine uses its Record() result to apply a break once.
//   FractureRate  the sender's cap: at most kFractureBurst fractures per object per kFractureWindowMs, the rest are dropped
//                 (a collection that falls apart into many clusters at once must not flood the reliable channel).
#pragma once
#include "session/hsm_object_event.h"
#include <cstdint>
#include <map>
#include <vector>

namespace hsm {

constexpr size_t kMaxFracturesPerObject = 64;
constexpr size_t kMaxFractureObjects = 256;
constexpr int kFractureBurst = 8;
constexpr double kFractureWindowMs = 1000.0;

class FractureLog {
public:
    // true when m is a valid, new fracture and was kept (first break of that cluster, room for it); false for anything else.
    bool Record(const ObjectEventMsg& m) {
        if (m.kind != ObjectEventKind::Fracture || !ValidObjectId(m.objId)) return false;
        size_t mine = 0;
        for (const ObjectEventMsg& e : log_) {
            if (e.objId != m.objId) continue;
            if (e.body == m.body && e.item == m.item) return false;
            ++mine;
        }
        if (mine >= kMaxFracturesPerObject) return false;
        if (mine == 0 && objects_ >= kMaxFractureObjects) return false;
        if (mine == 0) ++objects_;
        log_.push_back(m);
        return true;
    }
    bool Has(uint64_t objId, uint8_t body, uint16_t item) const {
        for (const ObjectEventMsg& e : log_)
            if (e.objId == objId && e.body == body && e.item == item) return true;
        return false;
    }
    std::vector<ObjectEventMsg> Of(uint64_t objId) const {
        std::vector<ObjectEventMsg> out;
        for (const ObjectEventMsg& e : log_) if (e.objId == objId) out.push_back(e);
        return out;
    }
    // Every recorded fracture, encoded, in the order it was recorded.
    std::vector<std::vector<uint8_t>> ForJoiner() const {
        std::vector<std::vector<uint8_t>> out;
        out.reserve(log_.size());
        for (const ObjectEventMsg& e : log_) out.push_back(Encode(e));
        return out;
    }
    void Forget(uint64_t objId) {
        const size_t before = log_.size();
        for (size_t i = 0; i < log_.size();) {
            if (log_[i].objId == objId) log_.erase(log_.begin() + long(i)); else ++i;
        }
        if (log_.size() != before) --objects_;
    }
    size_t Size() const { return log_.size(); }
    size_t Objects() const { return objects_; }
    void Clear() { log_.clear(); objects_ = 0; }
private:
    std::vector<ObjectEventMsg> log_;
    size_t objects_ = 0;
};

class FractureRate {
public:
    bool Allow(uint64_t objId, double nowMs) {
        auto it = win_.find(objId);
        if (it == win_.end()) {
            if (win_.size() >= kMaxFractureObjects) return false;
            win_[objId] = Window{nowMs, 1};
            return true;
        }
        Window& w = it->second;
        if (nowMs - w.startMs >= kFractureWindowMs || nowMs < w.startMs) { w = Window{nowMs, 1}; return true; }
        if (w.count >= kFractureBurst) return false;
        ++w.count;
        return true;
    }
    void Clear() { win_.clear(); }
private:
    struct Window { double startMs = 0; int count = 0; };
    std::map<uint64_t, Window> win_;
};

}  // namespace hsm

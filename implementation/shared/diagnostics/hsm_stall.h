#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace hsm::diagnostics {
// One producer (game thread), one reader. Every shared cell is atomic: a reader
// never waits for the producer, even if it stops mid-publication. Labels must be
// static strings. Function names are copied, never dereferenced by the reader.
static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<size_t>::is_always_lock_free &&
              std::atomic<const char*>::is_always_lock_free && std::atomic<char>::is_always_lock_free);
class StallTrace {
public:
    static constexpr size_t kDepth = 32, kName = 64;
    struct Frame {
        const char* stage = nullptr;
        uint64_t since = 0, detail = 0;
        std::array<char, kName> name{};
    };
    struct Snapshot {
        uint64_t heartbeat = 0, progress = 0, serial = 0;
        size_t depth = 0;
        std::array<Frame, kDepth> frames{};
    };
    void Heartbeat(uint64_t now) { Begin(); heartbeat_ = now; End(now); }
    size_t Push(const char* stage, uint64_t detail, uint64_t now) {
        Begin();
        const size_t slot = depth_.load();
        depth_ = slot + 1;
        if (slot < kDepth) {
            auto& f = frames_[slot];
            f.stage = stage; f.detail = detail; f.since = now;
            f.name[0] = 0;
        }
        End(now); return slot;
    }
    void Stage(size_t slot, const char* stage, uint64_t now) {
        Begin(); if (slot < kDepth) frames_[slot].stage = stage; End(now);
    }
    void Name(size_t slot, const char* name, uint64_t now) {
        Begin();
        if (slot < kDepth) {
            size_t i = 0;
            for (; i + 1 < kName && name[i]; ++i) frames_[slot].name[i] = name[i];
            frames_[slot].name[i] = 0;
        }
        End(now);
    }
    void Pop(size_t slot, uint64_t now) { Begin(); depth_ = slot; End(now); }
    bool Read(Snapshot& out) const {
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto before = sequence_.load();
            if (before & 1) continue;
            Snapshot s;
            s.heartbeat = heartbeat_.load(); s.progress = progress_.load();
            s.serial = before; s.depth = depth_.load();
            for (size_t i = 0; i < std::min(s.depth, kDepth); ++i) {
                const auto& src = frames_[i]; auto& dst = s.frames[i];
                dst.stage = src.stage.load(); dst.since = src.since.load(); dst.detail = src.detail.load();
                for (size_t j = 0; j < kName; ++j) dst.name[j] = src.name[j].load();
                dst.name.back() = 0;
            }
            if (sequence_.load() == before) { out = s; return true; }
        }
        return false; // caller retains its last coherent sample
    }
private:
    struct Cell {
        std::atomic<const char*> stage{nullptr};
        std::atomic<uint64_t> since{0}, detail{0};
        std::array<std::atomic<char>, kName> name{};
    };
    void Begin() { sequence_.fetch_add(1); }
    void End(uint64_t now) { progress_ = now; sequence_.fetch_add(1); }
    std::atomic<uint64_t> sequence_{0}, heartbeat_{0}, progress_{0};
    std::atomic<size_t> depth_{0};
    std::array<Cell, kDepth> frames_{};
};

// No alerts before the first tick. A slow load is reported as a stall too;
// this is diagnostic evidence, not a deadlock verdict or a recovery mechanism.
class StallAlarm {
public:
    enum class Event { None, Stalled, Recovered };
    Event Poll(uint64_t now, uint64_t heartbeat) {
        const bool stalled = heartbeat && now >= heartbeat && now - heartbeat >= 5000;
        if (stalled && (!active_ || now - lastReport_ >= 10000)) {
            active_ = true; lastReport_ = now; return Event::Stalled;
        }
        if (!stalled && active_) { active_ = false; return Event::Recovered; }
        return Event::None;
    }
private:
    bool active_ = false;
    uint64_t lastReport_ = 0;
};
} // namespace hsm::diagnostics

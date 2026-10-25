// Instrumenting call profiler core. Pure, no engine headers: the mod brackets each call it sees (a ProcessEvent, a
// Blueprint body run by the script VM, one of its own hooks) with Begin/End on the game thread and marks every frame;
// this keeps the call stack, the per-function totals, the frame times and an optional timeline of the longer calls.
//
// Times are ticks of any monotonic counter (the mod uses the TSC); the report and the trace take the ticks per
// millisecond. Self time is a call's own time minus the calls it made that the profiler saw: for a Blueprint function,
// its own nodes and the native functions it calls directly (those never reach ProcessEvent or the VM executor).
// Inclusive time is counted at the outermost active call only, so a recursive function is not counted twice.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hsm::prof {

enum Kind : uint8_t { kEvent, kScript, kHook, kMod, kKinds };
inline const char* KindName(uint8_t k) {
    static constexpr const char* kNames[kKinds] = {"event", "bp", "hook", "mod"};
    return k < kKinds ? kNames[k] : "?";
}

struct Stat {
    std::string name;
    uint8_t kind = 0;
    uint64_t calls = 0, selfTicks = 0, inclTicks = 0, maxTicks = 0;
    int active = 0;   // calls of this function on the stack now (recursion)
};

struct TraceEvent { uint32_t stat; uint32_t depth; uint64_t start, dur; };

struct Options {
    uint64_t traceMinTicks = 0;      // calls at least this long go on the timeline
    size_t traceMaxEvents = 0;       // 0 = no timeline
};

class Profiler {
public:
    static constexpr size_t kMaxDepth = 512;

    // Clears the previous capture. Calls already on the stack (begun in an earlier capture) still end correctly but
    // count nowhere.
    void Start(uint64_t now, Options o) {
        ++gen_;
        stats_.clear(); index_.clear(); frames_.clear(); trace_.clear();
        opts_ = o; start_ = now; stop_ = now; dropped_ = overflow_ = 0; calls_ = 0;
        recording_ = true;
    }
    void Stop(uint64_t now) { if (recording_) { recording_ = false; stop_ = now; } }
    bool recording() const { return recording_; }

    // Pushes a call; false (nothing pushed: do not call End) when not recording or too deep. name() is asked once per
    // new (key, kind) and must return its display name.
    template <class NameFn> bool Begin(const void* key, uint8_t kind, uint64_t now, NameFn&& name) {
        if (!recording_ || stack_.size() >= kMaxDepth) { if (recording_) ++overflow_; return false; }
        auto [it, fresh] = index_.try_emplace(Key{key, kind}, uint32_t(stats_.size()));
        if (fresh) { stats_.emplace_back(); stats_.back().name = name(); stats_.back().kind = kind; }
        ++stats_[it->second].active;
        stack_.push_back(Frame{it->second, gen_, now, 0});
        return true;
    }

    // Pops the call the matching Begin pushed.
    void End(uint64_t now) {
        if (stack_.empty()) return;
        const Frame f = stack_.back();
        stack_.pop_back();
        if (f.gen != gen_) return;                       // from an earlier capture
        const uint64_t incl = now > f.start ? now - f.start : 0;
        Stat& s = stats_[f.stat];
        --s.active;
        if (!recording_) return;                         // stopped while this call ran
        ++calls_;
        ++s.calls;
        s.selfTicks += incl > f.child ? incl - f.child : 0;
        if (s.active == 0) s.inclTicks += incl;
        s.maxTicks = std::max(s.maxTicks, incl);
        if (!stack_.empty() && stack_.back().gen == gen_) stack_.back().child += incl;
        if (opts_.traceMaxEvents && incl >= opts_.traceMinTicks) {
            if (trace_.size() < opts_.traceMaxEvents) trace_.push_back({f.stat, uint32_t(stack_.size()), f.start, incl});
            else ++dropped_;
        }
    }

    void FrameMark(uint64_t now) { if (recording_) frames_.push_back(now); }

    const std::vector<Stat>& stats() const { return stats_; }
    const std::vector<TraceEvent>& trace() const { return trace_; }
    const std::vector<uint64_t>& frameMarks() const { return frames_; }
    size_t depth() const { return stack_.size(); }
    uint64_t calls() const { return calls_; }
    uint64_t dropped() const { return dropped_; }
    uint64_t overflow() const { return overflow_; }
    uint64_t startTicks() const { return start_; }
    uint64_t durationTicks() const { return (recording_ ? start_ : stop_) - start_; }

    // Frame durations between consecutive marks, in ticks.
    std::vector<uint64_t> FrameTicks() const {
        std::vector<uint64_t> d;
        for (size_t i = 1; i < frames_.size(); ++i) d.push_back(frames_[i] - frames_[i - 1]);
        return d;
    }

private:
    struct Key {
        const void* p; uint8_t kind;
        bool operator==(const Key& o) const { return p == o.p && kind == o.kind; }
    };
    struct KeyHash { size_t operator()(const Key& k) const { return std::hash<const void*>()(k.p) * 31u + k.kind; } };
    struct Frame { uint32_t stat; uint32_t gen; uint64_t start, child; };

    std::vector<Stat> stats_;
    std::unordered_map<Key, uint32_t, KeyHash> index_;
    std::vector<Frame> stack_;
    std::vector<uint64_t> frames_;
    std::vector<TraceEvent> trace_;
    Options opts_;
    uint32_t gen_ = 0;
    bool recording_ = false;
    uint64_t start_ = 0, stop_ = 0, dropped_ = 0, overflow_ = 0, calls_ = 0;
};

// The value at fraction q (0..1) of v (nearest rank); 0 for an empty v.
inline uint64_t Percentile(std::vector<uint64_t> v, double q) {
    if (v.empty()) return 0;
    size_t k = std::min(v.size() - 1, size_t(q * double(v.size())));
    std::nth_element(v.begin(), v.begin() + std::ptrdiff_t(k), v.end());
    return v[k];
}

struct ReportInfo {
    double ticksPerMs = 1;
    double overheadNsPerCall = 0;    // measured cost of one Begin/End pair (shown as an estimate)
    std::string title, traceFile;
    size_t top = 60;                 // rows per table
};

// The text report of a stopped capture: frame times, the functions by self time and by inclusive time, the slowest
// frames (with their offset into the trace).
inline std::string Report(const Profiler& p, const ReportInfo& r) {
    std::string out;
    char b[512];
    auto ms = [&](uint64_t t) { return double(t) / r.ticksPerMs; };
    const double seconds = ms(p.durationTicks()) / 1000.0;
    const std::vector<uint64_t> ft = p.FrameTicks();
    const double frames = ft.empty() ? 0.0 : double(ft.size());
    const double per = frames > 0 ? frames : 1.0;   // per-frame columns fall back to totals without frame marks
    out += r.title + "\n\n";
    std::snprintf(b, sizeof b, "Captured %.2f s, %zu frames", seconds, ft.size());
    out += b;
    if (!ft.empty()) {
        uint64_t sum = 0, mx = 0;
        for (uint64_t t : ft) { sum += t; mx = std::max(mx, t); }
        std::snprintf(b, sizeof b, ": avg %.2f ms (%.0f fps), p50 %.2f, p95 %.2f, p99 %.2f, max %.2f ms",
                      ms(sum) / frames, frames > 0 && sum ? 1000.0 * frames / ms(sum) : 0.0, ms(Percentile(ft, 0.50)),
                      ms(Percentile(ft, 0.95)), ms(Percentile(ft, 0.99)), ms(mx));
        out += b;
    }
    out += "\n";
    std::snprintf(b, sizeof b, "Game-thread calls seen: %llu (%.0f per frame); profiler overhead about %.2f ms per frame (%.0f ns per call, inside the times below)\n",
                  (unsigned long long)p.calls(), double(p.calls()) / per, double(p.calls()) * r.overheadNsPerCall / 1e6 / per,
                  r.overheadNsPerCall);
    out += b;
    if (!r.traceFile.empty()) {
        std::snprintf(b, sizeof b, "Timeline: %zu calls written to %s (%llu more not kept)\n", p.trace().size(),
                      r.traceFile.c_str(), (unsigned long long)p.dropped());
        out += b;
    }
    if (p.overflow()) { std::snprintf(b, sizeof b, "Calls deeper than %zu not counted: %llu\n", Profiler::kMaxDepth, (unsigned long long)p.overflow()); out += b; }
    out += "\nKinds: event = called through ProcessEvent (ticks, input, hit/overlap delegates, timelines); bp = a Blueprint\n"
           "function body run by the script VM; hook = HSMMod's own hooks on that function; mod = an HSMMod subsystem.\n"
           "self = own time without the calls listed here (a Blueprint's nodes and the native engine functions it calls,\n"
           "physics queries included); incl = with them. Per-frame columns divide by the frames captured.\n";

    std::vector<uint32_t> order(p.stats().size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    auto table = [&](const char* heading, auto key) {
        std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t c) { return key(p.stats()[a]) > key(p.stats()[c]); });
        out += "\n";
        out += heading;
        out += "\n   self ms/f   incl ms/f    calls/f    avg us    max us  kind   function\n";
        for (size_t i = 0; i < order.size() && i < r.top; ++i) {
            const Stat& s = p.stats()[order[i]];
            if (!s.calls) continue;
            std::snprintf(b, sizeof b, "  %10.3f  %10.3f  %9.1f  %8.1f  %8.1f  %-5s  %s\n", ms(s.selfTicks) / per,
                          ms(s.inclTicks) / per, double(s.calls) / per, ms(s.inclTicks) * 1000.0 / double(s.calls),
                          ms(s.maxTicks) * 1000.0, KindName(s.kind), s.name.c_str());
            out += b;
        }
    };
    table("Top by self time", [](const Stat& s) { return s.selfTicks; });
    table("Top by inclusive time", [](const Stat& s) { return s.inclTicks; });

    if (!ft.empty()) {
        std::vector<uint32_t> idx(ft.size());
        for (uint32_t i = 0; i < idx.size(); ++i) idx[i] = i;
        const size_t n = std::min<size_t>(10, idx.size());
        std::partial_sort(idx.begin(), idx.begin() + std::ptrdiff_t(n), idx.end(), [&](uint32_t a, uint32_t c) { return ft[a] > ft[c]; });
        out += "\nSlowest frames (time into the capture, as on the timeline)\n";
        for (size_t i = 0; i < n; ++i) {
            std::snprintf(b, sizeof b, "  frame %6u  %8.2f ms  at %9.3f s\n", idx[i], ms(ft[idx[i]]),
                          ms(p.frameMarks()[idx[i]] - p.startTicks()) / 1000.0);
            out += b;
        }
    }
    return out;
}

// s as a JSON string body (no quotes).
inline void JsonEscape(std::string& out, std::string_view s) {
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (uint8_t(c) < 0x20) { char e[8]; std::snprintf(e, sizeof e, "\\u%04x", unsigned(uint8_t(c))); out += e; }
        else out += c;
    }
}

// The timeline in Chrome trace event format (chrome://tracing, ui.perfetto.dev, speedscope): one complete event per
// kept call on thread 1, one per frame on thread 2. sink(std::string_view) receives the text in pieces.
template <class Sink> void WriteTrace(const Profiler& p, double ticksPerMs, Sink&& sink) {
    const double perUs = ticksPerMs / 1000.0;
    std::string s = "{\"displayTimeUnit\":\"ms\",\"traceEvents\":[\n"
                    "{\"ph\":\"M\",\"pid\":1,\"tid\":1,\"name\":\"thread_name\",\"args\":{\"name\":\"Game thread\"}},\n"
                    "{\"ph\":\"M\",\"pid\":1,\"tid\":2,\"name\":\"thread_name\",\"args\":{\"name\":\"Frames\"}}";
    char b[160];
    const uint64_t t0 = p.startTicks();
    auto flush = [&] { if (s.size() > (1u << 16)) { sink(std::string_view(s)); s.clear(); } };
    for (const TraceEvent& e : p.trace()) {
        const Stat& st = p.stats()[e.stat];
        s += ",\n{\"ph\":\"X\",\"pid\":1,\"tid\":1,\"cat\":\"";
        s += KindName(st.kind);
        s += "\",\"name\":\"";
        JsonEscape(s, st.name);
        std::snprintf(b, sizeof b, "\",\"ts\":%.3f,\"dur\":%.3f}", double(e.start - t0) / perUs, double(e.dur) / perUs);
        s += b;
        flush();
    }
    const std::vector<uint64_t>& f = p.frameMarks();
    for (size_t i = 1; i < f.size(); ++i) {
        std::snprintf(b, sizeof b, ",\n{\"ph\":\"X\",\"pid\":1,\"tid\":2,\"cat\":\"frame\",\"name\":\"Frame %zu\",\"ts\":%.3f,\"dur\":%.3f}",
                      i - 1, double(f[i - 1] - t0) / perUs, double(f[i] - f[i - 1]) / perUs);
        s += b;
        flush();
    }
    s += "\n]}\n";
    sink(std::string_view(s));
}

}  // namespace hsm::prof

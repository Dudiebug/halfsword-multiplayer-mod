#include "agent/vmhook.h"
#include "agent/hsm_agent.h"
#include "log.h"
#include "ue/engine.h"
#include "ue/script_hook.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>

namespace hsm::agent {
namespace {
using namespace hsm::ue;

constexpr const char* kOwner = "vmhook"; // script-hook owner tag: `vmhook off` clears these rules only
constexpr int kLogCalls = 200;          // calls logged per log/block rule
constexpr int kStrikeLines = 400;       // strikes logged per "hits" rule
constexpr double kStrikeGapMs = 150.0;  // contacts of one collider on one fighter closer than this are one strike
constexpr size_t kStrikeBones = 6;      // bones listed per strike
constexpr uint64_t kCPF_OutParm = 0x100, kCPF_ReturnParm = 0x400;

std::string Plus(const std::string& s) {
    std::string out = s;
    for (char& ch : out) if (ch == '+') ch = ' ';
    return out;
}

std::string Num(double v) { char b[48]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

double NowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

double Len(const FVector& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

std::string ClassAndName(UObject* o) {
    return o ? ObjectName(reinterpret_cast<UObject*>(ClassOf(o))) + ":" + ObjectName(o) : "null";
}

// One parameter's value as text, from its reflected type and size. Unknown shapes print as type[size].
std::string Value(ScriptCall& call, FProperty* p) {
    const std::string name = PropertyName(p), type = PropertyType(p);
    const void* v = call.param(name);
    if (!v) return "?";
    const int32_t size = PropertySize(p);
    if (PropertyArrayDim(p) != 1) return type + "[" + std::to_string(size) + "x" + std::to_string(PropertyArrayDim(p)) + "]";
    auto at = [&](size_t i) { double d; std::memcpy(&d, static_cast<const uint8_t*>(v) + i * 8, 8); return d; };
    if (type == "DoubleProperty" && size == 8) return Num(at(0));
    if (type == "FloatProperty" && size == 4) { float f; std::memcpy(&f, v, 4); return Num(f); }
    if (type == "IntProperty" && size == 4) { int32_t i; std::memcpy(&i, v, 4); return std::to_string(i); }
    if (type == "Int64Property" && size == 8) { int64_t i; std::memcpy(&i, v, 8); return std::to_string(i); }
    if ((type == "ByteProperty" || type == "EnumProperty") && size == 1) return std::to_string(*static_cast<const uint8_t*>(v));
    if (type == "BoolProperty") { bool b = false; return GetBoolValue(p, v, b) ? (b ? "true" : "false") : "?"; }
    if (type == "NameProperty" && size == int32_t(sizeof(FName))) { FName n; std::memcpy(&n, v, sizeof n); return NameToString(n); }
    if (type == "StrProperty") return "\"" + ReadFString(v) + "\"";
    if (type == "ObjectProperty" && size == 8) { UObject* o; std::memcpy(&o, v, 8); return ClassAndName(o); }
    if (type == "StructProperty" && size == 24) return "(" + Num(at(0)) + "," + Num(at(1)) + "," + Num(at(2)) + ")";   // FVector / FRotator
    return type + "[" + std::to_string(size) + "]";
}

void LogCall(const char* tag, ScriptCall& call) {
    UFunction* fn = call.function();
    std::string line;
    for (FProperty* p : Params(fn)) {
        uint64_t flags = PropertyFlags(p);
        line += " " + PropertyName(p) + ((flags & kCPF_ReturnParm) ? "(ret)" : (flags & kCPF_OutParm) ? "(out)" : "") + "=" + Value(call, p);
    }
    HSM_LOG("vm %s %s on %s |%s", tag, ObjectName(reinterpret_cast<UObject*>(fn)).c_str(), ClassAndName(call.self()).c_str(), line.c_str());
}

// A double or float parameter as a double; false if absent or another type.
bool GetNumber(ScriptCall& call, const std::string& name, double& out) {
    FProperty* p = FindProperty(call.function(), name);
    const std::string type = p ? PropertyType(p) : "";
    if (type == "DoubleProperty") return call.get(name, out);
    float f;
    if (type != "FloatProperty" || !call.get(name, f)) return false;
    out = f;
    return true;
}

UObject* OwnerOf(UObject* component) {
    if (!component) return nullptr;
    Call c(component, "GetOwner");
    return c.valid() && c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

// ---- "hits": Deal Complex Damage calls folded into strikes. The game reports every physics step a striking component
// stays in contact as its own call (3 to 28 per punch, seen live), so one strike = the run of calls on one fighter by
// one component with gaps under kStrikeGapMs. Pointers are compared only, never followed after the strike opened:
// names are captured then.
struct Strike {
    UObject* victim = nullptr;
    UObject* collider = nullptr;
    int id = 0;
    std::string text;                    // "striker -> victim via collider"
    std::vector<std::string> bones;
    int contacts = 0, damaging = 0;
    double startMs = 0, lastMs = 0, maxSpeed = 0, maxImpulse = 0, damage = 0;
};

struct StrikeLog {
    std::function<UObject*()> localFighter;
    std::vector<Strike> open;
    int nextId = 1, linesLeft = kStrikeLines;

    void Write(const Strike& s) {
        if (linesLeft <= 0) return;
        if (--linesLeft == 0) HSM_LOG("vm: hits: %d strikes logged; further strikes are counted only (vmhook stats)", kStrikeLines);
        std::string bones;
        for (const std::string& b : s.bones) bones += (bones.empty() ? "" : ",") + b;
        HSM_LOG("vm strike #%d %s | bones %s | %d contacts over %.0f ms | speed max %.0f | impulse max %.0f | damage %s (%d contacts)",
                s.id, s.text.c_str(), bones.c_str(), s.contacts, s.lastMs - s.startMs, s.maxSpeed, s.maxImpulse,
                Num(s.damage).c_str(), s.damaging);
    }
    void FlushOlderThan(double cutoffMs) {
        auto done = [&](const Strike& s) { return s.lastMs < cutoffMs; };
        for (const Strike& s : open) if (done(s)) Write(s);
        open.erase(std::remove_if(open.begin(), open.end(), done), open.end());
    }
    Strike* Find(UObject* victim, UObject* collider) {
        for (Strike& s : open) if (s.victim == victim && s.collider == collider) return &s;
        return nullptr;
    }

    void Pre(ScriptCall& call) {
        const double now = NowMs();
        FlushOlderThan(now - kStrikeGapMs);
        UObject* victim = call.self();
        UObject* collider = call.get<UObject*>("Collided Component");
        Strike* s = Find(victim, collider);
        if (!s) {
            UObject* me = localFighter ? localFighter() : nullptr;
            UObject* striker = OwnerOf(collider);
            auto who = [&](UObject* o) { return (o ? ObjectName(o) : std::string("?")) + (o && o == me ? "(you)" : ""); };
            Strike n;
            n.victim = victim; n.collider = collider; n.id = nextId++; n.startMs = now;
            n.text = who(striker) + " -> " + who(victim) + " via " + ClassAndName(collider) +
                     (striker ? " of " + ObjectName(reinterpret_cast<UObject*>(ClassOf(striker))) : "");
            open.push_back(std::move(n));
            s = &open.back();
        }
        FName bone{};
        std::string b = call.get("Hit Bone", bone) ? NameToString(bone) : "?";
        if (s->bones.size() < kStrikeBones && std::find(s->bones.begin(), s->bones.end(), b) == s->bones.end()) s->bones.push_back(b);
        ++s->contacts;
        s->lastMs = now;
        s->maxSpeed = std::max(s->maxSpeed, Len(call.get<FVector>("Hit Velocity")));
        s->maxImpulse = std::max(s->maxImpulse, Len(call.get<FVector>("Hit Impulse")));
    }

    // After the body: the caller's Damage Out now holds this call's damage (before the body it is the previous call's).
    void Post(ScriptCall& call) {
        double dmg = 0;
        Strike* s = Find(call.self(), call.get<UObject*>("Collided Component"));
        if (!s || !GetNumber(call, "Damage Out", dmg) || !std::isfinite(dmg) || dmg <= 0) return;
        s->damage += dmg;
        ++s->damaging;
    }

    void FlushAll() { FlushOlderThan(1e300); }
};

std::vector<std::shared_ptr<StrikeLog>> g_hitLogs;   // game thread

std::string Err(const std::string& why) { return "{\"ok\":false,\"error\":" + JsonString(why) + "}"; }
constexpr const char* kUsage = "vmhook stats | hits | log <Class> <Function> | block <Class> <Function> | off ('+' = space)";
}  // namespace

void VmHookFlush() {
    for (auto& l : g_hitLogs) l->FlushAll();
}

std::string VmHookCommand(const std::vector<std::string>& args, std::function<UObject*()> localFighter) {
    if (args.empty()) return Err(kUsage);
    const std::string& sub = args[0];
    if (sub == "stats" && args.size() == 1) {
        VmHookFlush();
        ScriptHookStats s = GetScriptHookStats();
        char b[320];
        std::snprintf(b, sizeof b,
                      "{\"ok\":true,\"active\":%s,\"functions\":%d,\"executorCalls\":%llu,\"hookedCalls\":%llu,\"blocked\":%llu,"
                      "\"hookMs\":%.3f,\"missNs\":%.2f}",
                      ScriptHookActive() ? "true" : "false", s.functions, static_cast<unsigned long long>(s.executorCalls),
                      static_cast<unsigned long long>(s.hookedCalls), static_cast<unsigned long long>(s.blocked), s.hookNs / 1e6,
                      ScriptHookActive() ? MeasureScriptHookMissNs() : 0.0);
        return b;
    }
    if (sub == "off" && args.size() == 1) {
        VmHookFlush();
        g_hitLogs.clear();
        ClearScriptHooks(kOwner);
        return "{\"ok\":true}";
    }
    if (!ScriptHookActive() && (sub == "hits" || sub == "log" || sub == "block")) return Err("script VM hook not installed (see log)");
    int n = -1;
    if (sub == "hits" && args.size() == 1) {
        auto log = std::make_shared<StrikeLog>();
        log->localFighter = std::move(localFighter);
        g_hitLogs.push_back(log);
        n = HookScript(kOwner, "Willie_BP_C", "Deal Complex Damage",
                       [log](ScriptCall& call) { log->Pre(call); return false; },
                       [log](ScriptCall& call) { log->Post(call); });
    } else if ((sub == "log" || sub == "block") && args.size() == 3) {
        const bool block = sub == "block";
        auto left = std::make_shared<int>(kLogCalls);
        n = HookScript(kOwner, args[1], Plus(args[2]), [block, left](ScriptCall& call) {
            if (*left > 0) { --*left; LogCall(block ? "block" : "call", call); }
            if (block) call.ZeroOutputs();
            return block;
        });
    }
    if (n < 0) return Err(kUsage);
    // A rule that resolves to nothing yet stays registered and resolves again after each level load.
    return "{\"ok\":true,\"functions\":" + std::to_string(n) + (n ? "" : ",\"note\":\"not loaded yet; kept for the next level\"") + "}";
}

}  // namespace hsm::agent

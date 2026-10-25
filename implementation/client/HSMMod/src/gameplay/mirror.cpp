#include "gameplay/mirror.h"
#include "agent/hsm_agent.h"
#include "log.h"
#include "ue/script_hook.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace hsm::game {
using namespace hsm::ue;
using sync::V3;

namespace {
constexpr double kTruthKeepMs = 2000.0;             // longer than the longest playback delay (250 ms) plus a stall
constexpr double kLogEveryMs = 1000.0;
constexpr const char* kGuardOwner = "standin";

double Dist(const V3& a, const V3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 Lerp(const V3& a, const V3& b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
std::string Num(double v, const char* fmt = "%.2f") { char b[32]; std::snprintf(b, sizeof b, fmt, v); return b; }
std::string Err(const std::string& why) { return "{\"ok\":false,\"error\":" + JsonString(why) + "}"; }
}  // namespace

std::string Mirror::Command(const std::vector<std::string>& args, Adapter& adapter, bool online, bool fightLevel) {
    if (args.empty()) return Err("usage: mirror on <delayMs> <jitterMs> <lossPct> [offsetUu] [keeptick] | stats | off");
    if (args[0] == "off") {
        if (active_) adapter.DestroyFighter(standIn_, standInIndex_);   // checks the stand-in is still the one spawned
        Stop("off");
        return "{\"ok\":true}";
    }
    if (args[0] == "stats") return StatsJson();
    if (args[0] != "on" || args.size() < 4 || args.size() > 6) return Err("usage: mirror on <delayMs> <jitterMs> <lossPct> [offsetUu] [keeptick]");
    if (online) return Err("mirror is offline only (leave the session first)");
    if (!fightLevel) return Err("not in a fight level");
    if (active_) return Err("mirror already on (mirror off first)");
    const float delay = ParseArgFloat(args[1], 0, 1000), jitter = ParseArgFloat(args[2], 0, 500), loss = ParseArgFloat(args[3], 0, 100);
    float offset = 250.f;
    bool keepTick = false;
    for (size_t i = 4; i < args.size(); ++i) {
        if (args[i] == "keeptick") keepTick = true;
        else offset = ParseArgFloat(args[i], 100, 2000);
    }
    if (std::isnan(delay) || std::isnan(jitter) || std::isnan(loss) || std::isnan(offset))
        return Err("bad numbers (delay 0..1000 ms, jitter 0..500 ms, loss 0..100 %, offset 100..2000 uu)");
    UObject* me = adapter.PlayerWillie(0);
    if (!me) return Err("no local fighter");
    std::vector<uint8_t> passport;
    if (!adapter.ExportPassport(passport)) return Err("could not export the local passport");
    const FVector at = adapter.ActorLocation(me);
    const double yaw = adapter.ActorYaw(me) * 0.017453292519943295;
    const V3 right{-std::sin(yaw) * offset, std::cos(yaw) * offset, 0.0};   // the player's right
    Adapter::SpawnOptions o;
    o.blob = &passport;
    o.at = FVector{at.x + right.x, at.y + right.y, at.z};
    o.yawDeg = adapter.ActorYaw(me);
    const int32_t* team = Field<int32_t>(me, "Team Int");
    o.teamInt = team ? *team : 1;
    o.brain = false;
    UObject* s = adapter.SpawnFighter(o);
    if (!s) return Err("stand-in spawn failed (see log)");
    *this = Mirror{};
    adapter_ = &adapter;
    standIn_ = s;
    standInIndex_ = ObjectIndex(s);
    linkCfg_ = sync::SanitizeLink(sync::LinkConfig{delay, jitter, loss, 0x5EEDull});
    link_ = std::make_unique<sync::LinkSim<Packet, 256>>(linkCfg_);
    offsetUu_ = offset;
    offset_ = right;
    keepTick_ = keepTick;
    const double now = SteadyNowMs();
    streams_.Start(now);
    standIns_.Attach(kEntity, s);
    standIns_.SetStepObserver([this](uint8_t, const sync::DriverStep& st, const sync::FighterNow& n, const sync::PoseFrame& t,
                                     const FighterRig& rig) { Observe(st, n, t, rig); });
    nextLogMs_ = now + kLogEveryMs;
    active_ = true;
    InstallGuard();
    HSM_LOG("mirror: on (link %.0f +- %.0f ms, %.1f %% loss; stand-in %s %.0f uu to the right%s; the network stand-in path)",
            linkCfg_.delayMs, linkCfg_.jitterMs, linkCfg_.lossPct, ObjectName(s).c_str(), offsetUu_,
            keepTick_ ? "; its Willie tick runs" : "");
    return "{\"ok\":true,\"standin\":" + JsonString(ObjectName(s)) + "}";
}

// The stand-in must neither take nor deal damage: every "Deal Complex Damage" run on it, or whose striking component
// belongs to it (its body or its weapons), is blocked before its body runs, outputs zeroed. Invulnerable and Force
// Disable Dismemberment (FighterRig::Neutralise) stay set as a backstop. "Get Damage" reached by other paths (e.g. a
// blade left stuck in a body) is not guarded until the claim-based combat replaces this guard.
void Mirror::InstallGuard() {
    ClearScriptHooks(kGuardOwner);
    const int n = HookScript(kGuardOwner, "Willie_BP_C", "Deal Complex Damage", [this](ScriptCall& c) {
        if (!active_ || !standIn_) return false;
        bool block = IsStandIn(c.self());
        if (!block) {
            UObject* comp = nullptr;
            if (c.get("Collided Component", comp) && comp && adapter_)
                block = IsStandIn(adapter_->FighterOf(OuterOf(comp)));
        }
        if (!block) return false;
        c.ZeroOutputs();
        ++guardBlocked_;
        return true;
    });
    HSM_LOG("mirror: stand-in damage guard %s (%d function(s))", n > 0 ? "on" : "NOT INSTALLED (script VM hook unavailable)", n);
}

void Mirror::Stop(const char* why) {
    if (active_) {
        if (total_.frames) LogWindow(total_, "total");
        HSM_LOG("mirror: stopped (%s); damage guard blocked %llu call(s)", why, static_cast<unsigned long long>(guardBlocked_));
        ClearScriptHooks(kGuardOwner);
    }
    *this = Mirror{};
}

void Mirror::OnLevel() {
    if (active_) Stop("level changed");
}

void Mirror::Tick(Adapter& adapter, float dt) {
    if (!active_) return;
    const auto t0 = std::chrono::steady_clock::now();
    const double nowMs = SteadyNowMs();
    if (!adapter.StillValid(standIn_, standInIndex_) || !standIns_.ActorOf(kEntity)) { Stop("the stand-in is gone"); return; }

    // Owner side, as Session's owner-sync pump: the player streamed as entity kEntity, every message into the link. While
    // the player is dead or respawning nothing is sent, and the stand-in holds on its stale buffer.
    std::vector<std::pair<uint8_t, UObject*>> owned;
    if (UObject* me = adapter.PlayerWillie(0)) owned.push_back({kEntity, me});
    streams_.Tick(adapter, nowMs, dt, owned, [this, nowMs](const std::vector<uint8_t>& msg) { Send(msg, nowMs); });

    // Receiving side, as Session's Pose / Vitals handlers, then the stand-in servo.
    Receive(nowMs);
    std::vector<V3> avoid;
    if (V3 p; streams_.PelvisOf(kEntity, p)) avoid.push_back(p);
    standIns_.Tick(adapter, nowMs, dt, avoid);

    const double costMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    for (Window* w : {&window_, &total_}) {
        ++w->frames;
        w->costSumMs += costMs;
        w->costMaxMs = std::max(w->costMaxMs, costMs);
    }
    if (nowMs >= nextLogMs_) {
        nextLogMs_ = nowMs + kLogEveryMs;
        LogWindow(window_, "1s");
        window_ = Window{};
    }
}

void Mirror::Send(const std::vector<uint8_t>& msg, double nowMs) {
    if (msg.size() > kPoseMsgMaxBytes) { ++decodeFailed_; return; }
    MsgType t;
    PoseMsg m;
    sync::PoseFrame f;
    if (PeekType(msg.data(), msg.size(), t) && t == MsgType::Pose && Decode(msg.data(), msg.size(), m, &f)) {
        truth_.push_back(Truth{f.tsMs, {Add(f.bones[sync::kBoneHandR].pos, offset_), Add(f.bones[sync::kBoneHandL].pos, offset_)}});
        while (!truth_.empty() && truth_.front().tsMs + kTruthKeepMs < f.tsMs) truth_.pop_front();
    }
    Packet p;
    p.size = static_cast<uint16_t>(msg.size());
    std::copy(msg.begin(), msg.end(), p.bytes.begin());
    link_->Send(p, nowMs);
}

// The stream arrives shifted by offset_, so the stand-in stands beside the player rather than inside it.
void Mirror::Receive(double nowMs) {
    Packet p;
    while (link_->Receive(nowMs, p)) {
        MsgType t;
        if (!PeekType(p.bytes.data(), p.size, t)) { ++decodeFailed_; continue; }
        if (t == MsgType::Pose) {
            PoseMsg m;
            sync::PoseFrame f;
            if (!Decode(p.bytes.data(), p.size, m, &f)) { ++decodeFailed_; continue; }
            sync::OffsetPose(f, offset_);
            m.rootX += float(offset_.x); m.rootY += float(offset_.y); m.rootZ += float(offset_.z);
            standIns_.OnPose(m, f, nowMs);
        } else if (t == MsgType::Vitals) {
            VitalsMsg v;
            if (Decode(p.bytes.data(), p.size, v)) standIns_.OnVitals(v);
            else ++decodeFailed_;
        }
    }
}

// Tracking error is the stand-in's body against the target it was servoed to, read one step later: what this
// frame's sample asks for is where last frame's command aimed. Frames that placed the stand-in measure the jump, not
// the tracking, and are counted apart (placed/snaps).
void Mirror::Observe(const sync::DriverStep& s, const sync::FighterNow& now, const sync::PoseFrame& target, const FighterRig& rig) {
    for (Window* w : {&window_, &total_}) {
        w->struck += s.struck;
        if (!s.have) continue;
        ++w->targeted;
        w->extrapolated += s.info.extrapolated;
        w->stale += s.info.stale;
        w->cuts += s.info.cut;
        w->delaySum += s.info.delayMs;
        w->j95Max = std::max(w->j95Max, s.info.j95Ms);
        if (s.place) { ++w->placed; w->snaps += s.snap; continue; }
        if (s.pelvisErrUu >= 0.0) w->pelvis.Add(s.pelvisErrUu);
        for (int b : {sync::kBoneHandL, sync::kBoneHandR}) {
            const double e = sync::BoneError(now, target, b);
            if (e >= 0.0) w->hands.Add(e);
        }
        for (int k = 0; k < sync::kPoseWeapons; ++k) {
            V3 tip;
            if (target.weapons[k].present && rig.WeaponTip(k, tip)) w->tip.Add(Dist(tip, target.weapons[k].tip));
        }
        double be = 0.0;
        if (BufferError(target, s.info.playTsMs, be)) w->buffer.Add(be);
    }
}

// The buffered target's hands against the truth at the same owner time (interpolated between the two frames around
// it): what the link and the jitter buffer cost, before any servo error. The larger of the two hands.
bool Mirror::BufferError(const sync::PoseFrame& target, uint32_t playTsMs, double& out) const {
    if (truth_.size() < 2 || playTsMs < truth_.front().tsMs || playTsMs > truth_.back().tsMs) return false;
    auto hi = std::lower_bound(truth_.begin(), truth_.end(), playTsMs, [](const Truth& t, uint32_t ms) { return t.tsMs < ms; });
    if (hi == truth_.end()) return false;
    const Truth& b = *hi;
    const Truth& a = hi == truth_.begin() ? b : *(hi - 1);
    const double t = b.tsMs > a.tsMs ? double(playTsMs - a.tsMs) / double(b.tsMs - a.tsMs) : 0.0;
    out = std::max(Dist(target.bones[sync::kBoneHandR].pos, Lerp(a.hand[0], b.hand[0], t)),
                   Dist(target.bones[sync::kBoneHandL].pos, Lerp(a.hand[1], b.hand[1], t)));
    return true;
}

void Mirror::LogWindow(const Window& w, const char* label) const {
    const double n = w.frames ? double(w.frames) : 1.0;          // cost: per frame
    const double nt = w.targeted ? double(w.targeted) : 1.0;     // buffer health: per frame with a target
    const sync::LinkStats& l = link_->Stats();
    HSM_LOG("mirror %s: pelvis avg %.2f p99 %.2f max %.1f | hands avg %.2f p99 %.2f max %.1f | tip avg %.2f p99 %.2f max %.1f uu"
            " | buffer hand avg %.2f p99 %.2f | delay %.0f ms j95 %.0f | extrap %.1f%% stale %.1f%% | cuts %llu snaps %llu placed %llu"
            " struck %llu | sent %llu lost %llu | cost avg %.3f max %.3f ms",
            label, w.pelvis.Mean(), w.pelvis.Percentile(0.99), w.pelvis.Max(), w.hands.Mean(), w.hands.Percentile(0.99), w.hands.Max(),
            w.tip.Mean(), w.tip.Percentile(0.99), w.tip.Max(), w.buffer.Mean(), w.buffer.Percentile(0.99), w.delaySum / nt, w.j95Max,
            100.0 * double(w.extrapolated) / nt, 100.0 * double(w.stale) / nt, static_cast<unsigned long long>(w.cuts),
            static_cast<unsigned long long>(w.snaps), static_cast<unsigned long long>(w.placed), static_cast<unsigned long long>(w.struck),
            static_cast<unsigned long long>(l.sent), static_cast<unsigned long long>(l.lost), w.costSumMs / n, w.costMaxMs);
}

std::string Mirror::StatsJson() const {
    if (!active_) return "{\"ok\":true,\"active\":false}";
    const Window& w = total_;
    const double n = w.frames ? double(w.frames) : 1.0;
    const double nt = w.targeted ? double(w.targeted) : 1.0;
    auto err = [](const sync::ErrorStats& e) {
        return "{\"n\":" + std::to_string(e.Count()) + ",\"avg\":" + Num(e.Mean()) + ",\"p99\":" + Num(e.Percentile(0.99)) +
               ",\"max\":" + Num(e.Max()) + "}";
    };
    const sync::LinkStats& l = link_->Stats();
    return "{\"ok\":true,\"active\":true,\"frames\":" + std::to_string(w.frames) + ",\"targeted\":" + std::to_string(w.targeted) + ",\"pelvis\":" + err(w.pelvis) + ",\"hands\":" +
           err(w.hands) + ",\"tip\":" + err(w.tip) + ",\"buffer_hand\":" + err(w.buffer) + ",\"delay_ms\":" + Num(w.delaySum / nt, "%.1f") +
           ",\"j95_max_ms\":" + Num(w.j95Max, "%.1f") + ",\"extrap_pct\":" + Num(100.0 * double(w.extrapolated) / nt) +
           ",\"stale_pct\":" + Num(100.0 * double(w.stale) / nt) + ",\"cuts\":" + std::to_string(w.cuts) + ",\"snaps\":" +
           std::to_string(w.snaps) + ",\"placed\":" + std::to_string(w.placed) + ",\"struck\":" + std::to_string(w.struck) +
           ",\"sent\":" + std::to_string(l.sent) + ",\"lost\":" + std::to_string(l.lost) + ",\"decode_failed\":" +
           std::to_string(decodeFailed_) + ",\"guard_blocked\":" + std::to_string(guardBlocked_) + ",\"cost_avg_ms\":" +
           Num(w.costSumMs / n, "%.3f") + ",\"cost_max_ms\":" + Num(w.costMaxMs, "%.3f") + "}";
}

}  // namespace hsm::game

#include "gameplay/twin.h"
#include "agent/hsm_agent.h"
#include "gameplay/owner_sync.h"   // SteadyNowMs
#include "log.h"
#include "ue/script_hook.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hsm::game {
using namespace hsm::ue;
using sync::V3;

namespace {
constexpr double kTruthKeepMs = 1500.0;   // longer than the largest input delay plus the link's worst delay
constexpr double kLogEveryMs = 1000.0;
constexpr const char* kGuardOwner = "twin";
std::string Err(const std::string& why) { return "{\"ok\":false,\"error\":" + JsonString(why) + "}"; }
std::string Num(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f", v); return b; }
}  // namespace

std::string Twin::Command(const std::vector<std::string>& args, Adapter& adapter, bool online, bool fightLevel) {
    const char* usage = "usage: twin on <delayMs> <jitterMs> <lossPct> [inputDelayMs] [correct] [offsetUu] | stats | off";
    if (args.empty()) return Err(usage);
    if (args[0] == "off") {
        if (active_) adapter.DestroyFighter(twin_, twinIndex_);
        Stop("off");
        return "{\"ok\":true}";
    }
    if (args[0] == "stats") return StatsJson();
    if (args[0] != "on" || args.size() < 4 || args.size() > 7) return Err(usage);
    if (online) return Err("twin is offline only (leave the session first)");
    if (!fightLevel) return Err("not in a fight level");
    if (active_) return Err("twin already on (twin off first)");
    const float delay = ParseArgFloat(args[1], 0, 1000), jitter = ParseArgFloat(args[2], 0, 500), loss = ParseArgFloat(args[3], 0, 100);
    float inputDelay = 0.f, offset = 250.f;
    bool correct = false, haveInputDelay = false;
    for (size_t i = 4; i < args.size(); ++i) {
        if (args[i] == "correct") correct = true;
        else if (!haveInputDelay) { inputDelay = ParseArgFloat(args[i], 0, float(kInputDelayMaxMs)); haveInputDelay = true; }
        else offset = ParseArgFloat(args[i], 100, 2000);
    }
    if (std::isnan(delay) || std::isnan(jitter) || std::isnan(loss) || std::isnan(inputDelay) || std::isnan(offset))
        return Err("bad numbers (delay 0..1000 ms, jitter 0..500 ms, loss 0..100 %, input delay 0..200 ms, offset 100..2000 uu)");
    UObject* me = adapter.PlayerWillie(0);
    if (!me) return Err("no local fighter");
    std::vector<uint8_t> passport;
    if (!adapter.ExportPassport(passport)) return Err("could not export the local passport");
    const FVector at = adapter.ActorLocation(me);
    const double yaw = adapter.ActorYaw(me) * 0.017453292519943295;
    const V3 left{std::sin(yaw) * offset, -std::cos(yaw) * offset, 0.0};   // the player's left (the mirror stands right)
    Adapter::SpawnOptions o;
    o.blob = &passport;
    o.at = FVector{at.x + left.x, at.y + left.y, at.z};
    o.yawDeg = adapter.ActorYaw(me);
    const int32_t* team = Field<int32_t>(me, "Team Int");
    o.teamInt = team ? *team : 1;
    o.brain = true;   // a live fighter driven by input, as the host drives a client's under host physics
    UObject* t = adapter.SpawnFighter(o);
    if (!t) return Err("twin spawn failed (see log)");
    *this = Twin{};
    adapter_ = &adapter;
    twin_ = t;
    twinIndex_ = ObjectIndex(t);
    linkCfg_ = sync::SanitizeLink(sync::LinkConfig{delay, jitter, loss, 0x7171ull});
    inputLink_ = std::make_unique<sync::LinkSim<InPacket, 256>>(linkCfg_);
    sync::LinkConfig poseCfg = linkCfg_;
    poseCfg.seed = 0x7172ull;   // the two streams lose different packets
    poseLink_ = std::make_unique<sync::LinkSim<sync::PoseFrame, 128>>(poseCfg);
    offsetUu_ = offset;
    offset_ = left;
    inputDelayMs_ = uint16_t(std::lround(inputDelay));
    correct_ = correct;
    startMs_ = SteadyNowMs();
    nextLogMs_ = startMs_ + kLogEveryMs;
    active_ = true;
    InstallGuard();
    HSM_LOG("twin: on (link %.0f +- %.0f ms, %.1f %% loss; input delay %u ms; %s; twin %s %.0f uu to the left)", linkCfg_.delayMs,
            linkCfg_.jitterMs, linkCfg_.lossPct, unsigned(inputDelayMs_), correct_ ? "corrected by the pose stream" : "inputs only, no correction",
            ObjectName(t).c_str(), offsetUu_);
    return "{\"ok\":true,\"twin\":" + JsonString(ObjectName(t)) + "}";
}

// Neither the twin nor the player may hurt the other: a blow between them would end the run (and the twin's own death
// would stop it moving). Every "Deal Complex Damage" on the twin, or struck by one of its bodies or weapons, is blocked.
void Twin::InstallGuard() {
    ClearScriptHooks(kGuardOwner);
    const int n = HookScript(kGuardOwner, "Willie_BP_C", "Deal Complex Damage", [this](ScriptCall& c) {
        if (!active_ || !twin_) return false;
        bool block = c.self() == twin_;
        if (!block) {
            UObject* comp = nullptr;
            if (c.get("Collided Component", comp) && comp && adapter_) block = adapter_->FighterOf(OuterOf(comp)) == twin_;
        }
        if (!block) return false;
        c.ZeroOutputs();
        ++guardBlocked_;
        return true;
    });
    HSM_LOG("twin: damage guard %s (%d function(s))", n > 0 ? "on" : "NOT INSTALLED (script VM hook unavailable)", n);
}

void Twin::Stop(const char* why) {
    if (active_) {
        if (total_.frames) HSM_LOG("%s", sync::TwinLine(total_, "total").c_str());
        HSM_LOG("twin: stopped (%s); damage guard blocked %llu call(s), %llu undecodable", why,
                static_cast<unsigned long long>(guardBlocked_), static_cast<unsigned long long>(decodeFailed_));
        ClearScriptHooks(kGuardOwner);
    }
    *this = Twin{};
}

void Twin::OnLevel() {
    if (active_) Stop("level changed");
}

// The player's input frame goes out as the owner sync sender sends it: stamped to execute inputDelayMs_ after the press.
void Twin::Feed(const InputFrame& f) {
    if (!active_) return;
    const InputBatch b = sender_.Add(kEntity, f, uint32_t(std::llround(Clock(SteadyNowMs()))), inputDelayMs_);
    const std::vector<uint8_t> bytes = Encode(b);
    if (bytes.size() > InPacket{}.bytes.size()) { ++decodeFailed_; return; }
    InPacket p;
    p.size = uint16_t(bytes.size());
    std::copy(bytes.begin(), bytes.end(), p.bytes.begin());
    inputLink_->Send(p, SteadyNowMs());
}

// The player's own frame nearest owner time ts (the frames are a game frame apart: the nearest is close enough).
bool Twin::TruthAt(double ts, sync::PoseFrame& out) const {
    if (truth_.empty() || ts < double(truth_.front().tsMs)) return false;
    const sync::PoseFrame* best = &truth_.front();
    for (const sync::PoseFrame& f : truth_) {
        if (std::fabs(double(f.tsMs) - ts) < std::fabs(double(best->tsMs) - ts)) best = &f;
        if (double(f.tsMs) > ts) break;
    }
    out = *best;
    return true;
}

void Twin::Tick(Adapter& adapter, float dt, const Drive& drive) {
    if (!active_) return;
    if (!adapter.StillValid(twin_, twinIndex_)) { Stop("the twin is gone"); return; }
    UObject* me = adapter.PlayerWillie(0);
    if (!me) return;   // dead or respawning: nothing to copy, nothing to compare
    const double nowMs = SteadyNowMs(), clock = Clock(nowMs);
    const uint32_t ts = uint32_t(std::llround(clock));

    // The player, read as OwnerStreams reads it: the truth, and (with correct) its pose stream into the link.
    sync::PoseFrame mine;
    if ((meRig_.Bound() && meRig_.Willie() == me) || meRig_.Bind(me)) {
        if (meRig_.Read(ts, 0, &mine, nullptr)) {
            truth_.push_back(mine);
            while (!truth_.empty() && double(truth_.front().tsMs) + kTruthKeepMs < clock) truth_.pop_front();
            if (correct_) poseLink_->Send(mine, nowMs);
        }
    }

    // The inputs that crossed the link, played when due, as a receiving machine would play them.
    InPacket p;
    while (inputLink_->Receive(nowMs, p)) {
        InputBatch b;
        if (Decode(p.bytes.data(), p.size, b)) timeline_.Push(b, clock);
        else ++decodeFailed_;
    }
    const sync::TimelineStep step = timeline_.Step(clock, 1000.0 / 60.0);
    if (step.have) {
        InputFrame f = step.frame;
        f.seq = ++seq_;   // a prediction repeats the last frame: the applier takes every step as new
        applier_.Receive(f);
    }
    for (sync::TwinWindow* w : {&window_, &total_}) w->predicted += step.mainPredicted ? 1 : 0;   // what the input delay covers
    drive(applier_, twin_, dt);

    // The twin's bodies now; with correct, the poses that arrived pull it toward the truth they carry.
    if (!((twinRig_.Bound() && twinRig_.Willie() == twin_) || twinRig_.Bind(twin_))) return;
    sync::PoseFrame theirs;
    sync::FighterNow now;
    if (!twinRig_.Read(ts, 0, &theirs, &now)) return;
    const double ownerTs = clock - double(inputDelayMs_);   // the owner time the twin's inputs belong to
    if (correct_) {
        reconciler_.Record(ownerTs, now);
        sync::PoseFrame in;
        while (poseLink_->Receive(nowMs, in)) {
            sync::OffsetPose(in, offset_);   // the history is kept on owner time (ownerTs), the pose's own clock
            pending_.push_back(in);
            if (pending_.size() > 64) pending_.pop_front();
        }
        // A pose is compared once the twin has played that far (with an input delay it arrives before the twin gets there).
        while (!pending_.empty() && double(pending_.front().tsMs) <= reconciler_.NewestTs()) {
            const sync::PoseFrame f = pending_.front();
            pending_.pop_front();
            const sync::Reconciler::Result r = reconciler_.OnAuth(f);
            for (sync::TwinWindow* w : {&window_, &total_}) { w->corrections += r.applied ? 1 : 0; w->snaps += r.snap ? 1 : 0; }
            if (r.snap && now.valid[sync::kBonePelvis]) {   // too far to slew: placed where the truth puts it, as a cut places a stand-in
                const V3& to = f.bones[sync::kBonePelvis].pos;
                const V3& from = now.bones[sync::kBonePelvis].pos;
                const FVector at = adapter.ActorLocation(twin_);
                adapter.Teleport(twin_, FVector{at.x + to.x - from.x, at.y + to.y - from.y, at.z + to.z - from.z});
                reconciler_.Reset();
                pending_.clear();
                break;   // the history was of the old place
            }
        }
        twinRig_.Nudge(reconciler_.Step(double(dt) * 1000.0));
        // The pelvis is tied to the capsule: the root goes with the pelvis's correction, or the tie pulls the bodies back.
        const V3 v = reconciler_.CarriedLin(size_t(sync::kBonePelvis));
        const double dtS = double(dt);
        if (v.x != 0 || v.y != 0 || v.z != 0) adapter.NudgeActor(twin_, FVector{v.x * dtS, v.y * dtS, 0.0});   // the floor keeps its height
    }
    if (sync::PoseFrame truth; TruthAt(ownerTs, truth)) {
        const sync::TwinDiff d = sync::CompareTwin(truth, theirs, offset_);
        window_.Add(d);
        total_.Add(d);
    }
    if (nowMs >= nextLogMs_) {
        nextLogMs_ = nowMs + kLogEveryMs;
        HSM_LOG("%s", sync::TwinLine(window_, "1s").c_str());
        window_ = sync::TwinWindow{};
    }
}

std::string Twin::StatsJson() const {
    const sync::TwinWindow& t = total_;
    return std::string("{\"ok\":true,\"active\":") + (active_ ? "true" : "false") + ",\"correct\":" + (correct_ ? "true" : "false") +
           ",\"frames\":" + std::to_string(t.frames) + ",\"pelvisAvg\":" + Num(t.pelvis.Mean()) + ",\"pelvisP99\":" + Num(t.pelvis.Quantile(0.99)) +
           ",\"handsAvg\":" + Num(t.hands.Mean()) + ",\"handsP99\":" + Num(t.hands.Quantile(0.99)) + ",\"tipAvg\":" + Num(t.tip.Mean()) +
           ",\"predicted\":" + std::to_string(t.predicted) + ",\"corrections\":" + std::to_string(t.corrections) +
           ",\"snaps\":" + std::to_string(t.snaps) + "}";
}

}  // namespace hsm::game

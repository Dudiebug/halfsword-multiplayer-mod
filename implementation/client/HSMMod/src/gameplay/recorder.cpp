#include "gameplay/recorder.h"
#include "gameplay/adapter.h"
#include "gameplay/lobby.h"
#include "gameplay/object_sync.h"
#include "gameplay/shatter_sync.h"
#include "gameplay/owner_sync.h"
#include "gameplay/standin_rig.h"
#include "hsm_owner.h"
#include "session/hsm_lease.h"
#include "sync/hsm_pose_codec.h"
#include "util/hsm_names.h"
#include "ue/hook.h"
#include "ue/profiler.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <functional>

namespace hsm::game {
using namespace hsm::ue;

namespace {
constexpr double kRecEveryMs = 1000.0 / 60.0;
constexpr double kVitalsEveryMs = 1000.0 / 15.0;
constexpr double kRetryBindMs = 1000.0;      // a fighter whose rig does not bind is tried again this often
constexpr double kLogEveryMs = 10000.0;
constexpr double kPropScanMs = 1000.0;       // dropped weapons are looked for this often
constexpr double kWalkMs = 5000.0;           // the level's physics objects (ObjectSync::WalkForRecorder) this often
constexpr float kStrayCm = 3000.f;           // a prop body this far from the prop's first body is not part of it (SampleProps)
constexpr double kBreakCheckMs = 250.0;      // a breakable's IsRootBroken, as ObjectSync polls it
constexpr double kPropRestCheckMs = 1000.0 / 15.0;   // a prop at rest is read this often; a moving one every sample
constexpr double kHitEveryMs = 100.0;        // one Hit event per contact pair this often at most
constexpr size_t kMaxHitPairs = 4096;        // the pair map is cleared past this
constexpr size_t kMaxQueue = 64u << 20;      // bytes the writer may fall behind before samples are dropped
constexpr const char* kFistsPrefix = "Weapon_Fists";   // an empty hand's pseudo weapon (standin_rig.cpp)

// The Willie_BP_C fields the events come from (re/out/bp/Character/Blueprints/Willie_BP.txt). Two per hand: R, then L.
enum BoolField {
    kGrabbedR, kGrabbedL, kSheathR, kSheathL, kDrawR, kDrawL, kThrustR, kThrustL, kKickR, kKickL, kParryR, kParryL,
    kStuckR, kStuckL, kBeingGrabbed, kFallen, kDed, kBoolFields
};
constexpr const char* kBoolNames[kBoolFields] = {
    "Grabbed R", "Grabbed L", "R Weapon Sheathing", "L Weapon Sheathing", "Weapon UnSheathing R", "Weapon UnSheathing L",
    "R_Thrusting", "L_Thrusting", "Kicking  R", "Kicking  L", "Parrying R", "Parrying L", "R Hand Stuck", "L Hand Stuck",
    "Being Grabbed", "Fallen", "DED"};
constexpr const char* kGrabComponent[2] = {"Grab Component R", "Grab Component L"};
constexpr const char* kGrabBone[2] = {"Grab Bone R", "Grab Bone L"};
constexpr const char* kWeapon[2] = {"Weapon R", "Weapon L"};
constexpr const char* kSlot[rec::kSheathSlots] = {"Weapon Slot R 1", "Weapon Slot R 2", "Weapon Slot Back", "Weapon Slot L 1",
                                                  "Weapon Slot L 2"};
// hsm::Action order (adapter.cpp kActionNames): the Input events' text.
constexpr const char* kActionText[kActionCount] = {
    "Grab Left", "Grab Right", "Guard Left", "Guard Right", "Thrust", "Thrust Gamepad", "Jump", "Crouch Key",
    "Crouch Hold", "Run", "Swap Hands", "Toggle Camera Lock", "Key Forward", "Key Back", "Key Left", "Key Right",
    "Talk", "RightMouseButton", "MiddleMouseButton"};

float Finite(double v) { return std::isfinite(v) ? float(v) : 0.f; }
rec::F3 F3Of(const sync::V3& v) { return {Finite(v.x), Finite(v.y), Finite(v.z)}; }
rec::F3 F3Of(const FVector& v) { return {Finite(v.x), Finite(v.y), Finite(v.z)}; }
Quat16 QOf(const sync::Q4& q) { return PackQuat(float(q.x), float(q.y), float(q.z), float(q.w)); }
Quat16 QOf(const FQuat& q) { return PackQuat(float(q.x), float(q.y), float(q.z), float(q.w)); }

int32_t Offset(const UClass* cls, const char* name, const char* type) {
    FProperty* p = FindProperty(cls, name);
    return p && PropertyType(p) == type ? PropertyOffset(p) : -1;
}
}  // namespace

struct Recorder::Fighter {
    UObject* willie = nullptr;
    int32_t index = -1;
    uint16_t id = 0;               // 0 until bound
    double retryMs = 0;
    bool seen = false;             // in this sample's AllWillies
    FighterRig rig;                // the 23 pose bones, held weapons and grip, read as the owner streams them
    UObject* mesh = nullptr;
    std::vector<FName> extra;      // the mesh's other simulating bodies
    PreparedCall socket, linVelAt, angVel;
    std::array<FProperty*, kBoolFields> bools{};
    std::array<int32_t, 2> grabComp{-1, -1}, grabBone{-1, -1}, weapon{-1, -1};
    int32_t grabbedBy = -1;
    std::array<UObject*, 2> weaponActor{};
    std::array<int32_t, rec::kSheathSlots> slot{-1, -1, -1, -1, -1};
    std::array<UObject*, rec::kSheathSlots> slotActor{};   // this sample's sheathed weapons
    std::array<uint32_t, 2> weaponHash{};
    std::array<bool, 2> weaponFists{};
    rec::FighterState prev;
    bool havePrev = false;
    double nextVitalsMs = 0;
    rec::Vitals prevVitals;        // for VitalsEvents
    bool haveVitals = false;

    template <class T> T At(int32_t off) const {
        return off >= 0 ? *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(willie) + off) : T{};
    }
    bool Bool(BoolField b) const {
        bool v = false;
        return bools[b] && GetBoolValue(bools[b], reinterpret_cast<const uint8_t*>(willie) + PropertyOffset(bools[b]), v) && v;
    }
};

// The calls a prop body is read with, per primitive class.
struct Recorder::BodyCalls {
    PreparedCall toWorld, linVel, angVel, simulating;
    bool valid = false;
};

// A physics object: a piece of the level's furniture, a dropped weapon or a loose armour piece, every movable body of it.
struct Recorder::Prop {
    struct Body {
        UObject* comp = nullptr;
        int32_t index = -1;
        BodyCalls* calls = nullptr;
        bool gc = false;           // a geometry collection (a breakable): its root break is an event
        bool broken = false;
        bool simulates = false;    // checked at each scan; a sheathed weapon does not
        bool moving = false;       // the last frame written was not at rest
        bool stray = false;        // left out for good (SampleProps)
        bool known = false;        // pos and rot hold where it was last read
        FVector pos{};
        FQuat rot{};
        double nextReadMs = 0, nextBreakMs = 0;
    };
    UObject* actor = nullptr;
    int32_t index = -1;
    uint64_t id = 0;
    bool weapon = false;           // a held one is recorded in its fighter's frame instead
    std::vector<Body> bodies;
    bool tabled = false;           // its PropTable is written (on its first motion or break)
    bool seen = false;             // in this scan
};

Recorder::Recorder(bool on) : on_(on) {}

Recorder::~Recorder() {
    if (file_) Close(SteadyNowMs());
}

uint64_t Recorder::PropId(const UObject* actor) {
    const int32_t i = actor ? ObjectIndex(actor) : -1;
    return i > 0 ? uint64_t(i) : 0;
}

void Recorder::Hit(UObject* striker, UObject* struck, UObject* strikerFighter, UObject* struckFighter, bool strikerWeapon,
                   const FVector& impulse) {
    if (!file_ || !striker || !struck) return;
    const double now = SteadyNowMs();
    if (lastHit_.size() > kMaxHitPairs) lastHit_.clear();
    const bool lower = std::less<const UObject*>()(striker, struck);
    double& last = lastHit_[{lower ? striker : struck, lower ? struck : striker}];
    if (last > 0 && now - last < kHitEveryMs) return;
    last = now;
    rec::Event e;
    e.ts = now; e.kind = rec::Ev::Hit; e.hand = rec::kNoHand;
    e.subject = FighterId(strikerFighter); e.other = FighterId(struckFighter);
    e.propId = strikerWeapon ? PropId(striker) : 0;
    e.text = ObjectName(struck);
    e.x[0] = Finite(impulse.x); e.x[1] = Finite(impulse.y); e.x[2] = Finite(impulse.z);
    e.x[3] = Finite(std::sqrt(impulse.x * impulse.x + impulse.y * impulse.y + impulse.z * impulse.z));
    Note(e);
}

uint16_t Recorder::FighterId(const UObject* willie) const {
    if (!willie) return 0;
    for (const auto& f : fighters_) if (f->willie == willie && f->id) return f->id;
    return 0;
}

void Recorder::Tick(Adapter& adapter, UObject* gameMode, bool fightLevel, const std::string& level, UObject* localWillie, float dt) {
    if (!on_) return;
    const double now = SteadyNowMs();
    if (gameMode != gameMode_) {
        if (file_) Close(now);
        gameMode_ = gameMode;
        if (fightLevel) Open(level, now);
    }
    if (!file_ || now < nextSampleMs_) return;
    // 60 Hz on the steady clock; a slower game gets one sample per frame, never a burst to catch up.
    nextSampleMs_ = std::max(nextSampleMs_ + kRecEveryMs, now - kRecEveryMs);
    {
        ue::prof::Scope scope("HSMMod recorder", ue::prof::Kind::kMod);
        Sample(adapter, localWillie, dt, now);
    }
    const double cost = SteadyNowMs() - now;
    winCostMs_ += cost;
    winMaxMs_ = std::max(winMaxMs_, cost);
    ++winSamples_;
    if (now >= nextLogMs_) {
        nextLogMs_ = now + kLogEveryMs;
        size_t bound = 0;
        for (const auto& f : fighters_) bound += f->id ? 1 : 0;
        HSM_LOG("recorder: %llu samples (%.1f Hz), %zu fighters, %llu frames, %zu props, %llu prop frames, %llu input samples, "
                "%llu events, %.1f MB queued, writer behind max %zu KB, dropped %llu; sample avg %.2f ms max %.2f ms, level walk max "
                "%.2f ms", (unsigned long long)samples_, winSamples_ * 1000.0 / kLogEveryMs, bound, (unsigned long long)frames_,
                props_.size(), (unsigned long long)propFrames_, (unsigned long long)inputs_, (unsigned long long)events_,
                queued_ / 1e6, queueMax_ >> 10, (unsigned long long)dropped_, winSamples_ ? winCostMs_ / double(winSamples_) : 0.0,
                winMaxMs_, winWalkMaxMs_);
        winSamples_ = 0; winCostMs_ = winMaxMs_ = winWalkMaxMs_ = 0;
    }
}

void Recorder::Note(const rec::Event& e) {
    if (!file_) return;
    rec::Append(batch_, rec::Chunk::Event, e);
    ++events_;
}

void Recorder::Marker(const std::string& text) {
    rec::Event e;
    e.ts = SteadyNowMs(); e.kind = rec::Ev::Marker; e.text = text;
    Note(e);
    HSM_LOG("recorder: marker '%s'%s", text.c_str(), file_ ? "" : " ignored (not recording)");
}

void Recorder::Open(const std::string& level, double nowMs) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t base[64];
    std::swprintf(base, 64, L"HSMRec-%04u%02u%02u-%02u%02u%02u", unsigned(st.wYear), unsigned(st.wMonth), unsigned(st.wDay),
                  unsigned(st.wHour), unsigned(st.wMinute), unsigned(st.wSecond));
    const std::wstring path = InstanceFilePath(base, L".hsmrec");
    file_ = _wfopen(path.c_str(), L"wb");
    name_.clear();
    for (size_t i = path.find_last_of(L"\\/") + 1; i < path.size(); ++i) name_.push_back(char(path[i]));   // ASCII by construction
    if (!file_) { HSM_LOG("recorder: cannot create %s; this level is not recorded", name_.c_str()); return; }
    rec::FileHeader h;
    h.buildKey = BuildKey(); h.modBuild = ModBuild(); h.level = level; h.steadyMs = nowMs;
    h.wallMs = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    batch_ = rec::EncodeHeader(h);
    rec::Event e;
    e.ts = nowMs; e.kind = rec::Ev::LevelStart; e.text = level;
    rec::Append(batch_, rec::Chunk::Event, e);
    samples_ = frames_ = queued_ = dropped_ = 0;
    events_ = 1;
    queueMax_ = 0;
    winSamples_ = 0; winCostMs_ = winMaxMs_ = 0;
    openedMs_ = nextSampleMs_ = nowMs;
    nextLogMs_ = nowMs + kLogEveryMs;
    fighters_.clear();
    nextId_ = 1;
    props_.clear();
    nextPropScanMs_ = nextWalkMs_ = nowMs;
    walker_ = std::make_unique<ObjectSync>();
    calls_.clear();
    lastHit_.clear();
    propFrames_ = inputs_ = 0;
    input_ = InputCapture{};
    stop_ = writeFailed_ = false;
    pending_.clear();
    Push();
    writer_ = std::thread([this] { WriterLoop(); });
    HSM_LOG("recorder: recording %s to %s", level.c_str(), name_.c_str());
}

void Recorder::Close(double nowMs) {
    rec::Event e;
    e.ts = nowMs; e.kind = rec::Ev::LevelEnd;
    Note(e);
    Push();
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_one();
    if (writer_.joinable()) writer_.join();
    std::fclose(file_);
    file_ = nullptr;
    size_t ids = nextId_ - 1u;
    HSM_LOG("recorder: closed %s: %.1f s, %llu samples, %zu fighters seen, %llu frames, %llu prop frames, %llu input samples, "
            "%llu events, %.1f MB, dropped %llu samples%s", name_.c_str(), (nowMs - openedMs_) / 1000.0, (unsigned long long)samples_, ids,
            (unsigned long long)frames_, (unsigned long long)propFrames_, (unsigned long long)inputs_, (unsigned long long)events_, queued_ / 1e6, (unsigned long long)dropped_, writeFailed_ ? "; DISK WRITE FAILED" : "");
    fighters_.clear();
    props_.clear();
    walker_.reset();
    calls_.clear();
    lastHit_.clear();
}

void Recorder::Push() {
    if (batch_.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (pending_.size() + batch_.size() > kMaxQueue) {
            ++dropped_;
        } else {
            pending_.insert(pending_.end(), batch_.begin(), batch_.end());
            queued_ += batch_.size();
            queueMax_ = std::max(queueMax_, pending_.size());
        }
    }
    batch_.clear();
}

// Writes what the game thread queued four times a second, and the rest when told to stop. The game thread never waits
// on the disk: it only holds the lock to append.
void Recorder::WriterLoop() {
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop_; });
        std::vector<uint8_t> out;
        out.swap(pending_);
        const bool stop = stop_;
        lock.unlock();
        if (!out.empty() && (std::fwrite(out.data(), 1, out.size(), file_) != out.size() || std::fflush(file_) != 0)) writeFailed_ = true;
        lock.lock();
        if (stop && pending_.empty()) return;
    }
}

bool Recorder::Bind(Fighter& f, Adapter& adapter, UObject* willie, UObject* localWillie) {
    if (!f.rig.Bind(willie)) return false;
    UObject* mesh = adapter.Mesh(willie);
    if (!mesh) return false;
    const UClass* mc = ClassOf(mesh);
    f.socket = PreparedCall(mc, "GetSocketTransform", {"InSocketName", "TransformSpace", "ReturnValue"});
    f.linVelAt = PreparedCall(mc, "GetPhysicsLinearVelocityAtPoint", {"Point", "BoneName", "ReturnValue"});
    f.angVel = PreparedCall(mc, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
    if (!f.socket.valid() || !f.linVelAt.valid() || !f.angVel.valid()) return false;
    f.mesh = mesh;

    // The table: the 23 pose bones first (the sync stream's order and parents), then every other bone with a simulating
    // body, parented to its nearest recorded ancestor.
    rec::FighterTable t;
    t.recId = nextId_;
    t.flags = willie == localWillie ? rec::kFighterLocal : rec::kFighterBot;
    t.name = ObjectName(willie);
    t.cls = ObjectName(reinterpret_cast<UObject*>(ClassOf(willie)));
    for (int i = 0; i < sync::kPoseBones; ++i)
        t.bones.push_back({sync::kPoseBoneNames[i], i == 0 ? rec::kNoParent : uint16_t(sync::kPoseParent[i]), sync::kPoseHasBody[i]});
    auto indexOf = [&t](const std::string& n) {
        for (size_t i = 0; i < t.bones.size(); ++i) if (t.bones[i].name == n) return int(i);
        return -1;
    };
    Call count(mesh, "GetNumBones");
    const int32_t bones = count.invoke() ? count.get<int32_t>("ReturnValue") : 0;
    std::vector<FName> extraNames;
    for (int32_t b = 0; b < bones && t.bones.size() < rec::kMaxBones; ++b) {
        Call bn(mesh, "GetBoneName");
        if (!bn.set<int32_t>("BoneIndex", b).invoke()) break;
        const FName n = bn.get<FName>("ReturnValue");
        const std::string name = NameToString(n);
        if (name.empty() || name == "None" || indexOf(name) >= 0) continue;
        Call sim(mesh, "IsSimulatingPhysics");
        if (!sim.setName("BoneName", n).invoke() || !sim.get<bool>("ReturnValue")) continue;
        t.bones.push_back({name, rec::kNoParent, true});
        extraNames.push_back(n);
    }
    for (size_t i = sync::kPoseBones; i < t.bones.size(); ++i) {
        FName at = extraNames[i - sync::kPoseBones];
        for (int depth = 0; depth < 64; ++depth) {
            Call parent(mesh, "GetParentBone");
            if (!parent.setName("BoneName", at).invoke()) break;
            at = parent.get<FName>("ReturnValue");
            const std::string pn = NameToString(at);
            if (pn.empty() || pn == "None") break;
            const int k = indexOf(pn);
            if (k >= 0) { t.bones[i].parent = uint16_t(k); break; }
        }
    }
    f.extra = std::move(extraNames);

    const UClass* wc = ClassOf(willie);
    std::string missing;
    for (int b = 0; b < kBoolFields; ++b) {
        FProperty* p = FindProperty(wc, kBoolNames[b]);
        f.bools[b] = p && PropertyType(p) == "BoolProperty" ? p : nullptr;
        if (!f.bools[b]) missing += std::string(missing.empty() ? "" : ", ") + kBoolNames[b];
    }
    for (int h = 0; h < 2; ++h) {
        f.grabComp[h] = Offset(wc, kGrabComponent[h], "ObjectProperty");
        f.grabBone[h] = Offset(wc, kGrabBone[h], "NameProperty");
        f.weapon[h] = Offset(wc, kWeapon[h], "ObjectProperty");
        if (f.grabComp[h] < 0) missing += std::string(missing.empty() ? "" : ", ") + kGrabComponent[h];
        if (f.grabBone[h] < 0) missing += std::string(missing.empty() ? "" : ", ") + kGrabBone[h];
        if (f.weapon[h] < 0) missing += std::string(missing.empty() ? "" : ", ") + kWeapon[h];
    }
    for (size_t k = 0; k < rec::kSheathSlots; ++k) {
        f.slot[k] = Offset(wc, kSlot[k], "ObjectProperty");
        if (f.slot[k] < 0) missing += std::string(missing.empty() ? "" : ", ") + kSlot[k];
    }
    f.grabbedBy = Offset(wc, "Grabbed By Willie", "ObjectProperty");
    if (f.grabbedBy < 0) missing += std::string(missing.empty() ? "" : ", ") + "Grabbed By Willie";

    f.id = nextId_++;
    rec::Append(batch_, rec::Chunk::FighterTable, t);
    size_t bodies = 0;
    for (const auto& b : t.bones) bodies += b.hasBody ? 1 : 0;
    HSM_LOG("recorder: #%u %s (%s): %zu bones, %zu bodies (%zu past the pose bones)%s%s", unsigned(f.id), t.name.c_str(),
            willie == localWillie ? "local" : "bot", t.bones.size(), bodies, f.extra.size(), missing.empty() ? "" : "; no field ",
            missing.c_str());
    return true;
}

void Recorder::ReadState(const Fighter& f, rec::FighterState& s) const {
    for (int h = 0; h < 2; ++h) {
        s.grabbing[h] = f.Bool(BoolField(kGrabbedR + h));
        if (s.grabbing[h]) {
            UObject* comp = f.At<UObject*>(f.grabComp[h]);
            UObject* owner = comp ? OuterOf(comp) : nullptr;
            s.grabbedFighter[h] = FighterId(owner);
            // A fighter: the bone held. Anything else: the actor grabbed, by prop id and name.
            if (s.grabbedFighter[h]) s.grabBone[h] = f.grabBone[h] >= 0 ? NameToString(f.At<FName>(f.grabBone[h])) : "";
            else if (owner) { s.grabbedProp[h] = PropId(owner); s.grabBone[h] = ObjectName(owner); }
        }
        s.holding[h] = f.weaponActor[h] && !f.weaponFists[h];
        s.weaponClass[h] = s.holding[h] ? f.weaponHash[h] : 0;
        s.weaponProp[h] = s.holding[h] ? PropId(f.weaponActor[h]) : 0;
        s.sheathing[h] = f.Bool(BoolField(kSheathR + h));
        s.drawing[h] = f.Bool(BoolField(kDrawR + h));
        s.thrusting[h] = f.Bool(BoolField(kThrustR + h));
        s.kicking[h] = f.Bool(BoolField(kKickR + h));
        s.parrying[h] = f.Bool(BoolField(kParryR + h));
        s.stuck[h] = f.Bool(BoolField(kStuckR + h));
    }
    s.beingGrabbed = f.Bool(kBeingGrabbed);
    if (s.beingGrabbed) s.grabbedBy = FighterId(f.At<UObject*>(f.grabbedBy));
    for (size_t k = 0; k < rec::kSheathSlots; ++k) s.sheathed[k] = PropId(f.slotActor[k]);
    s.fallen = f.Bool(kFallen);
    s.dead = f.Bool(kDed);
}

void Recorder::ReadFrame(Fighter& f, float dt, double nowMs) {
    sync::PoseFrame pose;
    if (!f.rig.Read(uint32_t(nowMs), 0, &pose, nullptr)) return;
    rec::FighterFrame fr;
    fr.ts = nowMs; fr.dt = dt; fr.recId = f.id;
    fr.bones.reserve(sync::kPoseBones + f.extra.size());
    for (int i = 0; i < sync::kPoseBones; ++i) {
        const sync::BoneSample& b = pose.bones[i];
        rec::BoneFrame o;
        o.pos = F3Of(b.pos); o.rot = QOf(b.rot); o.hasBody = sync::kPoseHasBody[i];
        if (o.hasBody) { o.vel = F3Of(b.vel); o.angVelDeg = F3Of(b.angVelDeg); }
        fr.bones.push_back(o);
    }
    for (const FName& n : f.extra) {
        rec::BoneFrame o;
        o.hasBody = true;
        f.socket.put(0, n);   // TransformSpace stays RTS_World (0), as FighterRig::Read
        if (f.socket.invoke(f.mesh)) {
            const FTransform t = f.socket.take<FTransform>(2);
            o.pos = F3Of(t.translation); o.rot = QOf(t.rotation);
            f.linVelAt.put(0, t.translation).put(1, n);
            if (f.linVelAt.invoke(f.mesh)) o.vel = F3Of(f.linVelAt.take<FVector>(2));
            f.angVel.put(0, n);
            if (f.angVel.invoke(f.mesh)) o.angVelDeg = F3Of(f.angVel.take<FVector>(1));
        }
        fr.bones.push_back(o);
    }
    for (int w = 0; w < sync::kPoseWeapons; ++w) {
        const sync::WeaponSample& s = pose.weapons[w];
        rec::WeaponFrame& o = fr.weapons[w];
        o.present = s.present;
        if (!s.present) continue;
        o.classHash = s.classHash; o.pos = F3Of(s.pos); o.rot = QOf(s.rot); o.vel = F3Of(s.vel); o.angVelDeg = F3Of(s.angVelDeg);
        o.base = F3Of(s.base); o.tip = F3Of(s.tip);
    }
    fr.grip = sync::detail::PackGrip(pose.grip);
    rec::Append(batch_, rec::Chunk::FighterFrame, fr);
    ++frames_;
}

void Recorder::Sample(Adapter& adapter, UObject* localWillie, float dt, double nowMs) {
    ++samples_;
    for (auto& f : fighters_) f->seen = false;
    for (UObject* w : adapter.AllWillies()) {
        const int32_t index = ObjectIndex(w);
        auto it = std::find_if(fighters_.begin(), fighters_.end(), [&](const auto& f) { return f->willie == w && f->index == index; });
        if (it == fighters_.end()) {
            fighters_.push_back(std::make_unique<Fighter>());
            it = fighters_.end() - 1;
            (*it)->willie = w;
            (*it)->index = index;
        }
        (*it)->seen = true;
    }
    fighters_.erase(std::remove_if(fighters_.begin(), fighters_.end(), [](const auto& f) { return !f->seen; }), fighters_.end());

    std::vector<rec::Event> events;
    for (auto& fp : fighters_) {
        Fighter& f = *fp;
        if (!f.id) {
            if (nowMs < f.retryMs) continue;
            if (!Bind(f, adapter, f.willie, localWillie)) { f.retryMs = nowMs + kRetryBindMs; continue; }
        }
        if (!f.rig.StillValid()) continue;   // going away: AllWillies drops it next sample
        for (int h = 0; h < 2; ++h) {        // the held weapon's class, worked out when the hand's actor changes
            UObject* a = f.At<UObject*>(f.weapon[h]);
            if (a == f.weaponActor[h]) continue;
            f.weaponActor[h] = a;
            const std::string cls = a ? ObjectName(reinterpret_cast<UObject*>(ClassOf(a))) : "";
            f.weaponHash[h] = a ? sync::WeaponClassHash(cls.c_str()) : 0;
            f.weaponFists[h] = a && StartsWithNameAscii(cls, kFistsPrefix);
        }
        for (size_t k = 0; k < rec::kSheathSlots; ++k) f.slotActor[k] = f.At<UObject*>(f.slot[k]);
        ReadFrame(f, dt, nowMs);
        rec::FighterState s;
        ReadState(f, s);
        if (f.havePrev) rec::EdgeEvents(f.prev, s, nowMs, f.id, events);
        f.prev = std::move(s);
        f.havePrev = true;
        if (nowMs >= f.nextVitalsMs) {
            f.nextVitalsMs = nowMs + kVitalsEveryMs;
            VitalsMsg v;
            if (adapter.ReadVitals(f.willie, 0, v)) {
                rec::Vitals o;
                o.ts = nowMs; o.recId = f.id; o.health = v.health; o.consciousness = v.consciousness; o.stamina = v.stamina;
                o.bleeding = v.bleeding; o.partHealth.assign(v.partHealth.begin(), v.partHealth.end());
                o.flags = v.flags; o.stateMask = v.state; o.severMask = v.severed; o.dislocMask = v.dislocated;
                rec::Append(batch_, rec::Chunk::Vitals, o);
                if (f.haveVitals) rec::VitalsEvents(f.prevVitals, o, events);
                f.prevVitals = std::move(o);
                f.haveVitals = true;
            }
        }
    }
    for (const rec::Event& e : events) Note(e);
    ScanProps(adapter, nowMs);
    SampleProps(adapter, nowMs);
    SampleInput(adapter, localWillie, nowMs);
    Push();
}

float Recorder::Dist(const FVector& a, const FVector& b) {
    const double x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return float(std::sqrt(x * x + y * y + z * z));
}

// A body welded to the first one reads the first one's state (a weapon's BaseMesh and Weapon): the same transform at
// the first look. It would only double the frames, so it is left out.
void Recorder::Dedupe(Prop& p) {
    auto read = [](Prop::Body& b) {
        if (!b.calls || !b.calls->toWorld.invoke(b.comp)) return false;
        const FTransform t = b.calls->toWorld.take<FTransform>(0);
        b.pos = t.translation; b.rot = t.rotation; b.known = true;
        return true;
    };
    if (p.bodies.size() < 2 || !read(p.bodies[0])) return;
    const FQuat q0 = p.bodies[0].rot;
    std::vector<Prop::Body> kept{p.bodies[0]};
    for (size_t i = 1; i < p.bodies.size(); ++i) {
        Prop::Body& b = p.bodies[i];
        if (read(b)) {
            const double dot = std::fabs(q0.x * b.rot.x + q0.y * b.rot.y + q0.z * b.rot.z + q0.w * b.rot.w);
            if (Dist(b.pos, p.bodies[0].pos) < 0.5f && dot > 0.9999) continue;
        }
        kept.push_back(b);
    }
    p.bodies = std::move(kept);
}

Recorder::BodyCalls* Recorder::CallsFor(UObject* comp) {
    const UClass* cls = ClassOf(comp);
    auto& c = calls_[cls];
    if (!c) {
        c = std::make_unique<BodyCalls>();
        c->toWorld = PreparedCall(cls, "K2_GetComponentToWorld", {"ReturnValue"});
        c->linVel = PreparedCall(cls, "GetPhysicsLinearVelocity", {"BoneName", "ReturnValue"});
        c->angVel = PreparedCall(cls, "GetPhysicsAngularVelocityInDegrees", {"BoneName", "ReturnValue"});
        c->simulating = PreparedCall(cls, "IsSimulatingPhysics", {"BoneName", "ReturnValue"});
        c->valid = c->toWorld.valid() && c->linVel.valid() && c->angVel.valid() && c->simulating.valid();
    }
    return c->valid ? c.get() : nullptr;
}

// Dropped weapons (not fists or feet) every kPropScanMs, every physics object of the level (furniture, loose armour,
// spawned objects; ObjectSync's walk) every kWalkMs. Each with every movable body, root first.
void Recorder::ScanProps(Adapter& adapter, double nowMs) {
    const bool weapons = nowMs >= nextPropScanMs_, walk = nowMs >= nextWalkMs_ && walker_;
    if (!weapons && !walk) return;
    if (weapons) nextPropScanMs_ = nowMs + kPropScanMs;
    if (walk) nextWalkMs_ = nowMs + kWalkMs;
    for (auto& p : props_) if (p->weapon ? weapons : walk) p->seen = false;
    auto add = [&](UObject* a, bool weapon, const std::vector<UObject*>& comps) {
        const int32_t index = ObjectIndex(a);
        auto it = std::find_if(props_.begin(), props_.end(), [&](const auto& p) { return p->actor == a && p->index == index; });
        if (it == props_.end()) {
            if (comps.empty() || !PropId(a)) return;
            auto p = std::make_unique<Prop>();
            p->actor = a; p->index = index; p->id = PropId(a); p->weapon = weapon;
            for (UObject* c : comps) {
                Prop::Body b;
                b.comp = c; b.index = ObjectIndex(c); b.calls = CallsFor(c); b.gc = IsGeometryCollection(c);
                if (b.calls || b.gc) p->bodies.push_back(b);
            }
            if (p->bodies.empty()) return;
            Dedupe(*p);
            props_.push_back(std::move(p));
            it = props_.end() - 1;
        }
        Prop& p = **it;
        p.seen = true;
        for (Prop::Body& b : p.bodies) {
            if (!b.calls || !adapter.StillValid(b.comp, b.index)) { b.simulates = false; continue; }
            b.calls->simulating.put(0, FName{});
            b.simulates = !b.stray && b.calls->simulating.invoke(b.comp) && b.calls->simulating.take<bool>(1);
        }
    };
    if (weapons)
        for (UObject* a : adapter.WeaponActors())
            if (!adapter.IsBodyWeapon(a)) add(a, true, walker_ ? walker_->RecorderBodies(adapter, a) : std::vector<UObject*>{});
    if (walk) {
        const double t0 = SteadyNowMs();
        for (ObjectSync::WalkedObject& w : walker_->WalkForRecorder(adapter)) add(w.actor, false, w.bodies);
        winWalkMaxMs_ = std::max(winWalkMaxMs_, SteadyNowMs() - t0);
    }
    props_.erase(std::remove_if(props_.begin(), props_.end(), [](const auto& p) { return !p->seen; }), props_.end());
}

// Each body that simulates (of a prop in no fighter's hand): a frame every sample while it moves, one more when it comes
// to rest (flagged kPropAtRest), nothing while it lies still. A breakable's root break is a Break event. The prop's table
// goes out before its first frame or event.
void Recorder::SampleProps(Adapter& adapter, double nowMs) {
    auto table = [&](Prop& p) {
        if (p.tabled) return;
        rec::PropTable tb;
        tb.propId = p.id; tb.name = ObjectName(p.actor);
        tb.cls = ObjectName(reinterpret_cast<const UObject*>(ClassOf(p.actor)));
        for (const Prop::Body& b : p.bodies) tb.bodies.push_back(ObjectName(b.comp));
        rec::Append(batch_, rec::Chunk::PropTable, tb);
        p.tabled = true;
    };
    for (auto& pp : props_) {
        Prop& p = *pp;
        if (p.weapon) {
            bool held = false;
            for (const auto& f : fighters_) {
                held = held || f->weaponActor[0] == p.actor || f->weaponActor[1] == p.actor;
                for (UObject* a : f->slotActor) held = held || a == p.actor;
            }
            // In a hand its motion is the FighterFrame's weapon; in a sheath slot it goes with the body.
            if (held) { for (Prop::Body& b : p.bodies) b.moving = false; continue; }
        }
        for (size_t i = 0; i < p.bodies.size(); ++i) {
            Prop::Body& b = p.bodies[i];
            if (b.gc && !b.broken && nowMs >= b.nextBreakMs) {
                b.nextBreakMs = nowMs + kBreakCheckMs;
                if (adapter.StillValid(b.comp, b.index)) {
                    Call ib(b.comp, "IsRootBroken");
                    if (ib.invoke() && ib.get<bool>("ReturnValue")) {
                        b.broken = true;
                        table(p);
                        rec::Event e;
                        e.ts = nowMs; e.kind = rec::Ev::Break; e.propId = p.id; e.text = ObjectName(b.comp);
                        Note(e);
                    }
                }
            }
            if (!b.calls || !b.simulates || nowMs < b.nextReadMs) continue;
            if (!adapter.StillValid(b.comp, b.index)) { b.simulates = false; continue; }   // gone: the next scan drops it
            BodyCalls& c = *b.calls;
            c.linVel.put(0, FName{});
            const FVector v = c.linVel.invoke(b.comp) ? c.linVel.take<FVector>(1) : FVector{};
            c.angVel.put(0, FName{});
            const FVector w = c.angVel.invoke(b.comp) ? c.angVel.take<FVector>(1) : FVector{};
            BodyStateMsg s;
            s.vx = Finite(v.x); s.vy = Finite(v.y); s.vz = Finite(v.z);
            s.wx = Finite(w.x); s.wy = Finite(w.y); s.wz = Finite(w.z);
            const bool rest = PropAtRest(s);
            b.nextReadMs = rest ? nowMs + kPropRestCheckMs : 0;
            if (rest && !b.moving) continue;   // lying still: only its velocity was read
            if (!c.toWorld.invoke(b.comp)) continue;
            const FTransform t = c.toWorld.take<FTransform>(0);
            b.pos = t.translation; b.rot = t.rotation; b.known = true;
            // Some weapons carry a part (NODE_AddStaticMeshComponent-0) that simulates on its own at the world origin and
            // falls for ever; it is not the weapon.
            if (i && p.bodies[0].known && Dist(b.pos, p.bodies[0].pos) > kStrayCm) {
                b.simulates = false; b.stray = true;
                continue;
            }
            table(p);
            rec::PropFrame fr;
            fr.ts = nowMs; fr.propId = p.id; fr.body = uint8_t(i); fr.flags = rest ? rec::kPropAtRest : 0;
            fr.pos = F3Of(t.translation); fr.rot = QOf(t.rotation);
            fr.vel = {s.vx, s.vy, s.vz}; fr.angVelDeg = {s.wx, s.wy, s.wz};
            rec::Append(batch_, rec::Chunk::PropFrame, fr);
            ++propFrames_;
            b.moving = !rest;
        }
    }
}

void Recorder::HookInput(Adapter& adapter, UObject* localWillie) {
    if (inputHooked_ || !localWillie || !adapter.BindInputs(localWillie)) return;
    inputHooked_ = true;
    for (const auto& [fn, b] : adapter.Bindings()) {
        const InputBinding bind = b;
        HookFunction(fn, [this, bind](UObject* obj, UFunction* f, void* params) {
            if (!file_ || !obj || obj != inputWillie_) return false;
            if (bind.isAxis) {
                input_.OnAxis(Axis(bind.index), Adapter::ReadAxisParam(f, params));
                return false;
            }
            input_.OnAction(Action(bind.index), bind.pressed);
            rec::Event e;
            e.ts = SteadyNowMs(); e.kind = rec::Ev::Input; e.subject = FighterId(obj);
            e.text = bind.index < kActionCount ? kActionText[bind.index] : "?";
            e.x[0] = bind.pressed ? 1.f : 0.f; e.x[1] = float(bind.index);
            Note(e);
            return false;
        });
    }
    HSM_LOG("recorder: input hooks on %zu function(s)", adapter.Bindings().size());
}

// The local player's input since the last sample: axes (mouse summed), held buttons, the camera.
void Recorder::SampleInput(Adapter& adapter, UObject* localWillie, double nowMs) {
    inputWillie_ = localWillie;
    if (!localWillie) return;
    HookInput(adapter, localWillie);
    const uint16_t id = FighterId(localWillie);
    if (!inputHooked_ || !id) return;
    const InputFrame f = input_.Take(uint32_t(nowMs));
    rec::InputSample s;
    s.ts = nowMs; s.recId = id; s.axes.assign(f.axes.begin(), f.axes.end()); s.buttons = f.buttons;
    adapter.ControlRotation(0, s.controlYaw, s.controlPitch);
    rec::Append(batch_, rec::Chunk::Input, s);
    ++inputs_;
}

}  // namespace hsm::game

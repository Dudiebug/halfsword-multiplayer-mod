#include "gameplay/equip_sync.h"
#include "gameplay/standin_slots.h"
#include "util/hsm_names.h"
#include "log.h"
#include "sync/hsm_handmatch.h"
#include "sync/hsm_pose_codec.h"
#include <cstdio>

namespace hsm::game {

using namespace hsm::ue;
using combat::EquipStateMsg;
using combat::EquipStep;
using combat::kEquipSlots;

namespace {

constexpr double kPollMs = 100;      // the owner reads its fighters this often (a draw or sheathe takes far longer)
constexpr uint64_t kLogFirst = 10;   // the first states sent and steps run are logged in full (test evidence)
// A pose hand counts for a stale-weapon hint once it has shown the same class this long: a held weapon the pose misses
// for a few frames (kinematic, or swung past kHeldMaxUu) is not put away.
constexpr double kPoseSettleMs = 200;

constexpr const char* kFistsPrefix = "Weapon_Fists";   // an empty hand's pseudo weapon (standin_rig.cpp)

// Willie_BP's members by hand (0 right, 1 left) and by slot (SheathSlots_Enum order: R 1, R 2, Back, L 1, L 2), each
// name written out so tools/check_reflection.py sees it.
UObject** HandField(UObject* w, int hand) { return hand == 0 ? Field<UObject*>(w, "Weapon R") : Field<UObject*>(w, "Weapon L"); }
UObject** SlotField(UObject* w, int slot) {
    switch (slot) {
        case 0: return Field<UObject*>(w, "Weapon Slot R 1");
        case 1: return Field<UObject*>(w, "Weapon Slot R 2");
        case 2: return Field<UObject*>(w, "Weapon Slot Back");
        case 3: return Field<UObject*>(w, "Weapon Slot L 1");
        case 4: return Field<UObject*>(w, "Weapon Slot L 2");
        default: return nullptr;
    }
}
UObject** ConstraintField(UObject* w, int slot) {
    switch (slot) {
        case 0: return Field<UObject*>(w, "Slot R 1 Constraint");
        case 1: return Field<UObject*>(w, "Slot R 2 Constraint");
        case 2: return Field<UObject*>(w, "Slot Back Constraint");
        case 3: return Field<UObject*>(w, "Slot L 1 Constraint");
        case 4: return Field<UObject*>(w, "Slot L 2 Constraint");
        default: return nullptr;
    }
}

// "Set Up Right/Left Hand Weapon" with that hand's weapon (null: let go). No class, no blow, nothing destroyed on
// purpose; the passport is only read to spawn a class. Not under ScriptHookBypass: the claims hooks keep guarding
// anything it reaches (as prop_sync's drop).
bool SetUpHand(UObject* willie, int hand, UObject* weapon) {
    if (hand == 0) {
        Call c(willie, "Set Up Right Hand Weapon");
        c.set<UObject*>("Weapon R", weapon);
        return c.valid() && c.invoke();
    }
    Call c(willie, "Set Up Left Hand Weapon");
    c.set<UObject*>("Weapon L", weapon);
    return c.valid() && c.invoke();
}

bool CreateSheathConstraint(UObject* willie, int hand, uint8_t slotEnum) {
    if (hand == 0) {
        Call c(willie, "Create Sheath Constraint R");
        c.set<uint8_t>("Slot", slotEnum);
        return c.valid() && c.invoke();
    }
    Call c(willie, "Create Sheath Constraint L");
    c.set<uint8_t>("Slot", slotEnum);
    return c.valid() && c.invoke();
}

// Weapon_Slots_Enum of a slot for one hand's Create Sheath Constraint (R: R 1, R 2, Back; L: L 1, L 2, Back), -1 when
// that hand cannot sheathe there.
int HandSlotEnum(int hand, int slot) {
    if (slot == 2) return 2;
    if (hand == 0) return slot <= 1 ? slot : -1;
    return slot >= 3 ? slot - 3 : -1;
}

// The socket a weapon sheathes at in a slot, by its SheathSlot (Sheathe on Spawn, Willie_BP.cpp:6994); null for none.
const char* SlotSocket(uint8_t sheathSlot, int slot) {
    static constexpr const char* kSmall[kEquipSlots] = {"Sheath_Slot_R_1", "Sheath_Slot_R_2", "Sheath_Slot_Back", "Sheath_Slot_L_1",
                                                        "Sheath_Slot_L_2"};
    static constexpr const char* kLarge[kEquipSlots] = {"Sheath_Slot_R_3", "Sheath_Slot_R_4", "Sheath_Slot_Back", "Sheath_Slot_L_3",
                                                        "Sheath_Slot_L_4"};
    switch (sheathSlot) {
        case 0: return kSmall[slot];
        case 1: return kLarge[slot];
        case 2: return "Sheath_Slot_Shoulder_L";
        case 3: return "Sheath_Slot_Back";
        default: return nullptr;
    }
}

// A component that is there and not being destroyed (a drawn weapon's constraint is destroyed, not cleared).
bool Live(UObject* component) {
    if (!component) return false;
    Call c(component, "IsBeingDestroyed");
    return c.invoke() && !c.get<bool>("ReturnValue");
}

bool Fists(UObject* actor) { return actor && StartsWithNameAscii(ObjectName(actor), kFistsPrefix); }

// As the pose carries it (standin_rig.cpp): the FNV-1a hash of the weapon's class name.
uint32_t ClassHash(UObject* actor) { return sync::WeaponClassHash(ObjectName(reinterpret_cast<UObject*>(ClassOf(actor))).c_str()); }

const char* Side(int hand) { return hand == 0 ? "R" : "L"; }

}  // namespace

void EquipSync::Start(Adapter& adapter, StandIns& standIns, Env env) {
    Stop();
    adapter_ = &adapter;
    standIns_ = &standIns;
    env_ = std::move(env);
    active_ = true;
    HSM_LOG("equip: on: weapons drawn and sheathed by fighters simulated here are shown on every stand-in of them");
}

void EquipSync::Stop() {
    if (active_) HSM_LOG("equip: off (%s)", HealthLine().c_str());
    *this = EquipSync{};
}

void EquipSync::Forget(uint8_t entity) {
    own_.erase(entity);
    standIn_.erase(entity);
    owner_.erase(entity);
    planPose_.erase(entity);
    sender_.Forget(entity);
}

// The hands, the slot variables and whether each slot's sheath constraint is live. False when a field is unreadable.
bool EquipSync::ReadFighter(UObject* willie, Read& out) const {
    if (!willie || Adapter::BeingDestroyed(willie)) return false;
    for (int h = 0; h < 2; ++h) {
        UObject** p = HandField(willie, h);
        if (!p) return false;
        out.hands[h] = *p;
        out.snap.hands[h] = *p;
    }
    for (int k = 0; k < kEquipSlots; ++k) {
        UObject** slot = SlotField(willie, k);
        UObject** constraint = ConstraintField(willie, k);
        if (!slot || !constraint) return false;
        out.slots[k] = *slot;
        out.snap.slots[k] = *slot;
        out.snap.constrained[k] = *slot && Live(*constraint);
    }
    return true;
}

// ---------------------------------------------------------------- the owner's side

void EquipSync::Tick(double nowMs, const std::vector<std::pair<uint8_t, UObject*>>& owned) {
    if (!active_ || nowMs < nextPollMs_) return;
    nextPollMs_ = nowMs + kPollMs;
    for (const auto& [entity, willie] : owned) {
        Read r;
        if (!ReadFighter(willie, r)) { ++n_.readFailed; continue; }
        EquipStateMsg m = own_[entity].Read(r.snap);
        m.entity = entity;
        if (!sender_.Due(m, nowMs)) continue;
        m.seq = ++seq_;
        if (env_.send) env_.send(m);
        if (++n_.sent <= kLogFirst)
            HSM_LOG("equip: sent #%u e%u hands R %u L %u sheathed 0x%02x", m.seq, m.entity, m.hand[0], m.hand[1], m.sheathed);
    }
    // Stand-ins: a pose that settled on something else since the last plan may have left a held weapon stale.
    for (const auto& kv : owner_) {
        const uint8_t entity = kv.first;
        if (auto seen = planPose_.find(entity); seen != planPose_.end() && seen->second == PoseClasses(entity, nowMs)) continue;
        if (Plan(entity, nowMs)) ++n_.replanned;
    }
}

// ---------------------------------------------------------------- the stand-in's side

void EquipSync::Apply(const EquipStateMsg& m) {
    if (!active_) return;
    owner_[m.entity] = m;
    if (Plan(m.entity, SteadyNowMs())) ++n_.applied;
}

// The owner's pose per hand as far as a stale hint goes: the class it shows (0 empty), or 1 (unknown) before any pose,
// for a weapon of unknown class, and until that class has shown for kPoseSettleMs.
std::array<uint32_t, 2> EquipSync::PoseClasses(uint8_t entity, double nowMs) const {
    std::array<uint32_t, 2> out{sync::kHandUnknown, sync::kHandUnknown};
    for (int h = 0; h < 2; ++h) {
        StandIns::PoseHandView v;
        if (standIns_->PoseHandInfo(entity, h, v) && nowMs - v.sinceMs >= kPoseSettleMs) out[size_t(h)] = v.classHash;
    }
    return out;
}

// The stand-in's hand holds a real weapon (for the left, not the right's again) of a class the owner's settled pose does
// not show there (combat::EquipHints).
bool EquipSync::Stale(const Read& r, int hand, uint32_t poseClass) const {
    UObject* held = r.hands[size_t(hand)];
    if (!held || (hand == 1 && held == r.hands[0]) || Fists(held) || adapter_->IsBodyWeapon(held) || Adapter::BeingDestroyed(held))
        return false;
    return poseClass != sync::kHandUnknown && ClassHash(held) != poseClass;
}

bool EquipSync::Plan(uint8_t entity, double nowMs) {
    auto it = owner_.find(entity);
    if (it == owner_.end()) return false;
    const EquipStateMsg& m = it->second;
    const std::array<uint32_t, 2> pose = PoseClasses(entity, nowMs);
    planPose_[entity] = pose;
    UObject* standIn = standIns_->ActorOf(entity);
    if (!standIn) { ++n_.noStandIn; return false; }
    Read r;
    if (!ReadFighter(standIn, r)) { ++n_.readFailed; return false; }
    const EquipStateMsg mine = standIn_[entity].Read(r.snap);
    std::array<bool, kEquipSlots> hasSlot{};
    for (int k = 0; k < kEquipSlots; ++k) hasSlot[k] = r.slots[k] && !Adapter::BeingDestroyed(r.slots[k]);
    combat::EquipHints hints;
    for (int h = 0; h < 2; ++h) hints.staleHeld[size_t(h)] = Stale(r, h, pose[size_t(h)]);
    for (const EquipStep& s : combat::PlanEquip(m, mine, hasSlot, hints)) {
        Read now;   // each step changes the stand-in: read it again
        if (!ReadFighter(standIn, now)) { ++n_.readFailed; return true; }
        const bool draw = s.kind == EquipStep::Kind::Draw;
        const bool ok = draw ? Draw(standIn, now, s.hand, s.slot) : Sheathe(standIn, now, s.hand, s.slot);
        if (!ok) continue;
        const uint64_t done = draw ? ++n_.draws : ++n_.sheathes;
        if (done <= kLogFirst)
            HSM_LOG("equip: #%u stand-in of e%u %s hand %s slot %d", m.seq, m.entity, draw ? "drew into" : "sheathed from", Side(s.hand),
                    s.slot);
    }
    if (Read after; ReadFighter(standIn, after)) standIn_[entity].Read(after.snap);   // where the steps put things
    return true;
}

bool EquipSync::OwnerToken(uint8_t entity, int hand, uint8_t& token) const {
    auto it = owner_.find(entity);
    if (!active_ || it == owner_.end() || hand < 0 || hand > 1) return false;
    token = it->second.hand[size_t(hand)];
    return true;
}

bool EquipSync::MayBeDraw(uint8_t entity, uint32_t classHash) const {
    auto it = owner_.find(entity);
    UObject* standIn = active_ && it != owner_.end() ? standIns_->ActorOf(entity) : nullptr;
    if (!standIn) return false;
    std::array<uint32_t, kEquipSlots> slotClass{};
    for (int k = 0; k < kEquipSlots; ++k) {
        UObject** slot = SlotField(standIn, k);
        if (slot && *slot && !Adapter::BeingDestroyed(*slot)) slotClass[size_t(k)] = ClassHash(*slot);
    }
    return combat::MayBeDrawFrom(it->second.sheathed, slotClass, classHash);
}

bool EquipSync::MayBeSheathe(uint8_t entity, int hand) const {
    auto it = owner_.find(entity);
    UObject* standIn = active_ && it != owner_.end() ? standIns_->ActorOf(entity) : nullptr;
    if (!standIn) return false;
    for (int k = 0; k < kEquipSlots; ++k) {
        if (!combat::HandReachesSlot(hand, k) || !(it->second.sheathed & (1u << k))) continue;
        UObject** slot = SlotField(standIn, k);
        if (slot && (!*slot || Adapter::BeingDestroyed(*slot))) return true;
    }
    return false;
}

// The stand-in's hand grabs the slot's weapon, then the weapon leaves the slot as Check Scabbard Unsheath State lets it
// go. A hand holding a real weapon is left alone (prop_sync lets go of it first and asks for a re-plan, which draws).
bool EquipSync::Draw(UObject* standIn, const Read& r, int hand, int slot) {
    UObject* weapon = r.slots[slot];
    UObject* held = r.hands[hand];
    if (!weapon || Adapter::BeingDestroyed(weapon) || (held && !Fists(held))) { ++n_.skipped; return false; }
    const bool grabbed = SetUpHand(standIn, hand, weapon);
    UObject** handField = HandField(standIn, hand);
    if (!grabbed || !handField || *handField != weapon) {
        ++n_.failed;
        HSM_LOG("equip: stand-in %s did not take %s into hand %s", ObjectName(standIn).c_str(), ObjectName(weapon).c_str(), Side(hand));
        return false;
    }
    UObject** constraint = ConstraintField(standIn, slot);
    if (constraint && Live(*constraint)) {
        Call(*constraint, "BreakConstraint").invoke();
        Call(*constraint, "K2_DestroyComponent").set<UObject*>("Object", standIn).invoke();
    }
    Call(weapon, "Unsheathed Mode").invoke();
    if (UObject** p = SlotField(standIn, slot); p && *p == weapon) *p = nullptr;
    return true;
}

// The hand's weapon goes back to the slot it came from: the slot's socket and scabbard as Assign R/L Weapon Slot sets
// them, the game's own sheath constraint, then the hand lets go. Skipped when that hand cannot reach the slot, the weapon
// has no sheath socket there, or the hand's grip type is 0 (the release would destroy the weapon).
bool EquipSync::Sheathe(UObject* standIn, const Read& r, int hand, int slot) {
    UObject* weapon = r.hands[hand];
    const int slotEnum = HandSlotEnum(hand, slot);
    const uint8_t* sheathSlot = weapon ? Field<uint8_t>(weapon, "SheathSlot") : nullptr;
    const char* socket = sheathSlot ? SlotSocket(*sheathSlot, slot) : nullptr;
    UObject** scabbardField = weapon ? Field<UObject*>(weapon, "Scabbard") : nullptr;
    UObject* scabbard = scabbardField && sheathSlot && *sheathSlot == 0 ? *scabbardField : nullptr;
    if (scabbard && Adapter::BeingDestroyed(scabbard)) scabbard = nullptr;
    const uint8_t* grip = hand == 0 ? Field<uint8_t>(standIn, "R_GripType_Current") : Field<uint8_t>(standIn, "L_GripType_Current");
    UObject** current = hand == 0 ? Field<UObject*>(standIn, "Current Scabbard R") : Field<UObject*>(standIn, "Current Scabbard L");
    FName* socketField = hand == 0 ? Field<FName>(standIn, "R Sheath Slot Name") : Field<FName>(standIn, "L Sheath Slot Name");
    if (!weapon || Adapter::BeingDestroyed(weapon) || slotEnum < 0 || !socket || (scabbard && slot == 2) || !grip || *grip == 0 ||
        !current || !socketField || r.slots[slot]) {
        ++n_.skipped;
        return false;
    }
    *current = scabbard;
    *socketField = MakeName(socket);
    const bool made = CreateSheathConstraint(standIn, hand, uint8_t(slotEnum));
    UObject** slotField = SlotField(standIn, slot);
    if (!made || !slotField || *slotField != weapon) {
        ++n_.failed;
        HSM_LOG("equip: stand-in %s did not sheathe %s in slot %d", ObjectName(standIn).c_str(), ObjectName(weapon).c_str(), slot);
        return false;
    }
    const bool letGo = LetGo(standIn, hand);
    if (letGo) StandInSlots::Anchor(standIn, true);   // the sheathed weapon goes with the body, not BoneCore (standin_slots.h)
    return letGo;
}

// "Set Up ... Hand Weapon" with no weapon: the hand lets go of the (now sheathed) weapon, which keeps its slot.
bool EquipSync::LetGo(UObject* standIn, int hand) {
    const bool released = SetUpHand(standIn, hand, nullptr);
    UObject** handField = HandField(standIn, hand);
    if (!released || !handField || *handField) {
        ++n_.failed;
        HSM_LOG("equip: stand-in %s hand %s did not let go after sheathing", ObjectName(standIn).c_str(), Side(hand));
        return false;
    }
    return true;
}

std::string EquipSync::HealthLine() {
    const Counters& a = n_;
    const Counters& b = reported_;
    auto d = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
    char line[320];
    std::snprintf(line, sizeof line,
                  "equip: sent %llu (unreadable %llu) | applied %llu replanned %llu (no stand-in %llu): drew %llu sheathed %llu "
                  "skipped %llu failed %llu",
                  d(a.sent, b.sent), d(a.readFailed, b.readFailed), d(a.applied, b.applied), d(a.replanned, b.replanned),
                  d(a.noStandIn, b.noStandIn), d(a.draws, b.draws), d(a.sheathes, b.sheathes), d(a.skipped, b.skipped),
                  d(a.failed, b.failed));
    reported_ = n_;
    return line;
}

}  // namespace hsm::game

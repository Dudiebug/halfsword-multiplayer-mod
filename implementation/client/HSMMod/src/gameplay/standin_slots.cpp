#include "gameplay/standin_slots.h"
#include "combat/hsm_equip.h"
#include "log.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace hsm::game {
using namespace hsm::ue;
using combat::kEquipSlots;

namespace {

constexpr double kLogAtMs[3] = {0.0, 1000.0, 3000.0};   // after the first pass: at bind, then ~1 s and ~3 s later
constexpr const char* kSlotLabel[kEquipSlots] = {"R1", "R2", "Back", "L1", "L2"};
constexpr int kBackSlot = 2;                             // its constraint already holds the Mesh
constexpr double kLostAfterMs = 1000.0;                  // a slot weapon's constraint is made a moment after it is spawned
constexpr uint8_t kWorldStatic = 0, kWorldDynamic = 1;   // ECollisionChannel (standin_rig.cpp)
constexpr uint8_t kIgnore = 0;                           // ECollisionResponse::ECR_Ignore
constexpr size_t kMemory = 64;                           // constraints / weapons remembered
constexpr double kSnapBackCm = 15.0;                     // a waist weapon farther than this from its socket is put back first
constexpr uint8_t kWorldSpace = 0;                       // ERelativeTransformSpace::RTS_World

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

UObject* ObjField(UObject* o, const char* name) { UObject** p = o ? Field<UObject*>(o, name) : nullptr; return p ? *p : nullptr; }

// Willie_BP's slot variables and sheath constraints by slot (SheathSlots_Enum order: R 1, R 2, Back, L 1, L 2), each name
// written out so tools/check_reflection.py sees it (as equip_sync.cpp).
UObject* SlotActor(UObject* w, int slot) {
    switch (slot) {
        case 0: return ObjField(w, "Weapon Slot R 1");
        case 1: return ObjField(w, "Weapon Slot R 2");
        case 2: return ObjField(w, "Weapon Slot Back");
        case 3: return ObjField(w, "Weapon Slot L 1");
        case 4: return ObjField(w, "Weapon Slot L 2");
        default: return nullptr;
    }
}
UObject* SlotConstraint(UObject* w, int slot) {
    switch (slot) {
        case 0: return ObjField(w, "Slot R 1 Constraint");
        case 1: return ObjField(w, "Slot R 2 Constraint");
        case 2: return ObjField(w, "Slot Back Constraint");
        case 3: return ObjField(w, "Slot L 1 Constraint");
        case 4: return ObjField(w, "Slot L 2 Constraint");
        default: return nullptr;
    }
}

// The socket of the Mesh a weapon sheathes at in a slot, by its SheathSlot (a copy of equip_sync.cpp SlotSocket); null for
// none.
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

// A component that is there and not being destroyed (equip_sync.cpp Live).
bool ComponentLive(UObject* component) {
    if (!component) return false;
    Call c(component, "IsBeingDestroyed");
    return c.invoke() && !c.get<bool>("ReturnValue");
}

bool Simulating(UObject* body) {
    if (!body) return false;
    Call c(body, "IsSimulatingPhysics");
    c.setName("BoneName", FName{});
    return c.invoke() && c.get<bool>("ReturnValue");
}

// The socket of the Mesh a slot's weapon sheathes at (null when it has none).
const char* SocketOf(UObject* weapon, int slot) {
    const uint8_t* sheathSlot = weapon ? Field<uint8_t>(weapon, "SheathSlot") : nullptr;
    return sheathSlot ? SlotSocket(*sheathSlot, slot) : nullptr;
}

// Distance in cm from the weapon body to that socket of the Mesh; -1 when either cannot be read.
double SocketDistance(UObject* mesh, UObject* body, const char* socket) {
    if (!mesh || !body || !socket) return -1.0;
    Call at(body, "K2_GetComponentLocation");
    Call sock(mesh, "GetSocketLocation");
    sock.setName("InSocketName", MakeName(socket));
    if (!at.invoke() || !sock.invoke()) return -1.0;
    const FVector a = at.get<FVector>("ReturnValue"), b = sock.get<FVector>("ReturnValue");
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// One slot as the anchor sees it.
struct Slot {
    UObject* weapon = nullptr;
    UObject* constraint = nullptr;
    UObject* body = nullptr;   // the weapon's BaseMesh
    bool live = false;         // the constraint is there and not being destroyed
    bool own = false;          // the weapon's "Parent Actor" is this stand-in
    bool held = false;         // in a hand (a draw grabs it while the slot variable still holds it)
    bool anchor = false;       // combat::SlotAnchorPlan, an own weapon with a BaseMesh
};

std::array<Slot, kEquipSlots> ReadSlots(UObject* willie) {
    std::array<Slot, kEquipSlots> out{};
    std::array<const void*, kEquipSlots> actors{};
    std::array<bool, kEquipSlots> live{};
    for (int k = 0; k < kEquipSlots; ++k) {
        Slot& s = out[size_t(k)];
        s.weapon = SlotActor(willie, k);
        s.constraint = SlotConstraint(willie, k);
        s.live = ComponentLive(s.constraint);
        actors[size_t(k)] = s.weapon;
        live[size_t(k)] = s.live;
    }
    const std::array<const void*, 2> hands = {ObjField(willie, "Weapon R"), ObjField(willie, "Weapon L")};
    const std::array<bool, kEquipSlots> plan = combat::SlotAnchorPlan(actors, live, hands);
    for (int k = 0; k < kEquipSlots; ++k) {
        Slot& s = out[size_t(k)];
        if (!s.weapon) continue;
        s.body = ObjField(s.weapon, "BaseMesh");
        s.own = ObjField(s.weapon, "Parent Actor") == willie;
        s.held = s.weapon == hands[0] || s.weapon == hands[1];
        s.anchor = plan[size_t(k)] && s.own && s.body;
    }
    return out;
}

// What was already done, by object (an entry only counts while its index still holds the same object).
using Seen = std::vector<std::pair<UObject*, int32_t>>;
bool Remembered(Seen& seen, UObject* o) {
    seen.erase(std::remove_if(seen.begin(), seen.end(), [](const auto& e) { return ObjectAt(e.second) != e.first; }), seen.end());
    return std::any_of(seen.begin(), seen.end(), [&](const auto& e) { return e.first == o; });
}
void Remember(Seen& seen, UObject* o) {
    if (seen.size() >= kMemory) seen.erase(seen.begin());
    seen.emplace_back(o, ObjectIndex(o));
}

// The held weapons' body policy (standin_rig.cpp SetBodyPolicy): gravity off and world geometry ignored, pawn / body /
// weapon contacts stay.
void SetBodyPolicy(UObject* body) {
    Call(body, "SetEnableGravity").set<bool>("bGravityEnabled", false).invoke();
    Call(body, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldStatic).set<uint8_t>("NewResponse", kIgnore).invoke();
    Call(body, "SetCollisionResponseToChannel").set<uint8_t>("Channel", kWorldDynamic).set<uint8_t>("NewResponse", kIgnore).invoke();
}

// A teleporting move of a component by d, then at rest (standin_rig.cpp MoveBy / HoldStill).
void MoveBy(UObject* body, const sync::V3& d) {
    Call at(body, "K2_GetComponentLocation");
    if (!at.invoke()) return;
    const FVector p = at.get<FVector>("ReturnValue");
    Call(body, "K2_SetWorldLocation").set("NewLocation", FVector{p.x + d.x, p.y + d.y, p.z + d.z}).set<bool>("bSweep", false)
        .set<bool>("bTeleport", true).invoke();
}
void HoldStill(UObject* body) {
    Call(body, "SetPhysicsLinearVelocity").set("NewVel", FVector{}).set<bool>("bAddToCurrent", false).setName("BoneName", FName{}).invoke();
    Call(body, "SetPhysicsAngularVelocityInDegrees").set("NewAngVel", FVector{}).set<bool>("bAddToCurrent", false)
        .setName("BoneName", FName{}).invoke();
}

// Teleports the weapon body to the socket's world transform (its own scale kept), then at rest: a waist weapon that fell
// while BoneCore had no collision, before the constraint is made again (it would keep the offset).
bool SnapToSocket(UObject* mesh, UObject* body, const char* socket) {
    Call sock(mesh, "GetSocketTransform");
    sock.setName("InSocketName", MakeName(socket)).set<uint8_t>("TransformSpace", kWorldSpace);
    Call scale(body, "K2_GetComponentScale");
    if (!sock.invoke() || !scale.invoke()) return false;
    FTransform t = sock.get<FTransform>("ReturnValue");
    t.scale = scale.get<FVector>("ReturnValue");
    Call set(body, "K2_SetWorldTransform");
    set.set("NewTransform", t).set<bool>("bSweep", false).set<bool>("bTeleport", true);
    if (!set.invoke()) return false;
    HoldStill(body);
    return true;
}

// rig: <willie> slots: R1 <weapon or -> c=<constraint live> sim=<BaseMesh simulating> <distance to its socket>cm | R2 ...
void LogSlots(UObject* willie) {
    UObject* mesh = ObjField(willie, "Mesh");
    char line[1024];
    size_t n = size_t(std::snprintf(line, sizeof line, "rig: %s slots:", ObjectName(willie).c_str()));
    for (int k = 0; k < kEquipSlots && n < sizeof line; ++k) {
        UObject* weapon = SlotActor(willie, k);
        UObject* body = ObjField(weapon, "BaseMesh");
        const std::string name = weapon ? ObjectName(weapon) : "-";
        const int w = std::snprintf(line + n, sizeof line - n, "%s %s %s c=%d sim=%d %.0fcm", k ? " |" : "", kSlotLabel[k], name.c_str(),
                                    ComponentLive(SlotConstraint(willie, k)) ? 1 : 0, Simulating(body) ? 1 : 0,
                                    SocketDistance(mesh, body, SocketOf(weapon, k)));
        n = std::min(n + size_t(std::max(w, 0)), sizeof line - 1);
    }
    HSM_LOG("%s", line);
}

}  // namespace

void StandInSlots::Maintain(UObject* willie) {
    if (!willie) return;
    const double now = NowMs();
    if (willie != willie_ || ObjectIndex(willie) != index_) {
        willie_ = willie;
        index_ = ObjectIndex(willie);
        firstMs_ = now;
        logged_ = 0;
    }
    if (logged_ < 3 && now - firstMs_ >= kLogAtMs[logged_]) {
        LogSlots(willie);
        ++logged_;
    }
    Anchor(willie, now - firstMs_ >= kLostAfterMs);
}

void StandInSlots::Shift(UObject* willie, const sync::V3& d) {
    if (!willie) return;
    for (const Slot& s : ReadSlots(willie))
        if (s.anchor) {
            MoveBy(s.body, d);
            HoldStill(s.body);
        }
}

void StandInSlots::Anchor(UObject* willie, bool logLost) {
    static Seen retargeted, lost, snapped;   // game thread only
    UObject* mesh = willie ? ObjField(willie, "Mesh") : nullptr;
    if (!mesh) return;
    const std::array<Slot, kEquipSlots> slots = ReadSlots(willie);
    for (int k = 0; k < kEquipSlots; ++k) {
        const Slot& s = slots[size_t(k)];
        if (s.anchor) {
            SetBodyPolicy(s.body);
            // The waist slots hold BoneCore, whose collision the rig switches off; the pelvis body is the one it drives.
            if (k != kBackSlot && !Remembered(retargeted, s.constraint)) {
                // The new constraint keeps the weapon where it is: one that fell while BoneCore had no collision (the game
                // makes the constraint a moment after spawning the weapon) goes back to its socket first.
                const char* socket = SocketOf(s.weapon, k);
                const double away = SocketDistance(mesh, s.body, socket);
                if (away > kSnapBackCm && SnapToSocket(mesh, s.body, socket) && !Remembered(snapped, s.weapon)) {
                    Remember(snapped, s.weapon);
                    HSM_LOG("rig: %s slot %s snapped back from %.0fcm", ObjectName(willie).c_str(), kSlotLabel[k], away);
                }
                Call c(s.constraint, "SetConstrainedComponents");
                c.set<UObject*>("Component1", s.body).setName("BoneName1", FName{}).set<UObject*>("Component2", mesh)
                    .setName("BoneName2", MakeName("pelvis"));
                if (c.invoke()) Remember(retargeted, s.constraint);
            }
        } else if (logLost && s.weapon && s.own && !s.held && !s.live && !Remembered(lost, s.weapon)) {
            Remember(lost, s.weapon);
            HSM_LOG("rig: %s slot %s lost its sheath constraint", ObjectName(willie).c_str(), kSlotLabel[k]);
        }
    }
}

}  // namespace hsm::game

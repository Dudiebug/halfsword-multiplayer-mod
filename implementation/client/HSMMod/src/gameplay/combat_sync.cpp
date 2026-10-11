#include "gameplay/combat_sync.h"
#include <windows.h>
#include "combat/hsm_ue_map.h"
#include "log.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace hsm::game {
using namespace hsm::ue;
namespace {

// Enum_DismembermentPart value -> bone, from the "Spawn Bone" switch in Dismember_Function_Delayed
// (re/out/decomp/Character/Blueprints/Willie_BP.cpp:3269). Also the bone table CombatEvent::bone indexes.
constexpr const char* kPartBones[kCombatPartBits] = {
    "neck_01", "neck_01", "pelvis", "upperarm_r", "lowerarm_r", "hand_r", "upperarm_l", "lowerarm_l", "hand_l",
    "thigh_r", "calf_r", "foot_r", "thigh_l", "calf_l", "foot_l",
};

// Blueprint names carry spaces (call sites in the decomp show them, e.g. "Paint Blood Splash(" in Willie_BP.cpp:8038).
constexpr const char* kPaintBlood = "Paint Blood Splash";        // Willie_BP.cpp:4886 Paint_Blood_Splash
constexpr const char* kDismemberInitiate = "Dismember Function Initiate";   // Willie_BP.cpp:3648
constexpr const char* kSeveredMap = "Dismembered Parts Map";     // Willie_BP.cpp:1016, Map<Byte<Enum_DismembermentPart>,Bool>

// Blood tuning: CombatEvent::size 0..255 -> Size 0.25..4.25 (guess, tune in game); sharp hits paint harder.
constexpr double kBloodSizeBase = 0.25, kBloodSizePerUnit = 1.0 / 64.0;
constexpr double kBloodRateSharp = 1.0, kBloodRateBlunt = 0.5;

bool Readable(const void* p, size_t n) { return p && n && !IsBadReadPtr(p, n); }

bool HasParams(const UFunction* fn, std::initializer_list<const char*> names) {
    std::vector<FProperty*> params = Params(fn);
    for (const char* want : names) {
        bool found = std::any_of(params.begin(), params.end(), [want](FProperty* p) { return PropertyName(p) == want; });
        if (!found) return false;
    }
    return true;
}

}  // namespace

// The map's layout lives in shared/combat/hsm_ue_map.h (unit-tested); every read is pointer-checked and any surprise
// makes this fail closed. The reflected property size must match the layout the reader assumes.
uint32_t ReadSeveredMask(UObject* willie) {
    auto fail = [](const char* why) -> uint32_t {
        static bool logged = false;
        if (!logged) { HSM_LOG("combat: cannot read %s (%s)", kSeveredMap, why); logged = true; }
        return 0;
    };
    if (!willie) return 0;
    static int32_t propSize = -1;
    if (propSize < 0) {
        const FProperty* p = FindProperty(ClassOf(willie), kSeveredMap);
        propSize = p ? PropertySize(p) : 0;
    }
    if (propSize != int32_t(combat::kUeMapSize)) return fail("property missing or not a UE 5.4 TMap");
    const uint8_t* map = static_cast<const uint8_t*>(FieldPtr(willie, kSeveredMap));
    uint32_t mask = 0;
    switch (combat::ReadByteBoolMap(map, Readable, kCombatPartBits, mask)) {
        case combat::MapRead::Ok: return mask & kCombatPartMask;
        case combat::MapRead::Unreadable: return fail("TMap storage unreadable");
        case combat::MapRead::BadLayout: return fail("unexpected TMap layout");
        case combat::MapRead::BadEntry: return fail("unexpected key or value");
    }
    return fail("unknown result");
}

bool ApplyDismember(UObject* willie, uint32_t parts) {
    if (!willie || parts == 0 || (parts & ~kCombatPartMask) != 0) return false;
    UFunction* fn = FindFunction(ClassOf(willie), kDismemberInitiate);
    if (!fn) { HSM_LOG("combat: %s not found on %s", kDismemberInitiate, ObjectName(willie).c_str()); return false; }
    if (!HasParams(fn, {"Master Mesh", "Dismembered Part", "Attach Marker", "Overlapped Markers",
                        "VP Box Collision 1", "VP Box Collision 2", "Dismember Weapon"})) {
        HSM_LOG("combat: %s parameter list differs from the decomp; dismember skipped", kDismemberInitiate);
        return false;
    }
    // Not invoked: Initiate reads Attach Marker and the VP boxes, which only exist during Constraint_Weapon_Stuck_BP's
    // cut (Blueprints/Utility/Constraint_Weapon_Stuck_BP.cpp:274). Passing nulls would leave Dismemberment In Process
    // set (Willie_BP.cpp:3652), blocking later cuts on this Willie.
    HSM_LOG("combat: dismember mask 0x%x on %s not replayed: needs the live cut markers", parts, ObjectName(willie).c_str());
    return false;
}

bool ApplyBlood(UObject* willie, const CombatEvent& e) {
    if (!willie || e.kind != kCombatBlood || e.entity == 0 || e.bone >= kCombatPartBits) return false;
    if (!std::isfinite(e.x) || !std::isfinite(e.y) || !std::isfinite(e.z)) return false;
    UFunction* fn = FindFunction(ClassOf(willie), kPaintBlood);
    if (!fn) { HSM_LOG("combat: %s not found on %s", kPaintBlood, ObjectName(willie).c_str()); return false; }
    // Decomp: Paint_Blood_Splash(out Location, BoneName, Size, Rate) (Willie_BP.cpp:4886).
    if (!HasParams(fn, {"Location", "BoneName", "Size", "Rate"})) {
        HSM_LOG("combat: %s parameter list differs from the decomp; blood skipped", kPaintBlood);
        return false;
    }
    const double size = kBloodSizeBase + kBloodSizePerUnit * e.size;
    const double rate = e.sharp ? kBloodRateSharp : kBloodRateBlunt;
    Call c(willie, fn);
    c.set<FVector>("Location", FVector{e.x, e.y, e.z})
        .setName("BoneName", MakeName(kPartBones[e.bone]))
        .set<double>("Size", size)
        .set<double>("Rate", rate);
    return c.invoke();
}

uint8_t NearestPartBone(UObject* willie, const FVector& at) {
    UObject** mesh = Field<UObject*>(willie, "Mesh");
    if (!mesh || !*mesh) return 0;
    uint8_t best = 0; double bestD = 1e30;
    for (uint8_t i = 0; i < kCombatPartBits; ++i) {
        Call c(*mesh, "GetSocketLocation");
        c.setName("InSocketName", MakeName(kPartBones[i]));
        if (!c.invoke()) continue;
        FVector p = c.get<FVector>("ReturnValue");
        double d = (p.x - at.x) * (p.x - at.x) + (p.y - at.y) * (p.y - at.y) + (p.z - at.z) * (p.z - at.z);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

}  // namespace hsm::game

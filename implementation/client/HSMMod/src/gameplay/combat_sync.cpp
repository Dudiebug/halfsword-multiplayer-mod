#include "gameplay/combat_sync.h"
#include <windows.h>
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

// TMap<uint8, bool> in UE 5.4 is a TSet over a TSparseArray: { TArray<Element> Data (ptr, num, max),
// TBitArray AllocationFlags (inline storage at +16, NumBits at +32, MaxBits at +36), FirstFreeIndex, NumFreeIndices }.
// Element = { uint8 key, bool value, int32 hashNext, int32 hashIndex }, stride 12. Assumed layout, not yet verified
// in game; every read is bounds- and pointer-checked and any surprise makes the function fail closed.
constexpr size_t kSparseSize = 48, kFlagsOffset = 16, kNumBitsOffset = 32, kMaxBitsOffset = 36;
constexpr size_t kElementStride = 12;
constexpr int32_t kMaxSparseSlots = 256;
constexpr uint32_t kInlineBitWords = 4;

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

uint32_t ReadSeveredMask(UObject* willie) {
    auto fail = [](const char* why) -> uint32_t {
        static bool logged = false;
        if (!logged) { HSM_LOG("combat: cannot read %s (%s)", kSeveredMap, why); logged = true; }
        return 0;
    };
    if (!willie) return 0;
    const uint8_t* map = static_cast<const uint8_t*>(FieldPtr(willie, kSeveredMap));
    if (!Readable(map, kSparseSize)) return fail("property missing");

    const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(map);
    const int32_t num = *reinterpret_cast<const int32_t*>(map + 8);
    const int32_t numBits = *reinterpret_cast<const int32_t*>(map + kNumBitsOffset);
    const int32_t maxBits = *reinterpret_cast<const int32_t*>(map + kMaxBitsOffset);
    if (num < 0 || num > kMaxSparseSlots || numBits != num || maxBits < numBits) return fail("unexpected TMap layout");
    if (num == 0) return 0;

    // Bit storage: inline words while they fit, otherwise a heap pointer stored in the same slot.
    const uint32_t* words = nullptr;
    if (maxBits <= int32_t(kInlineBitWords * 32)) words = reinterpret_cast<const uint32_t*>(map + kFlagsOffset);
    else words = *reinterpret_cast<const uint32_t* const*>(map + kFlagsOffset);
    const size_t wordCount = (size_t(num) + 31) / 32;
    if (!Readable(words, wordCount * 4) || !Readable(data, size_t(num) * kElementStride)) return fail("TMap storage unreadable");

    uint32_t mask = 0;
    for (int32_t i = 0; i < num; ++i) {
        if (((words[size_t(i) >> 5] >> (size_t(i) & 31u)) & 1u) == 0) continue;   // free slot
        const uint8_t key = data[size_t(i) * kElementStride];
        const uint8_t value = data[size_t(i) * kElementStride + 1];
        if (key >= kCombatPartBits || value > 1) return fail("unexpected key or value");
        if (value) mask |= 1u << key;
    }
    return mask & kCombatPartMask;
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

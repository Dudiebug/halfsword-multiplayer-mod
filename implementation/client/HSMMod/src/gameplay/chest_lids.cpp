#include "gameplay/chest_lids.h"
#include "log.h"

namespace hsm::game {

using namespace hsm::ue;

namespace {
constexpr double kScanEveryMs = 10000;   // chests are placed with the level: a slow look finds them
constexpr double kPairEveryMs = 1000;
constexpr uint64_t kLogFirst = 10;
constexpr const char* kChestClasses[] = { "BP_Container_Chest_001_C", "BP_Container_Chest_002_C", "BP_Container_Chest_003_C",
                                          "BP_Container_Chest_004_C", "BP_Container_Chest_005_C" };
constexpr const char* kLimbs[] = { "hand_r", "lowerarm_r", "upperarm_r", "hand_l", "lowerarm_l", "upperarm_l", "foot_r", "foot_l" };
struct FNameArray { const FName* data = nullptr; int32_t num = 0, max = 0; };   // TArray<FName>
}  // namespace

void ChestLids::Clear() {
    chests_.clear();
    done_.clear();
    nextScanMs_ = nextPairMs_ = 0;
}

void ChestLids::Tick(double nowMs, const std::vector<UObject*>& fighters) {
    if (fighters.empty() || nowMs < nextPairMs_) return;
    nextPairMs_ = nowMs + kPairEveryMs;
    if (nowMs >= nextScanMs_) {
        nextScanMs_ = nowMs + kScanEveryMs;
        chests_.clear();
        for (const char* cls : kChestClasses)
            for (UObject* chest : FindInstances(cls))
                if (UObject** box = Field<UObject*>(chest, "Box"); box && *box && !IsGarbage(chest)) chests_.push_back({ *box, ObjectIndex(*box) });
    }
    if (chests_.empty()) return;
    if (!lib_)
        if (UClass* lib = FindClass("CollisionDisablerFunctionLibrary")) lib_ = DefaultObject(lib);
    if (!lib_) return;
    static std::vector<FName> limbs;
    if (limbs.empty()) for (const char* b : kLimbs) limbs.push_back(MakeName(b));
    const FNameArray names{ limbs.data(), int32_t(limbs.size()), int32_t(limbs.size()) };
    for (UObject* w : fighters) {
        UObject** meshField = w ? Field<UObject*>(w, "Mesh") : nullptr;
        UObject* mesh = meshField ? *meshField : nullptr;
        if (!mesh || IsGarbage(mesh)) continue;
        for (const auto& [box, index] : chests_) {
            if (ObjectAt(index) != box || IsGarbage(box) || done_.count({ box, mesh, ObjectIndex(mesh) })) continue;
            if (!Call(lib_, "DisableCollision_SkeletalVsSingleBody").set<UObject*>("SkeletalMesh", mesh).set<FNameArray>("SkeletalMeshBones", names)
                     .set<UObject*>("SingleBody", box).setReal("TimeToExpiration", -1).invoke())
                continue;
            done_.insert({ box, mesh, ObjectIndex(mesh) });
            if (++disabled_ <= kLogFirst) HSM_LOG("chests: %s's limbs no longer catch on %s", ObjectName(w).c_str(), ObjectName(OuterOf(box)).c_str());
        }
    }
}

}  // namespace hsm::game

#include "gameplay/gear.h"
#include "log.h"
#include "match/hsm_match.h"
#include "util/hsm_names.h"
#include <algorithm>

namespace hsm::game {
using namespace hsm::ue;
namespace {

constexpr const char* kGeneratorClass = "BP_Generator_Characters_Random_C";
constexpr const char* kWillieClass = "Willie_BP_C";
constexpr const char* kSaveGameClass = "SG_GameProgress_C";
constexpr const char* kSavePassport = "Free Mode Player Character 1";   // SG_GameProgress_C field carrying the blob
// Outside Free Mode the generator picks weapons within the tier's Equipment Budget (hsm::kTierBudget, shared with the
// lobby page's hint), so a zero budget (as SpawnBot uses) would starve the loadout.

UObject* GameplayStatics() {
    UObject* cls = FindObject("GameplayStatics", "Class");
    return cls ? DefaultObject(reinterpret_cast<UClass*>(cls)) : nullptr;
}

// Same serialization as Adapter::ExportPassport: the generated passport copied into a fresh SG_GameProgress_C.
bool SerializePassport(Adapter& a, UObject* statics, UClass* sgClass, const void* passport, UFunction* generate,
                       std::vector<uint8_t>& out) {
    if (!passport) { HSM_LOG("gear: Generate Character has no Character Passport"); return false; }
    FProperty* src = nullptr;
    for (FProperty* p : Params(generate)) if (SameNameAscii(PropertyName(p), "Character Passport")) src = p;
    Call c(statics, "CreateSaveGameObject");
    c.set<UClass*>("SaveGameClass", sgClass);
    UObject* sg = c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
    FProperty* dst = sg ? FindProperty(ClassOf(sg), kSavePassport) : nullptr;
    if (!src || !dst || PropertySize(src) != PropertySize(dst) || PropertyType(src) != PropertyType(dst)) {
        HSM_LOG("gear: passport property missing or mismatched in the generated loadout");
        return false;
    }
    CopyPropertyValue(dst, reinterpret_cast<uint8_t*>(sg) + PropertyOffset(dst), passport);
    return a.SaveToMemory(sg, out);
}

}  // namespace

std::vector<uint8_t> RandomPassportBlob(Adapter& a, int tier) {
    std::vector<uint8_t> blob;
    const int rank = std::clamp(tier, 0, 7);
    UObject* statics = GameplayStatics();
    UClass* gen = FindClass(kGeneratorClass);
    UClass* willie = FindClass(kWillieClass);
    UClass* sgClass = FindClass(kSaveGameClass);
    if (!a.World() || !statics || !gen || !willie || !sgClass) {
        HSM_LOG("gear: random tier %d needs a world, GameplayStatics, %s, %s and %s", rank, kGeneratorClass, kWillieClass, kSaveGameClass);
        return blob;
    }

    FTransform t;
    Call begin(statics, "BeginDeferredActorSpawnFromClass");
    begin.set<UObject*>("WorldContextObject", a.World()).set<UClass*>("ActorClass", gen).set("SpawnTransform", t)
         .set<uint8_t>("CollisionHandlingOverride", 0).set<UObject*>("Owner", nullptr).set<uint8_t>("TransformScaleMethod", 1);
    UObject* g = begin.invoke() ? begin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) { HSM_LOG("gear: generator spawn failed (tier %d)", rank); return blob; }
    if (double* budget = Field<double>(g, "Equipment Budget")) *budget = kTierBudget[rank];
    Call fin(statics, "FinishSpawningActor");
    fin.set<UObject*>("Actor", g).set("SpawnTransform", t).set<uint8_t>("TransformScaleMethod", 1);
    g = fin.invoke() ? fin.get<UObject*>("ReturnValue") : nullptr;
    if (!g) { HSM_LOG("gear: FinishSpawningActor failed for the generator (tier %d)", rank); return blob; }

    Call make(g, "Generate Character");
    make.set<UClass*>("Actor Class", willie).set<uint8_t>("Nationality", 0).set<uint8_t>("Tier", uint8_t(rank))
        .set<bool>("Is Mercenary", false).set<bool>("Ignore Budget", false);
    bool ok = make.invoke();
    if (ok) ok = SerializePassport(a, statics, sgClass, make.paramPtr("Character Passport"), make.function(), blob);
    else HSM_LOG("gear: Generate Character failed (tier %d)", rank);
    a.DestroyActor(g);
    if (!ok) { blob.clear(); HSM_LOG("gear: no loadout generated for tier %d", rank); }
    else HSM_LOG("gear: generated random tier %d loadout (%zu bytes)", rank, blob.size());
    return blob;
}

}  // namespace hsm::game

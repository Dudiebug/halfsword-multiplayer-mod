// The object walk's class skip list (session/hsm_object_skip.h): gore, blood, decals, particle and Niagara actors, effects and
// the mod's own helpers are never objects; real props, armour, weapons and fighters are untouched.
#include "session/hsm_object_skip.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestSkipped() {
    // The classes found in the game's content (Effects/Blood/NewBloodSystem, Effects/Bugs) and the engine's own.
    for (const char* n : {"BP_BloodDecal_C", "Blood_BP_P4_C", "Blood_BP_PT_C", "RunningBlood_BP_C", "BP_MeshBloodSim_C",
                          "BP_MeshBloodSimManager_C", "Blood_Weapon_Trailing_C", "Blood_Fountain_System_EA_C",
                          "NiagaraActor", "Emitter", "DecalActor", "ParticleEventManager", "N_Wasps_C", "N_Flies_I_C",
                          "BP_Gore_Gib_C", "BP_Gib_Skull_C", "GibActor", "FX_Impact_C", "BP_VFX_Spark_C", "P_Sparks_Metal_C",
                          "BP_SplatterDrop_C", "HSMGhost", "HSM_Marker_C", "BP_Generator_Weapons_Random_C",
                          "BP_Generator_Characters_Random_C", "BP_HitEffect_C", "ImpactEffects_C"})
        CHECK(ObjectClassSkipped(n));
}

static void TestKept() {
    // Real physics objects, armour, weapons, fighters and look-alike words stay objects.
    for (const char* n : {"BP_Barrel_C", "StaticMeshActor", "BP_Stool_C", "BP_Plank_C", "BP_Crate_Large_C", "Willie_BP_C",
                          "BP_Armor_Head_Kettle_Helm_A_C", "BP_Weapon_Sword_C", "BP_Gibson_C", "FlyingCrate_C", "BP_Decalcomania_C",
                          "BP_Sparkle_C", "BP_Wasp_Nest_C", "Fliesenleger", "BP_Bloodstone_C", "BP_Particleboard_C", "Actor", "Hsmith_C"})
        CHECK(!ObjectClassSkipped(n));
    CHECK(!ObjectClassSkipped("")); CHECK(!ObjectClassSkipped(nullptr));
    CHECK(!ObjectClassSkipped("BP_C"));
}

static void TestCasingAndSplitting() {
    CHECK(!ObjectClassSkipped("bp_blooddecal_c"));        // one lower-case word "blooddecal": not split, not a word of the list
    CHECK(ObjectClassSkipped("bp_blood_decal_c"));                // '_' splits whatever the casing
    CHECK(ObjectClassSkipped("BP_BLOOD_C"));                      // casing folded
    CHECK(ObjectClassSkipped("Bp_Gore2_C"));                      // a digit ends a word
    CHECK(ObjectClassSkipped("BloodDecal"));                      // camel case
    CHECK(ObjectClassSkipped("BPBloodDecal"));                    // the end of an upper-case run
    CHECK(ObjectClassSkipped("NiagaraActor"));
    CHECK(!ObjectClassSkipped("Niagaraactor"));                   // one word, not on the list
    CHECK(ObjectClassSkipped("BP_Armor_Blood"));                  // the last word counts (no trailing separator)
    CHECK(ObjectClassSkipped("Blood"));                           // the only word
    CHECK(ObjectClassSkipped("Blood_"));
}

static void TestPaths() {
    CHECK(ObjectClassSkipped("/Game/Effects/Blood/NewBloodSystem/BP_BloodDecal.BP_BloodDecal_C"));
    CHECK(!ObjectClassSkipped("/Game/Effects/Blood/Props/BP_Barrel.BP_Barrel_C"));   // only the last segment names the class
    CHECK(!ObjectClassSkipped("/Script/Engine.StaticMeshActor"));
    CHECK(ObjectClassSkipped("/Script/Niagara.NiagaraActor"));
    CHECK(ObjectClassSkipped("/Script/Engine.Emitter"));
}

int main() {
    TestSkipped(); TestKept(); TestCasingAndSplitting(); TestPaths();
    if (g_fail) { std::printf("test_hsm_object_skip: %d FAILED\n", g_fail); return 1; }
    std::printf("test_hsm_object_skip: all passed\n");
    return 0;
}

// Team kit colours: the four teams stay apart, free-for-all tints nothing, each setting tints the slots it names, a blade
// never, and the material-parameter and slot classifiers.
#include "match/hsm_team_kit.h"
#include "match/hsm_roles.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool None(const Rgba& c) { return c.a == 0.f; }

// Distances are Euclidean over r, g, b in 0..1 (KitDistance). The palette's closest pair, red and gold, is 0.59 apart; the
// thresholds sit about half that for the base (the cloth is the main read of a team) and a little under half for the
// derived trim and plate (trim lifts every channel 40% toward white, plate scales it to 60%: both shrink the gaps).
constexpr float kMinBase = 0.50f, kMinDerived = 0.30f;

static void TestTeamsStayApart() {
    for (int a = 0; a < 4; ++a)
        for (int b = a + 1; b < 4; ++b) {
            const KitColors x = KitColorsFor(a), y = KitColorsFor(b);
            CHECK(KitDistance(x.base, y.base) >= kMinBase);
            CHECK(KitDistance(x.trim, y.trim) >= kMinDerived);
            CHECK(KitDistance(x.plate, y.plate) >= kMinDerived);
        }
    // Plate is darker than the cloth, trim lighter, on every channel, and all three are opaque.
    for (int t = 0; t < 4; ++t) {
        const KitColors k = KitColorsFor(t);
        CHECK(k.base.a == 1.f && k.trim.a == 1.f && k.plate.a == 1.f);
        CHECK(k.plate.r < k.base.r && k.plate.g < k.base.g && k.plate.b < k.base.b);
        CHECK(k.trim.r > k.base.r && k.trim.g > k.base.g && k.trim.b > k.base.b);
        CHECK(k.trim.r <= 1.f && k.trim.g <= 1.f && k.trim.b <= 1.f);
        const Rgba tc = TeamColor(t);
        CHECK(k.base.r == tc.r && k.base.g == tc.g && k.base.b == tc.b);   // derived from TeamColor
    }
}

static void TestNoTeamTintsNothing() {
    for (int t : {int(kNoTeam), -7, 4, 99}) {
        CHECK(None(KitColorsFor(t).base) && None(KitColorsFor(t).trim) && None(KitColorsFor(t).plate));
        for (uint8_t k = 0; k < kTeamKitCount; ++k)
            for (uint8_t s = 0; s < kKitSlotCount; ++s) CHECK(None(KitColor(TeamKit(k), KitSlot(s), t)) && None(KitColor(TeamKit(k), KitSlot(s), t, true)));
    }
    // The formats without teams resolve everyone to kNoTeam, so they tint nothing; the team formats give a team.
    const std::vector<TeamRequest> three = {{1, kNoTeam, kNoTeam}, {2, kNoTeam, kNoTeam}, {3, kNoTeam, kNoTeam}};
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding})
        for (const auto& [slot, team] : ResolveTeams(three, f, 2))
            for (uint8_t s = 0; s < kKitSlotCount; ++s) CHECK(None(KitColor(TeamKit::Full, KitSlot(s), team)));
    int tinted = 0;
    for (const auto& [slot, team] : ResolveTeams(three, Format::TwoTeams, 2)) if (!None(KitColor(TeamKit::Full, KitSlot::Cloth, team))) ++tinted;
    CHECK(tinted == 3);
}

static void TestOffTabardFull() {
    for (int t = 0; t < 4; ++t)
        for (uint8_t s = 0; s < kKitSlotCount; ++s) CHECK(None(KitColor(TeamKit::Off, KitSlot(s), t)) && !KitTints(TeamKit::Off, KitSlot(s)));
    // Tabard: the tabard only.
    for (int t = 0; t < 4; ++t) {
        const KitColors k = KitColorsFor(t);
        for (uint8_t s = 0; s < kKitSlotCount; ++s) {
            const Rgba c = KitColor(TeamKit::Tabard, KitSlot(s), t);
            if (KitSlot(s) == KitSlot::Tabard) CHECK(c.a == 1.f && c.r == k.base.r && c.g == k.base.g && c.b == k.base.b);
            else CHECK(None(c));
        }
    }
    // Full: cloth, tabard and grip take the team colour, plate the darkened one, a shield its colour and, for a trim parameter, the trim; never a blade.
    for (int t = 0; t < 4; ++t) {
        const KitColors k = KitColorsFor(t);
        auto same = [](const Rgba& a, const Rgba& b) { return a.a == b.a && a.r == b.r && a.g == b.g && a.b == b.b; };
        CHECK(same(KitColor(TeamKit::Full, KitSlot::Cloth, t), k.base));
        CHECK(same(KitColor(TeamKit::Full, KitSlot::Tabard, t), k.base));
        CHECK(same(KitColor(TeamKit::Full, KitSlot::Grip, t), k.base));
        CHECK(same(KitColor(TeamKit::Full, KitSlot::Plate, t), k.plate));
        CHECK(same(KitColor(TeamKit::Full, KitSlot::Shield, t), k.base) && same(KitColor(TeamKit::Full, KitSlot::Shield, t, true), k.trim));
        CHECK(same(KitColor(TeamKit::Full, KitSlot::Cloth, t, true), k.base));   // trim is a shield's alone
        CHECK(None(KitColor(TeamKit::Full, KitSlot::Blade, t)) && None(KitColor(TeamKit::Full, KitSlot::Blade, t, true)));
    }
    for (TeamKit kit : {TeamKit::Off, TeamKit::Tabard, TeamKit::Full}) CHECK(!KitTints(kit, KitSlot::Blade));   // blades stay steel
    CHECK(KitTints(TeamKit::Full, KitSlot::Plate) && !KitTints(TeamKit::Tabard, KitSlot::Plate) && !KitTints(TeamKit::Tabard, KitSlot::Cloth));
    CHECK(std::string(TeamKitName(TeamKit::Off)) == "Off" && std::string(TeamKitName(TeamKit::Tabard)) == "Tabard" && std::string(TeamKitName(TeamKit::Full)) == "Full");
}

static void TestEffectiveKit() {
    for (uint8_t k = 0; k < kTeamKitCount; ++k) {
        for (Format f : {Format::TwoTeams, Format::ThreeTeams}) CHECK(EffectiveKit(f, TeamKit(k)) == TeamKit(k));   // the row rules the Teams format
        for (Format f : {Format::PlayersVsBots, Format::Zombies}) CHECK(EffectiveKit(f, TeamKit(k)) == TeamKit::Full);   // no row: as before
    }
}

static void TestSanitize() {
    MatchConfig c;
    CHECK(c.teamKit == TeamKit::Full);                       // the default
    for (uint8_t k = 0; k < kTeamKitCount; ++k) { c.teamKit = TeamKit(k); CHECK(Sanitize(c).teamKit == TeamKit(k)); }
    c.teamKit = TeamKit(3); CHECK(Sanitize(c).teamKit == TeamKit::Full);
    c.teamKit = TeamKit(255); CHECK(Sanitize(c).teamKit == TeamKit::Full);
}

static void TestParamClassifier() {
    // Colours to set.
    for (const char* n : {"Color", "Colour", "Tint", "Dye", "FabricColor1", "Fabric Color 2", "Fabric_Colour_3", "LeatherColor", "Leather Tint",
                          "BaseColor", "Cloth Color", "TintColor", "custom_color_grip", "DyeColour"})
        CHECK(IsTintableParam(n));
    // Not colours to set: what a colour-ish name can mean instead, and names with no colour in them.
    for (const char* n : {"MetalColor", "Metal Tint", "Roughness", "RoughnessColor", "EmissiveColor", "Emissive Tint", "BloodColor", "Blood Tint",
                          "DirtColor", "Rust Color", "Wound Color", "ColorMask", "Opacity", "NormalIntensity", "SpecularColor", "GoreColor",
                          "ColorGrading", "Scratches", "Metallic", "Roughness Min", "", "UV Scale", "Tiling"})
        CHECK(!IsTintableParam(n));
    // The classifier reads names only; the client offers it vector parameters, so a scalar named like a colour never reaches it.
    // Trim: secondary colours.
    for (const char* n : {"FabricColor2", "Fabric Color 3", "TrimColor", "Edge Tint", "SecondaryColor", "Accent Color"}) CHECK(IsTrimParam(n));
    for (const char* n : {"FabricColor1", "Color", "Tint", "LeatherColor", "Fabric Color 1"}) CHECK(!IsTrimParam(n));
}

static void TestSlotClassifiers() {
    CHECK(SlotForArmorClass("BP_Armor_ModularCore_Body_Tabard_1_C") == KitSlot::Tabard);
    CHECK(SlotForArmorClass("BP_Armor_Modular_Core_Body_Tabard1_C") == KitSlot::Tabard);
    for (const char* n : {"BP_Armor_Body_Gambeson_A_T1_C", "BP_Armor_Legs_HosenA_C", "BP_Armor_Hosen_C_Black_C", "BP_Armor_Modular_Core_Chest_Gambeson_1_C",
                          "BP_Armor_Legs_Hosen_Arming_A_C"})
        CHECK(SlotForArmorClass(n) == KitSlot::Cloth);
    for (const char* n : {"BP_Armor_Helmet_Kettle_C", "BP_Armor_Body_Cuirass_A_C", "BP_Armor_Arms_Vambrace_C", "BP_Armor_Legs_Greaves_C", ""})
        CHECK(SlotForArmorClass(n) == KitSlot::Plate);
    CHECK(IsShieldClass("BP_Weapon_Shield_Heater_C") && IsShieldClass("Buckler_A_C") && IsShieldClass("TowerShield") && !IsShieldClass("BP_Weapon_Sword_C"));
    CHECK(SlotForWeaponPart("Grip", false) == KitSlot::Grip && SlotForWeaponPart("Handle_Leather", false) == KitSlot::Grip
          && SlotForWeaponPart("Haft", false) == KitSlot::Grip && SlotForWeaponPart("GripSK", false) == KitSlot::Grip);
    for (const char* n : {"Blade", "Blade1", "Guard", "Pommel", "Head", "Fuller", "BaseMesh", ""}) CHECK(SlotForWeaponPart(n, false) == KitSlot::Blade);
    for (const char* n : {"Blade", "Grip", "Face", "Boss"}) CHECK(SlotForWeaponPart(n, true) == KitSlot::Shield);   // every part of a shield
    CHECK(std::string(KitSlotName(KitSlot::Blade)) == "blade" && std::string(KitSlotName(KitSlot::Tabard)) == "tabard");
}

// Integration (Wave 2): a crossbow is dressed like any weapon (its handle takes the colour, the rest stays steel under Full kit, nothing under
// Tabard), but its bolts never are, whatever their meshes are called.
static void TestProjectilesAreNeverDressed() {
    CHECK(IsProjectileClass("BP_Weapon_Ranged_Projectle_Bolt1_C") && IsProjectileClass("BP_Weapon_Ranged_Projectile_Arrow_C") && IsProjectileClass("Bolt"));
    CHECK(!IsProjectileClass("BP_Weapon_Ranged_Weapon_CrossbowLight_C") && !IsProjectileClass("BP_Weapon_Sword_Arming_C") && !IsProjectileClass(""));
    CHECK(SlotForWeaponPart("Handle", false) == KitSlot::Grip && SlotForWeaponPart("CrossbowStock", false) == KitSlot::Blade);
    CHECK(KitTints(TeamKit::Full, KitSlot::Grip) && !KitTints(TeamKit::Tabard, KitSlot::Grip) && !KitTints(TeamKit::Full, KitSlot::Blade));
}

// Integration (Wave 2): every team format tints by team, the role formats' two sides included. A Riot / Juggernaut fighter's team comes from
// its role (AssignRoles), and the kit the lobby chose is the one worn there (only the bot formats force the full kit).
static void TestRoleFormatsTintByTeam() {
    for (Format f : {Format::TwoTeams, Format::TeamDuel, Format::Riot, Format::Juggernaut}) {
        CHECK(HasTeams(f));
        for (TeamKit k : {TeamKit::Off, TeamKit::Tabard, TeamKit::Full}) CHECK(EffectiveKit(f, k) == k);
    }
    CHECK(EffectiveKit(Format::PlayersVsBots, TeamKit::Off) == TeamKit::Full);
    for (Format f : {Format::Riot, Format::Juggernaut}) {
        const MatchConfig cfg = Sanitize(MatchConfig{f});
        const auto dealt = AssignRoles(f, cfg, {1, 2, 3, 4, 5}, 11, 0);
        bool side[2] = {false, false};
        for (const auto& [e, r] : dealt) {
            CHECK(r.team == 0 || r.team == 1);
            const Rgba full = KitColor(TeamKit::Full, KitSlot::Cloth, r.team), tab = KitColor(TeamKit::Tabard, KitSlot::Tabard, r.team);
            CHECK(full.a > 0.f && tab.a > 0.f && KitColor(TeamKit::Off, KitSlot::Cloth, r.team).a == 0.f);
            CHECK(KitColor(TeamKit::Tabard, KitSlot::Cloth, r.team).a == 0.f);                  // Tabard: the tabard alone
            CHECK(KitColor(TeamKit::Full, KitSlot::Blade, r.team).a == 0.f);                    // never a blade
            side[r.team] = true;
        }
        CHECK(side[0] && side[1]);
        CHECK(KitDistance(KitColor(TeamKit::Full, KitSlot::Cloth, 0), KitColor(TeamKit::Full, KitSlot::Cloth, 1)) >= kMinBase);   // the two sides read apart
    }
}

int main() {
    TestRoleFormatsTintByTeam();
    TestProjectilesAreNeverDressed();
    TestTeamsStayApart();
    TestNoTeamTintsNothing();
    TestOffTabardFull();
    TestEffectiveKit();
    TestSanitize();
    TestParamClassifier();
    TestSlotClassifiers();
    std::printf(g_fail ? "%d FAILED\n" : "all team kit tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}

// Team kit colours (the lobby's "Team colours" row). Game-independent rules; the client's Adapter::TintKit applies them.
//   KitColors / KitColor  - per team the kit colours (base, trim, plate), derived from TeamColor
//   KitTints              - which slot kinds take a colour under Tabard and under Full (a blade never does)
//   SlotForArmorClass / SlotForWeaponPart - which slot kind a worn piece or a weapon's mesh part is, from its names
//   IsTintableParam / IsTrimParam - which material parameter names are colours to set (and which of those are trim)
// Free-for-all, Duel and Last Man Standing have no team (kNoTeam): TeamColor has no colour there, so nothing is tinted.
#pragma once
#include "match/hsm_match.h"
#include <cmath>
#include <initializer_list>
#include <string>

namespace hsm {

// What a mesh is on a fighter. Plate: armour that is not cloth. Cloth: gambeson, hose, doublet and the like. Tabard: the
// surcoat worn over armour. Shield. Grip: a weapon's handle or wrap. Blade: every other part of a weapon (steel).
enum class KitSlot : uint8_t { Plate = 0, Cloth = 1, Tabard = 2, Shield = 3, Grip = 4, Blade = 5 };
constexpr uint8_t kKitSlotCount = 6;

inline const char* TeamKitName(TeamKit k) { return k == TeamKit::Off ? "Off" : k == TeamKit::Tabard ? "Tabard" : "Full"; }
inline const char* KitSlotName(KitSlot s) {
    switch (s) {
        case KitSlot::Plate: return "plate";
        case KitSlot::Cloth: return "cloth";
        case KitSlot::Tabard: return "tabard";
        case KitSlot::Shield: return "shield";
        case KitSlot::Grip: return "grip";
        default: return "blade";
    }
}

// A team's kit colours. base: TeamColor (cloth, tabard, grips, a shield's main colour). trim: the same colour lifted
// toward white (a shield's secondary parameters). plate: the colour darkened, so plate keeps a steel look with a tinge.
// All a == 0 without a team.
struct KitColors { Rgba base, trim, plate; };
inline constexpr float kKitTrimLift = 0.40f;     // trim = base + (1 - base) * lift
inline constexpr float kKitPlateScale = 0.60f;   // plate = base * scale
inline KitColors KitColorsFor(int team) {
    KitColors k;
    k.base = TeamColor(team);
    if (k.base.a == 0.f) return k;
    k.trim = {k.base.r + (1.f - k.base.r) * kKitTrimLift, k.base.g + (1.f - k.base.g) * kKitTrimLift, k.base.b + (1.f - k.base.b) * kKitTrimLift, 1.f};
    k.plate = {k.base.r * kKitPlateScale, k.base.g * kKitPlateScale, k.base.b * kKitPlateScale, 1.f};
    return k;
}
inline float KitDistance(const Rgba& a, const Rgba& b) {
    const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// The kit a fighter with a team wears: the lobby's Team colours row for the Teams format; the bot formats (two fixed
// teams, no row) always wear the full kit. Formats without teams give their fighters no team, so nothing reaches this.
inline TeamKit EffectiveKit(Format f, TeamKit chosen) { return IsBotFormat(f) || IsInfection(f) ? TeamKit::Full : chosen; }
// The colour index a fighter of `team` is tinted with: its team's, except Infection's, where the survivors keep their own colours
// (no tint: kNoTeam) and the infected go dark green (kInfectedKitTeam).
inline int8_t KitTeamFor(Format f, int8_t team) {
    if (IsInfection(f)) return team == kInfectedTeam ? int8_t(kInfectedKitTeam) : kNoTeam;
    return team;
}

// Which slot kinds a setting tints. A blade is never one of them.
inline bool KitTints(TeamKit kit, KitSlot slot) {
    if (kit == TeamKit::Tabard) return slot == KitSlot::Tabard;
    if (kit == TeamKit::Full) return slot != KitSlot::Blade;
    return false;
}
// The colour a slot's parameters take (trim: a secondary parameter, IsTrimParam); a == 0: leave the slot alone.
inline Rgba KitColor(TeamKit kit, KitSlot slot, int team, bool trim = false) {
    if (!KitTints(kit, slot)) return Rgba{};
    const KitColors k = KitColorsFor(team);
    if (slot == KitSlot::Plate) return k.plate;
    if (slot == KitSlot::Shield && trim) return k.trim;
    return k.base;
}

namespace kit_detail {
inline std::string Compact(const std::string& s) {   // lower case, letters and digits only
    std::string o;
    for (char c : s) {
        if (c >= 'A' && c <= 'Z') o += char(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) o += c;
    }
    return o;
}
inline bool HasAny(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words) if (s.find(w) != std::string::npos) return true;
    return false;
}
}  // namespace kit_detail

// A weapon class that is ammunition, not a weapon to dress: a crossbow bolt (BP_Weapon_Ranged_Projectle_Bolt1_C; "Projectle" is the
// game's spelling), an arrow or any projectile. The kit never touches it, whatever its meshes are called (a bolt's "shaft" is no grip).
inline bool IsProjectileClass(const std::string& className) {
    const std::string n = kit_detail::Compact(className);
    return kit_detail::HasAny(n, {"projectl", "bolt", "arrow"});
}

// A worn armour piece by its class name (BP_Armor_Body_Gambeson_A_T1, BP_Armor_Legs_Hosen_A, BP_Armor_ModularCore_Body_Tabard_1).
inline KitSlot SlotForArmorClass(const std::string& className) {
    const std::string n = kit_detail::Compact(className);
    if (n.find("tabard") != std::string::npos || n.find("surcoat") != std::string::npos) return KitSlot::Tabard;
    if (kit_detail::HasAny(n, {"gambeson", "hosen", "hose", "doublet", "shirt", "tunic", "robe", "cloak", "hood", "cloth", "shoe", "sock",
                               "underwear", "panties", "padded", "padding", "apron"}))
        return KitSlot::Cloth;
    return KitSlot::Plate;
}
// A weapon's mesh part by its component name; shield: the actor is a shield (class or its Shield flag). Guard, pommel,
// head and blade are steel; the handle and its wrap take colour.
inline KitSlot SlotForWeaponPart(const std::string& partName, bool shield) {
    if (shield) return KitSlot::Shield;
    const std::string n = kit_detail::Compact(partName);
    if (kit_detail::HasAny(n, {"grip", "handle", "wrap", "leather", "haft", "shaft"})) return KitSlot::Grip;
    return KitSlot::Blade;
}
inline bool IsShieldClass(const std::string& className) {
    const std::string n = kit_detail::Compact(className);
    return n.find("shield") != std::string::npos || n.find("buckler") != std::string::npos || n.find("pavise") != std::string::npos;
}

// A material parameter that is a plain colour to set. By name: it says colour, tint, dye, fabric, leather or cloth, and
// it does not say what else a colour-ish name can mean (metal, roughness, emissive, blood, dirt, a mask or an opacity).
inline bool IsTintableParam(const std::string& name) {
    const std::string n = kit_detail::Compact(name);
    if (!kit_detail::HasAny(n, {"color", "colour", "tint", "dye", "fabric", "leather", "cloth"})) return false;
    return !kit_detail::HasAny(n, {"metal", "rough", "emiss", "glow", "spec", "fresnel", "normal", "opacity", "mask", "occlusion", "blood", "gore",
                                   "wound", "dirt", "mud", "rust", "scratch", "wear", "damage", "sweat", "subsurface", "skin", "hair", "eye",
                                   "scar", "decal", "grad", "gamma", "contrast", "saturation", "brightness"});
}
// A tintable parameter that is a secondary colour (a trim, edge, accent, or the 2nd/3rd of a numbered set).
inline bool IsTrimParam(const std::string& name) {
    const std::string n = kit_detail::Compact(name);
    if (kit_detail::HasAny(n, {"trim", "secondary", "accent", "edge", "border", "rim", "detail"})) return true;
    return !n.empty() && (n.back() == '2' || n.back() == '3');
}

}  // namespace hsm

// Armour collision audit (HELP IMPROVE THE MOD only): where each of a fighter's armour collision pieces sits, named by the
// collision mesh it was built from, and whether that place makes sense for the piece (#1679 and #2638: "an invisible
// hitbox blocks hits on the back of someone who wears only a front chestpiece").
//
// The game builds one simulated static mesh per armour piece (Willie_BP.cpp Set_Up_Proxy_Collisions: the cuirass at
// spine_04, the head at 'head', arms and legs likewise). Each is placed from the bone's transform at that instant and then
// attached to the matching CM_* sphere with keep-world, so a piece placed wrongly at that instant stays wrongly placed:
// it keeps blocking weapons (Weapon, PhysicsBody and body channels block) wherever it ended up, drawn or not. The
// collision assets come in front and back halves (CM_Armor_Chest_Cuirass_Gothic_Breastplate_Torso_00N,
// ..._Backplate_Torso_00N), so the name says which side of the body a piece belongs on. This file sorts the name, judges a
// place, and words the log line; the DLL reads the components (gameplay/hit_probe.cpp).
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

namespace hsm::combat {

enum class ArmorFace { Front, Back, Skirt, Other };

inline bool NameHas(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// CM_Armor_Chest_Cuirass_Gothic_Backplate_Torso_003 is a back; ..._Brust_Back_Foulds_Torso_001 is the back of a brust's
// skirt. A skirt (foulds, tassets) hangs below the waist and is judged only by distance. A whole-body plate
// (Brust_Plate, Roman_Plate) and everything else is Other.
inline ArmorFace ClassifyCollisionMesh(const std::string& assetName) {
    if (NameHas(assetName, "Backplate") || NameHas(assetName, "_Back_")) return ArmorFace::Back;
    if (NameHas(assetName, "_Front_")) return ArmorFace::Front;
    if (NameHas(assetName, "Foulds") || NameHas(assetName, "Tassets")) return ArmorFace::Skirt;
    if (NameHas(assetName, "Breastplate") || NameHas(assetName, "Placard")) return ArmorFace::Front;
    return ArmorFace::Other;
}

inline const char* ArmorFaceName(ArmorFace f) {
    switch (f) {
        case ArmorFace::Front: return "front";
        case ArmorFace::Back: return "back";
        case ArmorFace::Skirt: return "skirt";
        default: return "other";
    }
}

// What a piece's place says about it. Seated is the only good one.
enum class ArmorVerdict {
    Seated,       // nothing odd about where it is
    Unread,       // no place could be read
    FarFromBody,  // further from the pelvis than any armour piece reaches
    Adrift,       // far from the CM sphere it is attached to (it should ride it)
    FrontBehind,  // a front piece behind the body
    BackAhead,    // a back piece in front of the body
    TorsoLow,     // a breastplate or backplate (Torso) below the pelvis
};

inline const char* ArmorVerdictName(ArmorVerdict v) {
    switch (v) {
        case ArmorVerdict::Seated: return "seated";
        case ArmorVerdict::Unread: return "unread";
        case ArmorVerdict::FarFromBody: return "far-from-body";
        case ArmorVerdict::Adrift: return "adrift";
        case ArmorVerdict::FrontBehind: return "front-behind";
        case ArmorVerdict::BackAhead: return "back-ahead";
        case ArmorVerdict::TorsoLow: return "torso-low";
    }
    return "?";
}

// The place is that of the geometry (the centre of its world bounds), not the component's pivot: the game puts the pivot
// at the character root and models the collision in character space, so a pivot reads 100 cm or more under the body on a
// healthy fighter.
//
// Distances in cm. A fighter is about 180 cm tall, so no piece of armour has its centre 120 cm from the pelvis; a torso
// piece's centre is above the pelvis, never 25 cm below it; a front or back plate is 8 cm either way of the pelvis plane
// before it counts as on the wrong side; a piece rides its sphere within a hand's width of where it was put.
constexpr double kArmorFarCm = 120.0;
constexpr double kArmorSideCm = 8.0;
constexpr double kArmorTorsoLowCm = 25.0;
constexpr double kArmorAdriftCm = 60.0;

struct ArmorPlace {
    bool haveBody = false;         // forward/right/up are from the pelvis in the body's own frame
    double forward = 0, right = 0, up = 0;
    bool haveParent = false;       // fromParentCm is the distance from the geometry's centre to the CM sphere it is attached to
    double fromParentCm = 0;
    bool haveSize = false;         // sizeCm is the largest half-extent of the geometry's world box
    double sizeCm = 0;
};

inline ArmorVerdict JudgeArmorPiece(const std::string& assetName, const ArmorPlace& p) {
    if (!p.haveBody) return ArmorVerdict::Unread;
    const double dist = std::sqrt(p.forward * p.forward + p.right * p.right + p.up * p.up);
    if (!std::isfinite(dist)) return ArmorVerdict::Unread;
    if (dist > kArmorFarCm) return ArmorVerdict::FarFromBody;
    const ArmorFace face = ClassifyCollisionMesh(assetName);
    if (face == ArmorFace::Front && p.forward < -kArmorSideCm) return ArmorVerdict::FrontBehind;
    if (face == ArmorFace::Back && p.forward > kArmorSideCm) return ArmorVerdict::BackAhead;
    if ((face == ArmorFace::Front || face == ArmorFace::Back) && NameHas(assetName, "Torso") && p.up < -kArmorTorsoLowCm)
        return ArmorVerdict::TorsoLow;
    if (p.haveParent && p.fromParentCm > kArmorAdriftCm) return ArmorVerdict::Adrift;
    return ArmorVerdict::Seated;
}

// The part of a piece's state that is a change worth a new line: the verdict and the place in 25 cm steps (the log of a
// fighter walking about must not fill with a piece riding along).
inline std::string ArmorChangeKey(const std::string& assetName, ArmorVerdict v, const ArmorPlace& p) {
    auto step = [](double cm) { return int(std::floor(cm / 25.0)) * 25; };
    char buf[128];
    if (p.haveBody) std::snprintf(buf, sizeof buf, "%s %d/%d/%d", ArmorVerdictName(v), step(p.forward), step(p.right), step(p.up));
    else std::snprintf(buf, sizeof buf, "%s -", ArmorVerdictName(v));
    return assetName + " " + buf;
}

// "probe: armor e2 NODE_AddStaticMeshComponent-50 <- CM_Cuirass mesh CM_Armor_..._Breastplate_Torso_003 (front) seated at fwd 3 right 0 up 12,
// 4 cm from its sphere, half-size 28 cm"
inline std::string ArmorLine(const std::string& who, const std::string& comp, const std::string& parent, const std::string& assetName,
                              ArmorVerdict v, const ArmorPlace& p) {
    char where[96] = "at ?";
    if (p.haveBody) std::snprintf(where, sizeof where, "at fwd %.0f right %.0f up %.0f", p.forward, p.right, p.up);
    char sphere[96] = "";
    int n = 0;
    if (p.haveParent) n = std::snprintf(sphere, sizeof sphere, ", %.0f cm from its sphere", p.fromParentCm);
    if (p.haveSize && n >= 0 && size_t(n) < sizeof sphere) std::snprintf(sphere + n, sizeof sphere - size_t(n), ", half-size %.0f cm", p.sizeCm);
    return "probe: armor " + who + " " + comp + " <- " + (parent.empty() ? std::string("none") : parent) + " mesh " +
           (assetName.empty() ? std::string("none") : assetName) + " (" + ArmorFaceName(ClassifyCollisionMesh(assetName)) + ") " +
           ArmorVerdictName(v) + " " + where + sphere;
}

}  // namespace hsm::combat

// Unit tests for hsm_armor_audit.h. Run via implementation/shared/run_tests.sh test_hsm_armor_audit.
//
// #1679 / #2638: a front chestpiece's collision must not sit behind the body. The names are the game's own collision assets
// (Content/Assets/Collisions/Character).
#include "combat/hsm_armor_audit.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

static ArmorPlace At(double fwd, double right, double up) {
    ArmorPlace p;
    p.haveBody = true;
    p.forward = fwd; p.right = right; p.up = up;
    return p;
}

static void TestClassify() {
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Gothic_Breastplate_Torso_003") == ArmorFace::Front);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Breastplate_Torso_001") == ArmorFace::Front);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Placard_001") == ArmorFace::Front);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Roman_Placard_Torso_002") == ArmorFace::Front);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Gothic_Backplate_Torso_004") == ArmorFace::Back);
    // The back of a brust's skirt is a back, its front half a front; both sides of the match are named "Foulds".
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Brust_Back_Foulds_Torso_001") == ArmorFace::Back);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Brust_Front_Foulds_Torso_002") == ArmorFace::Front);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Roman_Foulds_Torso_003") == ArmorFace::Skirt);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Gothic_Tassets_Leg_L_003") == ArmorFace::Skirt);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Breastplate_Tassets_Leg_R_005") == ArmorFace::Skirt);
    // A whole plate and things that are not chest armour say nothing about a side.
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Brust_Plate_Torso_001") == ArmorFace::Other);
    CHECK(ClassifyCollisionMesh("CM_Armor_Chest_Cuirass_Roman_Plate_Torso_015") == ArmorFace::Other);
    CHECK(ClassifyCollisionMesh("CM_Armor_Arm_Harness_Arm_L_001") == ArmorFace::Other);
    CHECK(ClassifyCollisionMesh("") == ArmorFace::Other);
}

// The report: only a front chestpiece worn, and the back blocks hits.
static void TestFrontPlateBehindTheBody() {
    const std::string front = "CM_Armor_Chest_Cuirass_Gothic_Breastplate_Torso_003";
    CHECK(JudgeArmorPiece(front, At(10, 0, 20)) == ArmorVerdict::Seated);
    CHECK(JudgeArmorPiece(front, At(0, 0, 20)) == ArmorVerdict::Seated);
    CHECK(JudgeArmorPiece(front, At(-8, 0, 20)) == ArmorVerdict::Seated);   // the limit itself
    CHECK(JudgeArmorPiece(front, At(-20, 0, 20)) == ArmorVerdict::FrontBehind);
    CHECK(JudgeArmorPiece(front, At(-60, 5, 20)) == ArmorVerdict::FrontBehind);
}

static void TestBackPlateAheadOfTheBody() {
    const std::string back = "CM_Armor_Chest_Cuirass_Gothic_Backplate_Torso_004";
    CHECK(JudgeArmorPiece(back, At(-10, 0, 20)) == ArmorVerdict::Seated);
    CHECK(JudgeArmorPiece(back, At(8, 0, 20)) == ArmorVerdict::Seated);
    CHECK(JudgeArmorPiece(back, At(30, 0, 20)) == ArmorVerdict::BackAhead);
}

// Thresholds: a cuirass plate under the pelvis, a head piece far under the head, and an arm piece a body-width out are not
// on the body (the place is the geometry's centre, not the component pivot).
static void TestPiecesFarFromTheBody() {
    const std::string chest = "CM_Armor_Chest_Cuirass_Gothic_Breastplate_Torso_003";
    CHECK(JudgeArmorPiece(chest, At(0, 0, -100)) == ArmorVerdict::TorsoLow);
    CHECK(JudgeArmorPiece(chest, At(0, 0, -30)) == ArmorVerdict::TorsoLow);
    CHECK(JudgeArmorPiece(chest, At(0, 0, -20)) == ArmorVerdict::Seated);
    CHECK(JudgeArmorPiece("CM_Armor_Head_Helmet_001", At(0, 0, -200)) == ArmorVerdict::FarFromBody);
    CHECK(JudgeArmorPiece("CM_Armor_Arm_Harness_Arm_L_001", At(75, -175, -50)) == ArmorVerdict::FarFromBody);
    // A far piece is named far whatever side it is, and before the side is looked at.
    CHECK(JudgeArmorPiece(chest, At(-300, 0, 20)) == ArmorVerdict::FarFromBody);
    // A leg piece low on the body is where it should be.
    CHECK(JudgeArmorPiece("CM_Armor_Leg_Greaves_Leg_L_001", At(5, 10, -80)) == ArmorVerdict::Seated);
    // A skirt hangs below the waist.
    CHECK(JudgeArmorPiece("CM_Armor_Chest_Cuirass_Roman_Foulds_Torso_003", At(0, 0, -40)) == ArmorVerdict::Seated);
}

static void TestAdriftNeedsASphere() {
    ArmorPlace p = At(5, 0, 20);
    CHECK(JudgeArmorPiece("CM_Armor_Chest_Breastplate_Torso_001", p) == ArmorVerdict::Seated);
    p.haveParent = true;
    p.fromParentCm = 20;
    CHECK(JudgeArmorPiece("CM_Armor_Chest_Breastplate_Torso_001", p) == ArmorVerdict::Seated);
    p.fromParentCm = 61;
    CHECK(JudgeArmorPiece("CM_Armor_Chest_Breastplate_Torso_001", p) == ArmorVerdict::Adrift);
    // A wrong side outranks a drift: it is the more specific finding.
    p = At(-30, 0, 20);
    p.haveParent = true;
    p.fromParentCm = 90;
    CHECK(JudgeArmorPiece("CM_Armor_Chest_Breastplate_Torso_001", p) == ArmorVerdict::FrontBehind);
}

static void TestUnread() {
    ArmorPlace none;
    CHECK(JudgeArmorPiece("CM_Armor_Chest_Breastplate_Torso_001", none) == ArmorVerdict::Unread);
    ArmorPlace nan = At(std::nan(""), 0, 0);
    CHECK(JudgeArmorPiece("CM_Armor_Chest_Breastplate_Torso_001", nan) == ArmorVerdict::Unread);
}

static void TestChangeKey() {
    const std::string n = "CM_Armor_Chest_Breastplate_Torso_001";
    // Walking about inside one 25 cm step is no change; crossing the step or the verdict is.
    CHECK(ArmorChangeKey(n, ArmorVerdict::Seated, At(3, 1, 30)) == ArmorChangeKey(n, ArmorVerdict::Seated, At(8, 4, 40)));
    CHECK(ArmorChangeKey(n, ArmorVerdict::Seated, At(3, 1, 30)) != ArmorChangeKey(n, ArmorVerdict::Seated, At(-3, 1, 30)));
    CHECK(ArmorChangeKey(n, ArmorVerdict::Seated, At(3, 1, 30)) != ArmorChangeKey(n, ArmorVerdict::Adrift, At(3, 1, 30)));
    CHECK(ArmorChangeKey(n, ArmorVerdict::Seated, At(3, 1, 30)) != ArmorChangeKey("CM_Armor_Chest_Breastplate_Torso_002", ArmorVerdict::Seated, At(3, 1, 30)));
    CHECK(ArmorChangeKey(n, ArmorVerdict::Unread, ArmorPlace{}) != ArmorChangeKey(n, ArmorVerdict::Seated, At(0, 0, 0)));
}

static void TestLine() {
    ArmorPlace p = At(-20.4, 3.2, 12.0);
    p.haveParent = true;
    p.fromParentCm = 4.4;
    const std::string line = ArmorLine("e2", "NODE_AddStaticMeshComponent-50", "CM_Cuirass", "CM_Armor_Chest_Cuirass_Gothic_Breastplate_Torso_003",
                                       ArmorVerdict::FrontBehind, p);
    CHECK(line == "probe: armor e2 NODE_AddStaticMeshComponent-50 <- CM_Cuirass mesh CM_Armor_Chest_Cuirass_Gothic_Breastplate_Torso_003 (front) "
                  "front-behind at fwd -20 right 3 up 12, 4 cm from its sphere");   // no size read: none printed
    const std::string bare = ArmorLine("mine", "c", "", "", ArmorVerdict::Unread, ArmorPlace{});
    CHECK(Has(bare, "<- none mesh none (other) unread at ?"));
    CHECK(!Has(bare, "sphere"));
    CHECK(!Has(bare, "half-size"));
    ArmorPlace sized = At(3, 0, 12);
    sized.haveSize = true;
    sized.sizeCm = 27.6;
    CHECK(ArmorLine("e2", "c", "CM_Cuirass", "m", ArmorVerdict::Seated, sized) == "probe: armor e2 c <- CM_Cuirass mesh m (other) seated at fwd 3 right 0 up 12, half-size 28 cm");
    sized.haveParent = true;
    sized.fromParentCm = 4.4;
    CHECK(Has(ArmorLine("e2", "c", "CM_Cuirass", "m", ArmorVerdict::Seated, sized), ", 4 cm from its sphere, half-size 28 cm"));
}

int main() {
    TestClassify();
    TestFrontPlateBehindTheBody();
    TestBackPlateAheadOfTheBody();
    TestPiecesFarFromTheBody();
    TestAdriftNeedsASphere();
    TestUnread();
    TestChangeKey();
    TestLine();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_armor_audit: all passed\n");
    return 0;
}

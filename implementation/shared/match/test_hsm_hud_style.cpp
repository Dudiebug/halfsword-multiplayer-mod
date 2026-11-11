// HUD palette (MATCH_ROUNDS_UI_CHAT_PLAN 3.1): the values of the plan, and every text colour readable on its panel.
#include "match/hsm_hud_style.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::style;

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
static bool Same(Rgba a, Rgba b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }

static void TestPlanValues() {
    CHECK(Same(kSmoke, {0.02f, 0.017f, 0.014f, 0.82f}));
    CHECK(Same(kBone, {0.93f, 0.89f, 0.80f, 1}));
    CHECK(Same(kBlood, {0.70f, 0.05f, 0.04f, 1}));
    CHECK(Same(kIron, {0.45f, 0.47f, 0.50f, 1}));
    CHECK(Same(kOk, {0.35f, 0.65f, 0.25f, 1}));
    CHECK(Same(kWarn, {0.85f, 0.62f, 0.05f, 1}));
    CHECK(Same(kSceneGrey, {0.18f, 0.18f, 0.18f, 1}));
}

static void TestContrastMath() {
    const Rgba white{1, 1, 1, 1}, black{0, 0, 0, 1};
    CHECK(Near(Luminance(white), 1) && Near(Luminance(black), 0));
    CHECK(Near(Luminance({1, 0, 0, 1}), 0.2126) && Near(Luminance({0, 1, 0, 1}), 0.7152) && Near(Luminance({0, 0, 1, 1}), 0.0722));
    CHECK(Near(Contrast(white, black), 21, 1e-3) && Near(Contrast(black, white), 21, 1e-3));   // either order
    CHECK(Near(Contrast(kIron, kIron), 1));
    CHECK(Same(Over({0.2f, 0.4f, 0.6f, 1}, white), {0.2f, 0.4f, 0.6f, 1}));                  // opaque: the top
    CHECK(Same(Over({0.2f, 0.4f, 0.6f, 0}, {0.5f, 0.5f, 0.5f, 1}), {0.5f, 0.5f, 0.5f, 1}));  // clear: what is under
    CHECK(Same(Over({1, 0, 0, 0.25f}, {0, 0, 1, 1}), {0.25f, 0, 0.75f, 1}));
    CHECK(Same(Over({1, 0, 0, 0.5f}, {0, 0, 1, 0.5f}), {2 / 3.f, 0, 1 / 3.f, 0.75f}));       // both see-through
    CHECK(Same(Over({1, 1, 1, 0}, {1, 1, 1, 0}), {0, 0, 0, 0}));                               // nothing over nothing
    CHECK(Same(Over(kSmoke, kSceneGrey), PanelOverScene()));
    CHECK(Near(TextContrast(kBone), Contrast(kBone, PanelOverScene())));
    CHECK(Near(TextContrast({kBone.r, kBone.g, kBone.b, 0.5f}), Contrast(Over({kBone.r, kBone.g, kBone.b, 0.5f}, PanelOverScene()), PanelOverScene())));
    CHECK(Same(Faded(kBone, 0.25f), {kBone.r, kBone.g, kBone.b, 0.25f}));
    CHECK(Same(Faded({1, 1, 1, 0.8f}, 0.5f), {1, 1, 1, 0.4f}));
}

static void TestTextIsReadable() {
    for (Rgba c : {kBone, kIron, kOk, kWarn, kBloodInk}) {
        CHECK(TextContrast(c) >= 4.5);
        CHECK(Contrast(c, Over(kSmoke, {0, 0, 0, 1})) >= 4.5);                               // the panel over a black scene
    }
    for (int t = -1; t <= 4; ++t) CHECK(TextContrast(TeamInk(t)) >= 4.5);
    CHECK(TextContrast(kBlood) < 4.5);            // the accent is for frames and swatches; text takes kBloodInk
    for (int t = 0; t < 4; ++t) CHECK(TextContrast(TeamColor(t)) < 4.5 || t == 3);   // tabard tints are too dark for text
}

static void TestTeamInk() {
    Rgba red = TeamInk(0), blue = TeamInk(1), green = TeamInk(2), gold = TeamInk(3);
    CHECK(red.r > red.g && red.r > red.b);        // each keeps its team's hue
    CHECK(blue.b > blue.r && blue.b > blue.g);
    CHECK(green.g > green.r && green.g > green.b);
    CHECK(gold.r >= gold.g && gold.g > gold.b);
    CHECK(Same(TeamInk(-1), kBone) && Same(TeamInk(4), kBone));   // no team (free-for-all): bone
    for (int t = 0; t < 4; ++t) CHECK(!Same(TeamInk(t), kBone));   // every team stands out from it
    for (int t = -1; t <= 4; ++t) CHECK(Near(TeamInk(t).a, 1));
}

static void TestMenuPalette() {
    UseMenuLook(MenuLook::Game);
    // Every menu text colour reads on every parchment face it can sit on; the light text reads on the backdrop and on START.
    for (Rgba face : {kMenuPanel, kMenuCard, kMenuCardHover})
        for (Rgba c : {kMenuText, kMenuDim, kMenuAccentInk, kMenuOk, kMenuWarn}) CHECK(Contrast(c, face) >= 4.5);
    for (int t = -1; t < 4; ++t) CHECK(Contrast(OnParchment(TeamInk(t)), kMenuPanel) >= 4.5);
    CHECK(Contrast(kMenuLight, kMenuAccent) >= 4.5);
    CHECK(Contrast(kMenuLight, Over(kMenuBg, {0, 0, 0, 1})) >= 4.5);
    CHECK(Contrast(kMenuLight, Over(kMenuBg, kMenuBackdrop)) >= 4.5);
    // Hover shows: brighter parchment; frames and rules are darker than any face.
    CHECK(Luminance(kMenuCard) > Luminance(kMenuPanel) && Luminance(kMenuCardHover) > Luminance(kMenuCard));
    CHECK(Luminance(kMenuFrameHover) > Luminance(kMenuFrame) && Luminance(kMenuFrameHover) < Luminance(kMenuPanel));
    CHECK(kMenuAccent.r > 10 * kMenuAccent.g && kMenuAccentInk.r > 10 * kMenuAccentInk.g);   // rubric red, not orange
    UseMenuLook(MenuLook::Game);
}

// The dark look: light text reads on every face, laid over black and over a mid-grey scene; the faces are almost opaque
// and a page's background is opaque; light ink stays.
static void TestDarkMenus() {
    // The default before UseMenuLook is called is the game's own look, as before the dark one came.
    CHECK(ParchmentMenus() && !DarkMenus() && Same(kMenuText, kGameMenus.text) && Same(kMenuPanel, kGameMenus.panel) && Same(kMenuBg, kGameMenus.bg));
    UseMenuLook(MenuLook::Dark);
    CHECK(DarkMenus() && Same(kMenuText, kDarkMenus.text));
    for (Rgba face : {kMenuPanel, kMenuCard, kMenuCardHover})
        for (Rgba under : {Rgba{0, 0, 0, 1}, kSceneGrey})
            for (Rgba c : {kMenuText, kMenuDim, kMenuAccentInk, kMenuOk, kMenuWarn}) CHECK(Contrast(c, Over(face, under)) >= 4.5);
    for (Rgba face : {kMenuPanel, kMenuCard, kMenuCardHover}) CHECK(face.a >= 0.9f);
    CHECK(kMenuBg.a >= 0.99f && Contrast(kMenuLight, kMenuBg) >= 4.5);
    CHECK(Contrast(kMenuLight, kMenuAccent) >= 4.5);
    CHECK(Same(OnParchment(TeamInk(1)), TeamInk(1)));
    UseMenuLook(MenuLook::Game);
}

// The classic look: every menu text colour reads on every dark face, START's label on the accent; light ink stays.
static void TestClassicMenus() {
    UseMenuLook(MenuLook::Classic);
    CHECK(ClassicMenus());
    for (Rgba face : {kMenuBg, kMenuPanel, kMenuCard, kMenuCardHover, Over(kMenuBg, {0, 0, 0, 1})})
        for (Rgba c : {kMenuText, kMenuDim, kMenuAccentInk, kMenuOk, kMenuWarn}) CHECK(Contrast(c, face) >= 4.5);
    for (int t = -1; t < 4; ++t) CHECK(Contrast(OnParchment(TeamInk(t)), kMenuCard) >= 4.5);
    CHECK(Contrast(kMenuLight, kMenuAccent) >= 4.5);
    CHECK(Luminance(kMenuCard) > Luminance(kMenuPanel) && Luminance(kMenuCardHover) > Luminance(kMenuCard));
    CHECK(Same(OnParchment(TeamInk(1)), TeamInk(1)));   // classic faces are dark: light inks stay
    UseMenuLook(MenuLook::Game);
    CHECK(!ClassicMenus() && Same(kMenuText, kGameMenus.text) && Same(kMenuPanel, kGameMenus.panel));
    UseMenuLook(MenuLook::Game);
}

// Headings: title case in the game look, capitals in the classic one. HS Enhancer keeps its name in both.
static void TestMenuTitle() {
    CHECK(MenuTitle("HOST A LOBBY") == "Host A Lobby" && MenuTitle("CO-OP") == "Co-Op" && MenuTitle("PLAYERS 3/8") == "Players 3/8");
    CHECK(MenuTitle("PLAYER'S CLASS") == "Player's Class");            // no new word after the apostrophe
    CHECK(MenuTitle("Help Improve the Mod?") == "Help Improve the Mod?" && MenuTitle("report a bug") == "report a bug");
    CHECK(MenuTitle("ALLOW HS ENHANCER") == "Allow HS Enhancer" && MenuTitle("HS ENHANCER") == "HS Enhancer");
    CHECK(MenuTitle("BLOCK HS") == "Block HS" && MenuTitle("HS") == "HS" && MenuTitle("(HS)") == "(HS)");
    CHECK(MenuTitle("HSM MOD") == "Hsm Mod" && MenuTitle("DASHS") == "Dashs" && MenuTitle("H S") == "H S");   // only the whole word
    CHECK(MenuTitle("").empty() && MenuTitle("UNRESTRICTED CLASSES") == "Unrestricted Classes");
    UseMenuLook(MenuLook::Classic);
    CHECK(MenuTitle("Allow HS Enhancer") == "ALLOW HS ENHANCER" && MenuTitle("Help Improve the Mod?") == "HELP IMPROVE THE MOD?");
    CHECK(MenuTitle("PLAYERS 3/8") == "PLAYERS 3/8");
    UseMenuLook(MenuLook::Game);
}

static void TestOnParchment() {
    UseMenuLook(MenuLook::Game);
    for (int t = 0; t < 4; ++t) {                  // each team keeps its hue
        const Rgba a = TeamInk(t), b = OnParchment(a);
        CHECK(Luminance(b) <= 0.0601 && Near(b.a, a.a));
        CHECK(Near(a.r * b.g, a.g * b.r, 1e-5) && Near(a.g * b.b, a.b * b.g, 1e-5));
    }
    CHECK(Same(OnParchment(kMenuText), kMenuText));   // ink stays ink
    CHECK(Same(OnParchment({0, 0, 0, 0.5f}), {0, 0, 0, 0.5f}));
    UseMenuLook(MenuLook::Game);
}

static void TestHudOpacity() {
    CHECK(SaneHudOpacity(1.0) == 1.f && SaneHudOpacity(0.6) == 0.6f && SaneHudOpacity(kHudOpacityMin) == kHudOpacityMin);
    CHECK(SaneHudOpacity(0.0) == kHudOpacityMin && SaneHudOpacity(-2) == kHudOpacityMin);   // never invisible
    CHECK(SaneHudOpacity(3.0) == 1.f && SaneHudOpacity(std::nan("")) == 1.f);
    UseHudOpacity(0.5); CHECK(HudOpacity() == 0.5f);
    UseHudOpacity(1.0); CHECK(HudOpacity() == 1.f);
}

int main() {
    TestHudOpacity();
    TestPlanValues();
    TestContrastMath();
    TestTextIsReadable();
    TestTeamInk();
    TestDarkMenus();
    TestMenuPalette();
    TestOnParchment();
    TestClassicMenus();
    TestMenuTitle();
    if (g_fail) { std::printf("%d hud style checks failed\n", g_fail); return 1; }
    std::printf("all hud style tests passed\n");
    return 0;
}

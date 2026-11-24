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
    CHECK(Same(kIron, {0.4851f, 0.5029f, 0.5395f, 1}));   // raised from 0.45 0.47 0.50 (PR8): 4.3:1 over a white scene was not enough
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

// The type scale: eight sizes, in this order, and the typeface goes by role (a section and up is the heading face).
static void TestTypeScale() {
    using namespace hsm::style::type;
    CHECK(kHint == 14 && kBody == 15 && kRow == 17 && kEmphasis == 20 && kSection == 23 && kCard == 28 && kModal == 36 && kPage == 44);
    CHECK(sizeof(kScale) / sizeof(kScale[0]) == 8);
    const float want[8] = {14, 15, 17, 20, 23, 28, 36, 44};
    for (int i = 0; i < 8; ++i) CHECK(kScale[i] == want[i]);
    for (int i = 1; i < 8; ++i) CHECK(kScale[i] > kScale[i - 1]);                   // strictly rising
    for (float dropped : {13.f, 16.f, 19.f, 21.f, 22.f, 25.f, 30.f, 50.f})
        for (float s : kScale) CHECK(s != dropped);                                  // the sizes the scale retired
    CHECK(kBannerSub == 26 && kScore == 34 && kDeadTitle == 38 && kBanner == 80);   // the HUD's display sizes stay
    for (float s : {kBannerSub, kScore, kDeadTitle, kBanner}) for (float t : kScale) CHECK(s != t);
    CHECK(IsHeading(kSection) && IsHeading(kCard) && IsHeading(kModal) && IsHeading(kPage));
    CHECK(IsHeading(kBannerSub) && IsHeading(kScore) && IsHeading(kDeadTitle) && IsHeading(kBanner));
    CHECK(!IsHeading(kHint) && !IsHeading(kBody) && !IsHeading(kRow) && !IsHeading(kEmphasis));
    CHECK(IsHeading(kSection) && !IsHeading(kSection - 1));                         // the boundary is the section size
}

// A grey on black that reads exactly `ratio`:1: (g + 0.05) / 0.05.
static Rgba GreyFor(double ratio) { const float g = float(ratio * 0.05 - 0.05); return {g, g, g, 1}; }

// The thresholds: 4.5:1 for text and 3:1 for disabled text, each way round (a hair under fails, a hair over passes).
static void TestContrastThresholds() {
    const Rgba black{0, 0, 0, 1};
    CHECK(kMinTextContrast == 4.5 && kMinDisabledContrast == 3.0);
    CHECK(!ReadableText(Contrast(GreyFor(4.45), black)) && ReadableText(Contrast(GreyFor(4.55), black)));
    CHECK(!ReadableDisabled(Contrast(GreyFor(2.95), black)) && ReadableDisabled(Contrast(GreyFor(3.05), black)));
    CHECK(!ReadableText(Contrast(GreyFor(3.2), black)) && ReadableDisabled(Contrast(GreyFor(3.2), black)));   // 3:1 is the lower bar
    CHECK(!ReadableDisabled(Contrast(GreyFor(1), black)));
}

// The display-space blend: a plain mix of the sRGB values, which is not the linear mix.
static void TestDisplayBlend() {
    CHECK(Near(FromDisplay(ToDisplay(0.2)), 0.2) && Near(ToDisplay(0), 0) && Near(ToDisplay(1), 1));
    CHECK(Near(ToDisplay(0.5), 0.7354, 1e-3) && Near(FromDisplay(0.5), 0.2140, 1e-3));
    CHECK(Same(OverDisplay({0.2f, 0.4f, 0.6f, 1}, kSceneWhite), {0.2f, 0.4f, 0.6f, 1}));
    CHECK(Same(OverDisplay({0.2f, 0.4f, 0.6f, 0}, {0.5f, 0.5f, 0.5f, 1}), {0.5f, 0.5f, 0.5f, 1}));
    CHECK(Same(OverDisplay({1, 1, 1, 0}, {1, 1, 1, 0}), {0, 0, 0, 0}));
    const Rgba half = OverDisplay({0, 0, 0, 0.5f}, kSceneWhite);                       // half black over white: sRGB 0.5
    CHECK(Near(half.r, FromDisplay(0.5), 1e-4) && half.r < Over({0, 0, 0, 0.5f}, kSceneWhite).r);
}

// The panel over a bright scene: every ink the HUD prints on it (the plan's worst case is white), with the two inks
// that failed there (iron 4.3, blood ink 4.1) raised to the mock's sRGB #b9bcc2 and #fba597.
static void TestHudInksOverWhite() {
    CHECK(Same(PanelOverWhite(), OverDisplay(kSmoke, kSceneWhite)) && PanelOverWhite().a == 1.f);
    for (Rgba c : {kBone, kIron, kOk, kWarn, kBloodInk, kInfectedInk}) CHECK(ReadableText(TextContrastOverWhite(c)));
    for (int t = -1; t <= 4; ++t) CHECK(ReadableText(TextContrastOverWhite(TeamInk(t))));
    CHECK(TextContrastOverWhite(Rgba{0.45f, 0.47f, 0.50f, 1}) < 4.5);                   // kIron before this change
    CHECK(TextContrastOverWhite(Rgba{0.95f, 0.32f, 0.26f, 1}) < 4.5);                   // kBloodInk before this change
    CHECK(!ReadableText(TextContrastOverWhite(kBlood)));                                 // the accent is still not text
    CHECK(Near(kIron.r, 0.4851, 5e-4) && Near(kIron.g, 0.5029, 5e-4) && Near(kIron.b, 0.5395, 5e-4));             // #b9bcc2
    CHECK(Near(kBloodInk.r, 0.9647, 5e-4) && Near(kBloodInk.g, 0.3763, 5e-4) && Near(kBloodInk.b, 0.3095, 5e-4));   // #fba597
    CHECK(TextContrastOverWhite(kBone) < TextContrast(kBone));                           // white is worse than the reference grey
    const Rgba halfBone{kBone.r, kBone.g, kBone.b, 0.5f};                                 // a faded ink counts as drawn
    CHECK(TextContrastOverWhite(halfBone) < TextContrastOverWhite(kBone));
    CHECK(Near(TextContrastOverWhite(halfBone), Contrast(OverDisplay(halfBone, PanelOverWhite()), PanelOverWhite())));
}

// Text on a picked card and a picked plate, in every look, and a disabled control's label.
static void TestPickedAndDisabled() {
    for (MenuLook look : {MenuLook::Classic, MenuLook::Game, MenuLook::Dark}) {
        const MenuPalette& m = PaletteOf(look);
        // A picked card: its ink on its face (and on the panel behind it when the face is see-through).
        CHECK(ReadableText(Contrast(m.onPicked, m.picked)));
        CHECK(ReadableText(Contrast(m.onPicked, OverDisplay(m.picked, OverDisplay(m.panel, kSceneGrey)))));
        // A picked plate (your own kill-feed line) over the reference scene; the opaque ones over white as well.
        CHECK(ReadableText(Contrast(m.onPicked, OverDisplay(m.platePicked, kSceneGrey))));
        if (look != MenuLook::Classic) CHECK(ReadableText(Contrast(m.onPicked, OverDisplay(m.platePicked, kSceneWhite))));
        // A disabled control: the label, its card and the panel behind it all at the disabled opacity.
        Rgba bg = m.bg; bg.a = 1;
        const Rgba behind = OverDisplay(m.panel, bg);
        CHECK(ReadableDisabled(DisabledContrast(m.text, m.card, behind)));
        // A dimmed card that is also the picked one (a member sees the host's pick dimmed): dark ink on the faded amber reads
        // about 1.8:1 in the dark look. Known and left: the mock does not cover it either (docs/UI_STYLE.md).
        if (look != MenuLook::Dark) CHECK(ReadableDisabled(DisabledContrast(m.onPicked, m.picked, behind)));
        CHECK(DisabledContrast(m.text, m.card, behind) < Contrast(m.text, m.card));       // it does look fainter
    }
    CHECK(Same(kClassicMenus.onPicked, kClassicMenus.text));
    CHECK(ReadableText(Contrast(kClassicMenus.onPicked, kClassicMenus.accent)));         // the classic fallback paint of a picked card
    // Before this change: bone on the shaded amber read about 2.5:1 in the dark look.
    CHECK(!ReadableText(Contrast(kDarkMenus.text, kDarkMenus.picked)));
    CHECK(!ReadableText(Contrast(kDarkMenus.text, kDarkMenus.platePicked)));
    CHECK(Same(kDarkMenus.picked, Keep(kAmber, kDarkKeepPicked)) && Same(kDarkMenus.platePicked, Keep(kAmber, kDarkKeepPlate)));
    CHECK(Same(kGameMenus.picked, kAmber) && Same(kGameMenus.platePicked, kAmber) && Same(kGameMenus.pickedFrame, kAmber));
    CHECK(Same(kClassicMenus.pickedFrame, kClassicMenus.accent) && Same(kClassicMenus.picked, kClassicMenus.card));
    CHECK(Same(kClassicMenus.platePicked, kPlateMineClassic) && Same(kPlateMineClassic, {0.22f, 0.025f, 0.02f, 0.80f}));
    CHECK(Same(kAmber, {0.85f, 0.45f, 0.03f, 1}) && kDarkKeepPicked == 0.55f && kDarkKeepPlate == 0.50f);   // what the skin shades by
    // One opacity for every disabled control: the lowest of the old four that keeps the label at 3:1 in all three looks.
    CHECK(kDisabledAlpha == 0.55f);
    CHECK(kDisabledAlpha < 0.7f);                                                         // still plainly faded
    Rgba gameBg = kGameMenus.bg; gameBg.a = 1;
    const Rgba gameBehind = OverDisplay(kGameMenus.panel, gameBg);
    for (float old : {0.3f, 0.4f, 0.45f}) CHECK(!ReadableDisabled(DisabledContrast(kGameMenus.text, kGameMenus.card, gameBehind, old)));
    // The numbers themselves (the label, its card and the panel all faded): the game look is the tight one.
    Rgba classicBg = kClassicMenus.bg, darkBg = kDarkMenus.bg; classicBg.a = darkBg.a = 1;
    CHECK(Near(DisabledContrast(kGameMenus.text, kGameMenus.card, gameBehind), 3.50, 0.03));
    CHECK(Near(DisabledContrast(kClassicMenus.text, kClassicMenus.card, OverDisplay(kClassicMenus.panel, classicBg)), 4.92, 0.03));
    CHECK(Near(DisabledContrast(kDarkMenus.text, kDarkMenus.card, OverDisplay(kDarkMenus.panel, darkBg)), 4.77, 0.03));
}

// The new palette roles are set by UseMenuLook, one set per look.
static void TestPickedRoles() {
    for (MenuLook look : {MenuLook::Classic, MenuLook::Game, MenuLook::Dark, MenuLook::Classic}) {
        UseMenuLook(look);
        const MenuPalette& m = PaletteOf(look);
        CHECK(Same(kMenuPicked, m.picked) && Same(kMenuPickedFrame, m.pickedFrame) && Same(kMenuOnPicked, m.onPicked));
        CHECK(Same(kMenuPlatePicked, m.platePicked) && Same(kMenuField, m.field));
    }
    CHECK(kGameMenus.field.a == 1.f && kClassicMenus.field.a == 1.f && kDarkMenus.field.a < 1.f);
    CHECK(Same(kSmokeDense, {kSmoke.r, kSmoke.g, kSmoke.b, 0.92f}) && kOutlineInk.a == 0.9f && kOutlineInk.r == 0.f);
    CHECK(Same(kCardTint, {0.86f, 0.84f, 0.80f, 1}) && Same(kCardTintHover, {1, 0.97f, 0.88f, 1}) && Same(kStartTint, {0.85f, 0.85f, 0.85f, 1}));
    UseMenuLook(MenuLook::Game);
}

// Headings: title case in the game look, capitals in the classic one. HS Enhancer keeps its name in both.
static void TestMenuTitle() {
    UseMenuLook(MenuLook::Game);
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

// Before UseMenuLook is called the menus are in the classic look, the default (the look before 0.8.0-pre43).
static void TestDefaultLook() {
    CHECK(ClassicMenus() && !ParchmentMenus() && !DarkMenus() && CurrentMenuLook() == MenuLook::Classic);
    CHECK(Same(kMenuText, kClassicMenus.text) && Same(kMenuPanel, kClassicMenus.panel) && Same(kMenuBg, kClassicMenus.bg));
    CHECK(Same(kMenuCard, kClassicMenus.card) && Same(kMenuAccent, kClassicMenus.accent) && Same(kMenuLight, kClassicMenus.light));
}

// ui_style at start: classic is the default; a "game" the old template wrote moves to classic once; dark is kept.
static void TestStartLook() {
    const StartLook missing = ReadStartLook("classic", false);   // no ui_style line reads as classic
    CHECK(missing.look == MenuLook::Classic && missing.known && !missing.moveToClassic);
    const StartLook oldTemplate = ReadStartLook("game", false);   // pre43 to pre49 wrote this for everyone
    CHECK(oldTemplate.look == MenuLook::Classic && oldTemplate.known && oldTemplate.moveToClassic);
    const StartLook picked = ReadStartLook("game", true);        // picked after the move: kept
    CHECK(picked.look == MenuLook::Game && picked.known && !picked.moveToClassic);
    for (bool moved : {false, true}) {
        const StartLook dark = ReadStartLook("dark", moved);     // only a deliberate pick writes dark
        CHECK(dark.look == MenuLook::Dark && dark.known && !dark.moveToClassic);
        const StartLook classic = ReadStartLook("classic", moved);
        CHECK(classic.look == MenuLook::Classic && classic.known && !classic.moveToClassic);
        for (const char* bad : {"", "nonsense", "Game", "GAME", " game"}) {
            const StartLook b = ReadStartLook(bad, moved);
            CHECK(b.look == MenuLook::Classic && !b.known && !b.moveToClassic);
        }
    }
}

// SETTINGS > MENU LOOK starts at classic and goes through the other two back to it.
static void TestNextMenuLook() {
    CHECK(NextMenuLook(MenuLook::Classic) == MenuLook::Game);
    CHECK(NextMenuLook(MenuLook::Game) == MenuLook::Dark);
    CHECK(NextMenuLook(MenuLook::Dark) == MenuLook::Classic);
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
    TestDefaultLook();   // first: before any test sets a look
    TestStartLook();
    TestNextMenuLook();
    TestHudOpacity();
    TestPlanValues();
    TestContrastMath();
    TestTextIsReadable();
    TestTypeScale();
    TestContrastThresholds();
    TestDisplayBlend();
    TestHudInksOverWhite();
    TestPickedAndDisabled();
    TestPickedRoles();
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

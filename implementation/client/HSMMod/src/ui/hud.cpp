#include "ui/hud.h"
#include "ui/umg.h"
#include "ui/skin.h"
#include "ui/hud_host.h"
#include "log.h"
#include "hsm_protocol.h"
#include <windows.h>
#include <array>
#include <cmath>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr int32_t kZOrder = 50;
constexpr size_t kStripBlocks = 4, kBoardRows = kBoardRowBudget;   // twenty fighters and four team bands; or a Tournament's fighters and the bracket lines that fit (FitBracket)
constexpr Rgba kOutline = style::kOutlineInk, kClear{0, 0, 0, 0};
constexpr float kBoardNumberWidth = 70;   // KILLS and DEATHS are words now (the columns were 50 for K and D)
// #3056: at the results the banner is a headline, not a wall across the middle of the screen, and the board sits in a corner.
constexpr float kResultsBannerScale = 0.6f, kResultsSubScale = 0.85f;
constexpr Anchors kBoardMiddle{{0.5, 0.17}, {0.5, 0.17}}, kBoardCorner{{0.985, 0.97}, {0.985, 0.97}};

struct Block { Box column; Panel panel; Text label, score; };
struct TagLabel { Text text; Slot slot{nullptr}; };
struct BoardLine { Panel frame, panel; Icon swatch; Text name, badge, kills, deaths, score, ping, status; };

HudHost host{"hud"};   // the UI_HUD we sit on; outside State, so making the widgets again does not forget it

struct State {
    bool built = false;
    Kit kit;
    Box top; std::array<Block, kStripBlocks> blocks; Fixed barBox; Bar bar; Panel pill; Text pillText, clock, info;   // top centre (info: Tournament's pairing, Pit Bridge Duels' streak, the Crypts line)
    Box infoRow; Panel infoBack; Fixed infoGap; Box chipBox; Panel chipEdge; Text chipLabel, chipText;          // under the pill: the info line on smoke, and the Crypts key chip
    Box bannerBox; Text banner, bannerSub;                                                                      // centre
    Panel hold; Text holdText; Fixed holdBarBox; Bar holdBar;                                                   // the G hold, mid screen
    Panel dead; Text deadTitle, deadDetail, deadHint; Fixed deadBarBox; Bar deadBar;                            // lower third
    std::array<TagLabel, kMaxWillies> tags;                                                                              // name tags over heads
    UObject* statics = nullptr; UObject* layout = nullptr;                                                      // CDOs, resolved once
    Format tagFormat = Format::Duel;
    bool tagsOff = false;                                                                                       // a name was not found: hidden for good
    Blur board; Slot boardSlot{nullptr}; bool boardInCorner = false; Text boardTitle, boardFooter; Box boardLegend; Text legendDown, legendDead; BoardLine boardHeader; std::array<BoardLine, kBoardRows> boardLines;
} g;

// Text in the game's font with a dark outline.
Text MakeText(float size, int32_t outline, Rgba color = style::kBone, Justify justify = Justify::Left) {
    Text t = g.kit.MakeText();
    if (!t) return t;
    t.Font(size, outline, kOutline);
    t.Color(color);
    t.Align(justify);
    return t;
}

// Text written on the game's parchment: ink in its typeface, no outline (the classic look: light text with the dark
// outline, on smoke). Its colour goes through Ink().
Text MakeInk(float size, Rgba color = style::kMenuText, Justify justify = Justify::Left, skin::Face face = skin::Face::Body) {
    Text t = g.kit.MakeText();
    if (!t) return t;
    if (!style::ParchmentMenus()) t.Font(size, 1, kOutline); else t.Font(size);
    skin::Font(t, face);
    t.Color(style::OnParchment(color));
    t.Align(justify);
    return t;
}

// A scene colour (made for the dark HUD) as ink on parchment.
void Ink(Text& t, Rgba c) { t.Color(style::OnParchment(c)); }

Panel MakePanel(Rgba color, Margin padding) {
    Panel p = g.kit.MakePanel();
    p.Color(color);
    p.Padding(padding);
    return p;
}

// A panel of the game's art (its plain colour when the art is not loaded), `room` inside the art's frame or edges.
Panel MakeArt(skin::Art art, Margin room, Rgba tint = {1, 1, 1, 1}) {
    Panel p = g.kit.MakePanel();
    skin::Paint(p, art, tint);
    skin::Pad(p, art, room);
    return p;
}

Bar MakeBar(Fixed& box, Box& into, float width, float height, Margin padding) {
    box = g.kit.MakeFixed();
    box.Width(width); box.Height(height);
    into.Add(box).Align(HAlign::Center, VAlign::Top).Padding(padding);
    Bar bar = g.kit.MakeBar();
    bar.FillColor(style::kBlood);
    box.Content(bar);
    return bar;
}

// A table cell of a fixed width; its text fills it so the justification applies.
void Cell(Box& row, const Text& text, float width) {
    Fixed f = g.kit.MakeFixed();
    f.Width(width);
    f.Content(text).Align(HAlign::Fill, VAlign::Center);
    row.Add(f).Align(HAlign::Left, VAlign::Center);
}

BoardLine MakeBoardLine(Box& column, float size, Rgba ink) {
    BoardLine l;
    l.frame = MakePanel(kClear, {2, 2, 2, 2});                  // the local row's blood frame
    column.Add(l.frame).Align(HAlign::Fill, VAlign::Top);
    l.panel = MakePanel(kClear, {8, 1, 8, 1});                  // a team band's colour
    l.frame.Content(l.panel).Align(HAlign::Fill, VAlign::Fill);
    Box row = g.kit.MakeRow();
    l.panel.Content(row).Align(HAlign::Fill, VAlign::Center);
    l.swatch = g.kit.MakeIcon();
    l.swatch.Size({10, 22});
    l.swatch.Color(kClear);
    row.Add(l.swatch).Align(HAlign::Left, VAlign::Center).Padding({0, 0, 10, 0});
    l.name = MakeInk(size, ink); Cell(row, l.name, 240);
    l.badge = MakeInk(style::type::kHint, style::kMenuWarn); Cell(row, l.badge, 60);
    l.kills = MakeInk(size, ink, Justify::Right); Cell(row, l.kills, kBoardNumberWidth);
    l.deaths = MakeInk(size, ink, Justify::Right); Cell(row, l.deaths, kBoardNumberWidth);
    l.score = MakeInk(size, ink, Justify::Right); Cell(row, l.score, 80);
    l.ping = MakeInk(size, style::kMenuDim, Justify::Right); Cell(row, l.ping, 90);
    l.status = MakeInk(style::type::kHint, style::kMenuAccentInk, Justify::Right); Cell(row, l.status, 70);
    return l;
}

void BuildTop(Canvas& root) {
    g.top = g.kit.MakeColumn();
    root.Add(g.top).Anchor({{0.5, 0.015}, {0.5, 0.015}}, {0.5, 0}).AutoSize().Z(kZOrder);
    Box strip = g.kit.MakeRow();
    g.top.Add(strip).Align(HAlign::Center, VAlign::Top);
    for (Block& b : g.blocks) {
        b.column = g.kit.MakeColumn();
        strip.Add(b.column).Padding({4, 0, 4, 0});
        b.panel = MakeArt(skin::Art::Plate, {16, 6, 16, 6});
        b.column.Add(b.panel).Align(HAlign::Fill, VAlign::Top);
        Box row = g.kit.MakeRow();
        b.panel.Content(row);
        b.label = MakeInk(style::type::kEmphasis);
        row.Add(b.label).Align(HAlign::Left, VAlign::Center).Padding({0, 0, 12, 0});
        b.score = MakeInk(style::type::kScore, style::kMenuText, Justify::Left, skin::Face::Heading);
        row.Add(b.score).Align(HAlign::Left, VAlign::Center);
    }
    g.bar = MakeBar(g.barBox, g.top, 320, 5, {0, 6, 0, 0});
    g.pill = MakeArt(skin::Art::Plate, {16, 5, 16, 5});
    g.top.Add(g.pill).Align(HAlign::Center, VAlign::Top).Padding({0, 6, 0, 0});
    Box pillRow = g.kit.MakeRow();
    g.pill.Content(pillRow);
    g.pillText = MakeInk(style::type::kEmphasis);
    pillRow.Add(g.pillText).Align(HAlign::Left, VAlign::Center);
    g.clock = MakeInk(style::type::kEmphasis, style::kMenuDim);
    pillRow.Add(g.clock).Align(HAlign::Left, VAlign::Center).Padding({14, 0, 0, 0});
    // The info line and the Crypts key chip share a row under the pill. The info line is on smoke so it reads on a bright map.
    g.infoRow = g.kit.MakeRow();
    g.top.Add(g.infoRow).Align(HAlign::Center, VAlign::Top).Padding({0, 4, 0, 0});
    g.infoBack = MakePanel(style::kSmoke, {12, 3, 12, 3});
    g.infoRow.Add(g.infoBack).Align(HAlign::Left, VAlign::Fill);
    g.info = MakeText(style::type::kBody, 1, style::kIron, Justify::Center);
    g.infoBack.Content(g.info).Align(HAlign::Center, VAlign::Center);
    g.infoGap = g.kit.MakeFixed();
    g.infoGap.Width(8);
    g.infoRow.Add(g.infoGap).Align(HAlign::Left, VAlign::Fill);
    g.chipBox = g.kit.MakeRow();
    g.infoRow.Add(g.chipBox).Align(HAlign::Left, VAlign::Fill);
    Fixed edgeBox = g.kit.MakeFixed();
    edgeBox.Width(4);
    g.chipBox.Add(edgeBox).Align(HAlign::Left, VAlign::Fill);
    g.chipEdge = MakePanel(style::kWarn, {0, 0, 0, 0});
    edgeBox.Content(g.chipEdge).Align(HAlign::Fill, VAlign::Fill);
    Panel chipBack = MakePanel(style::kSmoke, {10, 3, 10, 3});
    g.chipBox.Add(chipBack).Align(HAlign::Left, VAlign::Fill);
    Box chipRow = g.kit.MakeRow();
    chipBack.Content(chipRow).Align(HAlign::Fill, VAlign::Center);
    g.chipLabel = MakeText(style::type::kHint, 1, style::kWarn);
    chipRow.Add(g.chipLabel).Align(HAlign::Left, VAlign::Center).Padding({0, 0, 8, 0});
    g.chipText = MakeText(style::type::kBody, 1, style::kBone);
    chipRow.Add(g.chipText).Align(HAlign::Left, VAlign::Center);
}

void BuildCentre(Canvas& root) {
    g.bannerBox = g.kit.MakeColumn();
    root.Add(g.bannerBox).Anchor({{0.5, 0.30}, {0.5, 0.30}}, {0.5, 0.5}).AutoSize().Z(kZOrder + 1);
    g.banner = MakeText(style::type::kBanner, 3, style::kBone, Justify::Center);
    skin::Font(g.banner, skin::Face::Heading);   // the game's own titles
    g.bannerBox.Add(g.banner).Align(HAlign::Center, VAlign::Top);
    g.bannerSub = MakeText(style::type::kBannerSub, 2, style::kBone, Justify::Center);
    g.bannerSub.Shadow({style::kShadowPx, style::kShadowPx}, style::kShadowInk);   // outline and a shadow, no panel: it reads on a bright map
    g.bannerBox.Add(g.bannerSub).Align(HAlign::Center, VAlign::Top);

    g.hold = MakeArt(skin::Art::Sheet, {20, 6, 20, 6});
    root.Add(g.hold).Anchor({{0.5, 0.56}, {0.5, 0.56}}, {0.5, 0.5}).AutoSize().Z(kZOrder + 1);
    Box holdCol = g.kit.MakeColumn();
    g.hold.Content(holdCol);
    g.holdText = MakeInk(style::type::kCard, style::kMenuAccentInk, Justify::Center, skin::Face::Heading);
    holdCol.Add(g.holdText).Align(HAlign::Center, VAlign::Top);
    g.holdBar = MakeBar(g.holdBarBox, holdCol, 280, 8, {0, 8, 0, 0});

    g.dead = MakeArt(skin::Art::Sheet, {26, 8, 26, 8});
    root.Add(g.dead).Anchor({{0.5, 0.70}, {0.5, 0.70}}, {0.5, 0.5}).AutoSize().Z(kZOrder);
    Box col = g.kit.MakeColumn();
    g.dead.Content(col);
    g.deadTitle = MakeInk(style::type::kDeadTitle, style::kMenuAccentInk, Justify::Center, skin::Face::Heading);
    col.Add(g.deadTitle).Align(HAlign::Center, VAlign::Top);
    g.deadDetail = MakeInk(style::type::kEmphasis, style::kMenuText, Justify::Center);
    col.Add(g.deadDetail).Align(HAlign::Center, VAlign::Top);
    g.deadBar = MakeBar(g.deadBarBox, col, 300, 6, {0, 8, 0, 0});
    g.deadHint = MakeInk(style::type::kBody, style::kMenuDim, Justify::Center);
    col.Add(g.deadHint).Align(HAlign::Center, VAlign::Top).Padding({0, 10, 0, 0});
}

void BuildBoard(Canvas& root) {
    g.board = g.kit.MakeBlur();
    // The game's scroll is opaque: nothing to blur, and a blur showed as a grey box round its rods. Classic: smoke over a blur.
    g.board.Strength(style::ParchmentMenus() ? 0.f : 8.f);
    g.boardSlot = root.Add(g.board);
    g.boardSlot.Anchor(kBoardMiddle, {0.5, 0}).AutoSize().Z(kZOrder + 2);
    Panel back = MakeArt(skin::Art::Scroll, {36, 14, 26, 34});   // the game's scroll, inside its rods
    g.board.Content(back);
    Box col = g.kit.MakeColumn();
    back.Content(col);
    g.boardTitle = MakeInk(style::type::kCard, style::kMenuAccentInk, Justify::Left, skin::Face::Heading);
    col.Add(g.boardTitle).Align(HAlign::Left, VAlign::Top).Padding({0, 0, 0, 10});
    g.boardHeader = MakeBoardLine(col, style::type::kBody, style::kMenuDim);   // its words come from the scene (Board::heads)
    for (BoardLine& l : g.boardLines) l = MakeBoardLine(col, style::type::kEmphasis, style::kMenuText);
    g.boardFooter = MakeInk(style::type::kEmphasis, style::kMenuWarn);
    col.Add(g.boardFooter).Align(HAlign::Left, VAlign::Top).Padding({0, 10, 0, 0});
    g.boardLegend = g.kit.MakeRow();   // what DOWN and DEAD mean, one line under the board
    col.Add(g.boardLegend).Align(HAlign::Left, VAlign::Top).Padding({0, 8, 0, 0});
    g.legendDown = MakeInk(style::type::kHint, style::kMenuDim);
    g.boardLegend.Add(g.legendDown).Align(HAlign::Left, VAlign::Center).Padding({0, 0, 24, 0});
    g.legendDead = MakeInk(style::type::kHint, style::kMenuDim);
    g.boardLegend.Add(g.legendDead).Align(HAlign::Left, VAlign::Center);
}

constexpr float kTagFont = style::type::kEmphasis;   // the nearest tag; the farthest is kHint

void BuildTags(Canvas& root) {
    for (TagLabel& t : g.tags) {
        t.text = MakeText(kTagFont, 2, style::kBone, Justify::Center);
        if (!t.text) return;
        t.text.Shadow({style::kShadowPx, style::kShadowPx}, style::kShadowInk);
        t.slot = root.Add(t.text);
        t.slot.Anchor({{0, 0}, {0, 0}}, {0.5, 1}).AutoSize().Z(kZOrder - 1);
        t.text.Show(false);
    }
}

void HideTags() { for (TagLabel& t : g.tags) if (t.text) t.text.Show(false); }

// Every call here fails soft: a name the game does not have logs once and the tags stay hidden; nothing else is touched.
void TagsFail(const char* what) {
    if (!g.tagsOff) HSM_LOG("hud: name tags off (%s not found)", what);
    g.tagsOff = true;
    HideTags();
}

void ApplyTags(const std::vector<NameTagSrc>& src) {
    if (g.tagsOff || !g.tags[0].text) return;
    if (src.empty()) { HideTags(); return; }
    if (!g.statics) if (UObject* c = FindObject("GameplayStatics", "Class")) g.statics = DefaultObject(reinterpret_cast<UClass*>(c));
    if (!g.layout) if (UObject* c = FindObject("WidgetLayoutLibrary", "Class")) g.layout = DefaultObject(reinterpret_cast<UClass*>(c));
    if (!g.statics || !g.layout) { TagsFail("GameplayStatics / WidgetLayoutLibrary"); return; }
    Call pc(g.statics, "GetPlayerController");
    if (!pc.valid()) { TagsFail("GetPlayerController"); return; }
    pc.set<UObject*>("WorldContextObject", host.Obj()).set<int32_t>("PlayerIndex", 0);
    UObject* ctrl = pc.invoke() ? pc.get<UObject*>("ReturnValue") : nullptr;
    if (!ctrl) { HideTags(); return; }                       // no controller this frame: not a failure
    Call sc(g.layout, "GetViewportScale"), vs(g.layout, "GetViewportSize");
    if (!sc.valid()) { TagsFail("GetViewportScale"); return; }
    if (!vs.valid()) { TagsFail("GetViewportSize"); return; }
    sc.set<UObject*>("WorldContextObject", host.Obj()); vs.set<UObject*>("WorldContextObject", host.Obj());
    if (!sc.invoke() || !vs.invoke()) { HideTags(); return; }
    const float scale = sc.get<float>("ReturnValue");
    const Vec2 view = vs.get<Vec2>("ReturnValue");
    Call cam(ctrl, "GetPlayerViewPoint");
    if (!cam.valid()) { TagsFail("GetPlayerViewPoint"); return; }
    FVector eye{}; if (cam.invoke()) eye = cam.get<FVector>("Location");
    for (size_t i = 0; i < g.tags.size(); ++i) {
        TagLabel& t = g.tags[i];
        if (!t.text) continue;
        if (i >= src.size()) { t.text.Show(false); continue; }
        const NameTagSrc& n = src[i];
        Call pj(ctrl, "ProjectWorldLocationToScreen");
        if (!pj.valid()) { TagsFail("ProjectWorldLocationToScreen"); return; }
        pj.set("WorldLocation", FVector{n.wx, n.wy, n.wz}).set<bool>("bPlayerViewportRelative", true);
        const bool ok = pj.invoke() && pj.get<bool>("ReturnValue");
        const Vec2 sp = ok ? pj.get<Vec2>("ScreenLocation") : Vec2{};
        const double dx = n.wx - eye.x, dy = n.wy - eye.y, dz = n.wz - eye.z;
        NameTagView v = NameTagLayout(ok, float(sp.x), float(sp.y), float(view.x), float(view.y), scale,
                                      float(std::sqrt(dx * dx + dy * dy + dz * dz)), n.alive, n.team, g.tagFormat, n.seen, n.it);
        t.text.Show(v.visible);
        if (!v.visible) continue;
        t.text.Set(n.name);
        t.text.Color(v.ink);
        t.text.Opacity(v.alpha * style::HudOpacity());
        t.text.Scale(v.fontSize / kTagFont);
        t.slot.Position({v.x, v.y});
    }
}

bool Build() {
    g.built = true;
    g.kit = Kit(host.Obj(), Vis::HitTestInvisible);
    Canvas root = g.kit.Root();
    if (!root) { HSM_LOG("hud: UI_HUD root is not a CanvasPanel; match HUD disabled"); return false; }
    ProbeFont(g.kit, host.Obj());
    BuildTop(root);
    BuildCentre(root);
    BuildBoard(root);
    BuildTags(root);
    if (const float a = style::HudOpacity(); a < 1.f)   // hud_opacity (the name tags take it in their own fade)
        for (Widget* x : std::initializer_list<Widget*>{&g.top, &g.bannerBox, &g.hold, &g.dead, &g.board}) x->Opacity(a);
    if (!g.pillText || !g.banner || !g.holdText || !g.boardTitle) { HSM_LOG("hud: could not make the match HUD widgets"); return false; }
    HSM_LOG("hud: match HUD added to UI_HUD");
    return true;
}

void Line(Text& t, const std::string& s, bool show) {
    if (show) t.Set(s);
    t.Show(show);
}

void Apply(Text& t, const SceneText& s, float scaleMul = 1.f) {
    t.Show(s.visible);
    if (!s.visible) return;
    t.Set(s.text);
    t.Color(s.color);
    t.Opacity(s.opacity);
    t.Scale(s.scale * scaleMul);
}

// The info line (on its smoke) and the Crypts key chip, side by side; each is hidden when it has nothing to say.
void ApplyInfo(const HudScene& s) {
    const bool info = s.info.visible && !s.info.text.empty(), chip = s.chip.visible;
    g.infoRow.Show(info || chip);
    g.infoBack.Show(info);
    if (info) Apply(g.info, s.info);
    g.infoGap.Show(info && chip);
    g.chipBox.Show(chip);
    if (chip) {
        g.chipLabel.Set(s.chip.label); g.chipLabel.Color(s.chip.labelColor);
        g.chipText.Set(s.chip.text); g.chipText.Color(s.chip.color);
    }
}

void ApplyTop(const HudScene& s) {
    g.top.Show(s.visible);
    for (size_t i = 0; i < g.blocks.size(); ++i) {
        Block& b = g.blocks[i];
        b.column.Show(i < s.strip.size());
        if (i >= s.strip.size()) continue;
        const ScoreBlock& sb = s.strip[i];
        b.label.Set(sb.label); Ink(b.label, sb.color);
        b.score.Set(std::to_string(sb.score)); Ink(b.score, sb.color);
    }
    g.barBox.Show(s.timeBar);
    if (s.timeBar) g.bar.Percent(s.timeFrac);
    g.pill.Show(s.pill.visible);
    if (s.pill.visible) { g.pillText.Set(s.pill.text); Ink(g.pillText, s.pill.color); }
    Line(g.clock, s.clock, s.timeBar && !s.clock.empty());
    ApplyInfo(s);
}

void ApplyHold(const HoldView& h) {
    g.hold.Show(h.visible);
    if (!h.visible) return;
    g.holdText.Set(h.text); Ink(g.holdText, h.color);
    g.holdBarBox.Show(h.bar);
    if (h.bar) g.holdBar.Percent(h.frac);
}

void ApplyDead(const DeadPanel& d) {
    g.dead.Show(d.visible);
    if (!d.visible) return;
    g.deadTitle.Set(d.title);
    Line(g.deadDetail, d.detail, !d.detail.empty());
    g.deadBarBox.Show(d.bar);
    if (d.bar) g.deadBar.Percent(d.barFrac);
    Line(g.deadHint, d.hint, !d.hint.empty());
}

void ApplyBoardLine(BoardLine& l, const BoardRow& r) {
    bool band = r.kind == BoardRow::Kind::Band || r.kind == BoardRow::Kind::Stage;
    bool bracket = r.kind == BoardRow::Kind::Stage || r.kind == BoardRow::Kind::Game;   // the bracket's lines: a name, no columns
    l.frame.Color(r.local ? (style::ClassicMenus() ? style::kBlood : style::kMenuPickedFrame) : kClear);
    l.panel.Color(band ? Rgba{r.swatch.r, r.swatch.g, r.swatch.b, 0.30f} : kClear);
    l.swatch.Color(band || bracket ? kClear : r.swatch);
    l.name.Set(r.tag.empty() ? r.name : r.name + "  " + r.tag); Ink(l.name, r.ink);
    l.badge.Set(r.host ? "HOST" : "");
    l.kills.Set(band || bracket ? "" : std::to_string(r.kills)); Ink(l.kills, r.ink);
    l.deaths.Set(band || bracket ? "" : std::to_string(r.deaths)); Ink(l.deaths, r.ink);
    l.score.Set(bracket ? "" : std::to_string(r.score)); Ink(l.score, r.ink);
    l.ping.Set(band || bracket || !r.pingMs ? "" : std::to_string(r.pingMs) + " ms");
    l.status.Set(r.dead ? "DEAD" : r.down ? "DOWN" : "");
}

void ApplyBoard(const Board& b) {
    g.board.Show(b.visible);
    if (!b.visible) return;
    if (b.results != g.boardInCorner && g.boardSlot) {   // the results board in the bottom right corner, the Tab board in the middle
        g.boardInCorner = b.results;
        g.boardSlot.Anchor(b.results ? kBoardCorner : kBoardMiddle, b.results ? Vec2{1, 1} : Vec2{0.5, 0});
    }
    g.boardTitle.Set(b.title);
    g.boardHeader.name.Set(b.heads.player); g.boardHeader.kills.Set(b.heads.kills); g.boardHeader.deaths.Set(b.heads.deaths);
    g.boardHeader.score.Set(b.heads.score); g.boardHeader.ping.Set(b.heads.ping);
    const size_t n = b.rows.size() + b.bracket.size();       // the fighters, then a Tournament's bracket (Tab)
    for (size_t i = 0; i < g.boardLines.size(); ++i) {
        g.boardLines[i].frame.Show(i < n);
        if (i < n) ApplyBoardLine(g.boardLines[i], i < b.rows.size() ? b.rows[i] : b.bracket[i - b.rows.size()]);
    }
    Line(g.boardFooter, b.footer, !b.footer.empty());
    const bool legend = !b.legendDown.empty() || !b.legendDead.empty();
    g.boardLegend.Show(legend);
    if (legend) { g.legendDown.Set(b.legendDown); g.legendDead.Set(b.legendDead); }
}
}  // namespace

bool HudTabHeld() {
    if (!(GetAsyncKeyState(VK_TAB) & 0x8000)) return false;   // the key first: asking whose window is in front costs more
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void HudTick(const HudScene& s, const std::vector<NameTagSrc>& tags, Format format) {
    // The widgets sit on the UI_HUD; when the game swaps it (or it is gone) they went with it, and are made again on the new one.
    if (host.Follow(s.visible, g.built) == HudHost::Step::Changed) g = State{};
    if (!host.Obj()) return;
    if (!g.built && !Build()) return;
    ApplyTop(s);
    Apply(g.banner, s.banner, s.board.results ? kResultsBannerScale : 1.f);
    Apply(g.bannerSub, s.bannerSub, s.board.results ? kResultsSubScale : 1.f);
    ApplyHold(s.hold);
    ApplyDead(s.dead);
    ApplyBoard(s.board);
    g.tagFormat = format;
    ApplyTags(s.visible ? tags : std::vector<NameTagSrc>{});
}

}  // namespace hsm::ui

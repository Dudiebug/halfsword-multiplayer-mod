#include "ui/hud.h"
#include "ui/umg.h"
#include "log.h"
#include <windows.h>
#include <array>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr int32_t kZOrder = 50;
constexpr size_t kStripBlocks = 4, kFeedSpans = 3, kBoardRows = 12;   // six fighters, three team bands, slack
constexpr Rgba kOutline{0, 0, 0, 0.9f}, kClear{0, 0, 0, 0};
constexpr Rgba kFeedMine{0.22f, 0.025f, 0.02f, 0.80f}, kFeedOther{0.02f, 0.017f, 0.014f, 0.55f};

struct Block { Box column; Panel panel; Text label, score; };
struct FeedLine { Panel panel; std::array<Text, kFeedSpans> spans; };
struct BoardLine { Panel frame, panel; Icon swatch; Text name, badge, kills, deaths, score, ping, status; };

struct State {
    UClass* hudClass = nullptr;
    UObject* hud = nullptr; int32_t hudIndex = -1;
    ULONGLONG nextScan = 0;
    bool built = false;
    Kit kit;
    Box top; std::array<Block, kStripBlocks> blocks; Fixed barBox; Bar bar; Panel pill; Text pillText, clock;   // top centre
    Box bannerBox; Text banner, bannerSub;                                                                      // centre
    Panel dead; Text deadTitle, deadDetail, deadHint; Fixed deadBarBox; Bar deadBar;                            // lower third
    Box feed; std::array<FeedLine, kFeedRows> feedLines;                                                       // top right
    Blur board; Text boardTitle, boardFooter; BoardLine boardHeader; std::array<BoardLine, kBoardRows> boardLines;
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

Panel MakePanel(Rgba color, Margin padding) {
    Panel p = g.kit.MakePanel();
    p.Color(color);
    p.Padding(padding);
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
    l.name = MakeText(size, 1, ink); Cell(row, l.name, 240);
    l.badge = MakeText(14, 1, style::kWarn); Cell(row, l.badge, 60);
    l.kills = MakeText(size, 1, ink, Justify::Right); Cell(row, l.kills, 50);
    l.deaths = MakeText(size, 1, ink, Justify::Right); Cell(row, l.deaths, 50);
    l.score = MakeText(size, 1, ink, Justify::Right); Cell(row, l.score, 80);
    l.ping = MakeText(size, 1, style::kIron, Justify::Right); Cell(row, l.ping, 90);
    l.status = MakeText(14, 1, style::kBloodInk, Justify::Right); Cell(row, l.status, 70);
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
        b.panel = MakePanel(style::kSmoke, {16, 2, 16, 2});
        b.column.Add(b.panel).Align(HAlign::Fill, VAlign::Top);
        Box row = g.kit.MakeRow();
        b.panel.Content(row);
        b.label = MakeText(20, 1);
        row.Add(b.label).Align(HAlign::Left, VAlign::Center).Padding({0, 0, 12, 0});
        b.score = MakeText(32, 2);
        row.Add(b.score).Align(HAlign::Left, VAlign::Center);
    }
    g.bar = MakeBar(g.barBox, g.top, 320, 5, {0, 6, 0, 0});
    g.pill = MakePanel(style::kSmoke, {14, 2, 14, 2});
    g.top.Add(g.pill).Align(HAlign::Center, VAlign::Top).Padding({0, 6, 0, 0});
    Box pillRow = g.kit.MakeRow();
    g.pill.Content(pillRow);
    g.pillText = MakeText(18, 1);
    pillRow.Add(g.pillText).Align(HAlign::Left, VAlign::Center);
    g.clock = MakeText(18, 1, style::kIron);
    pillRow.Add(g.clock).Align(HAlign::Left, VAlign::Center).Padding({14, 0, 0, 0});
}

void BuildCentre(Canvas& root) {
    g.bannerBox = g.kit.MakeColumn();
    root.Add(g.bannerBox).Anchor({{0.5, 0.30}, {0.5, 0.30}}, {0.5, 0.5}).AutoSize().Z(kZOrder + 1);
    g.banner = MakeText(76, 3, style::kBone, Justify::Center);
    g.bannerBox.Add(g.banner).Align(HAlign::Center, VAlign::Top);
    g.bannerSub = MakeText(26, 2, style::kBone, Justify::Center);
    g.bannerBox.Add(g.bannerSub).Align(HAlign::Center, VAlign::Top);

    g.dead = MakePanel(style::kSmoke, {28, 12, 28, 14});
    root.Add(g.dead).Anchor({{0.5, 0.70}, {0.5, 0.70}}, {0.5, 0.5}).AutoSize().Z(kZOrder);
    Box col = g.kit.MakeColumn();
    g.dead.Content(col);
    g.deadTitle = MakeText(34, 2, style::kBone, Justify::Center);
    col.Add(g.deadTitle).Align(HAlign::Center, VAlign::Top);
    g.deadDetail = MakeText(22, 1, style::kBone, Justify::Center);
    col.Add(g.deadDetail).Align(HAlign::Center, VAlign::Top);
    g.deadBar = MakeBar(g.deadBarBox, col, 300, 6, {0, 8, 0, 0});
    g.deadHint = MakeText(16, 1, style::kIron, Justify::Center);
    col.Add(g.deadHint).Align(HAlign::Center, VAlign::Top).Padding({0, 10, 0, 0});
}

void BuildFeed(Canvas& root) {
    g.feed = g.kit.MakeColumn();
    root.Add(g.feed).Anchor({{0.985, 0.10}, {0.985, 0.10}}, {1, 0}).AutoSize().Z(kZOrder);
    for (FeedLine& f : g.feedLines) {
        f.panel = MakePanel(kFeedOther, {10, 1, 10, 1});
        g.feed.Add(f.panel).Align(HAlign::Right, VAlign::Top).Padding({0, 0, 0, 3});
        Box row = g.kit.MakeRow();
        f.panel.Content(row);
        for (Text& t : f.spans) { t = MakeText(20, 1); row.Add(t).Align(HAlign::Left, VAlign::Center); }
    }
}

void BuildBoard(Canvas& root) {
    g.board = g.kit.MakeBlur();
    g.board.Strength(8);
    root.Add(g.board).Anchor({{0.5, 0.17}, {0.5, 0.17}}, {0.5, 0}).AutoSize().Z(kZOrder + 2);
    Panel back = MakePanel(style::kSmoke, {22, 14, 22, 16});
    g.board.Content(back);
    Box col = g.kit.MakeColumn();
    back.Content(col);
    g.boardTitle = MakeText(22, 1);
    col.Add(g.boardTitle).Align(HAlign::Left, VAlign::Top).Padding({0, 0, 0, 10});
    g.boardHeader = MakeBoardLine(col, 15, style::kIron);
    g.boardHeader.name.Set("PLAYER"); g.boardHeader.kills.Set("K"); g.boardHeader.deaths.Set("D");
    g.boardHeader.score.Set("SCORE"); g.boardHeader.ping.Set("PING");
    for (BoardLine& l : g.boardLines) l = MakeBoardLine(col, 20, style::kBone);
    g.boardFooter = MakeText(20, 1, style::kWarn);
    col.Add(g.boardFooter).Align(HAlign::Left, VAlign::Top).Padding({0, 10, 0, 0});
}

bool Build() {
    g.built = true;
    g.kit = Kit(g.hud, Vis::HitTestInvisible);
    Canvas root = g.kit.Root();
    if (!root) { HSM_LOG("hud: UI_HUD root is not a CanvasPanel; match HUD disabled"); return false; }
    if (UObject** src = Field<UObject*>(g.hud, "TextHurt"); src && *src) g.kit.FontFrom(*src, "UI_HUD.TextHurt");
    BuildTop(root);
    BuildCentre(root);
    BuildFeed(root);
    BuildBoard(root);
    if (!g.pillText || !g.banner || !g.boardTitle) { HSM_LOG("hud: could not make the match HUD widgets"); return false; }
    HSM_LOG("hud: match HUD added to UI_HUD");
    return true;
}

void Line(Text& t, const std::string& s, bool show) {
    if (show) t.Set(s);
    t.Show(show);
}

void Apply(Text& t, const SceneText& s) {
    t.Show(s.visible);
    if (!s.visible) return;
    t.Set(s.text);
    t.Color(s.color);
    t.Opacity(s.opacity);
    t.Scale(s.scale);
}

void ApplyTop(const HudScene& s) {
    g.top.Show(s.visible);
    for (size_t i = 0; i < g.blocks.size(); ++i) {
        Block& b = g.blocks[i];
        b.column.Show(i < s.strip.size());
        if (i >= s.strip.size()) continue;
        const ScoreBlock& sb = s.strip[i];
        b.label.Set(sb.label); b.label.Color(sb.color);
        b.score.Set(std::to_string(sb.score)); b.score.Color(sb.color);
    }
    g.barBox.Show(s.timeBar);
    if (s.timeBar) g.bar.Percent(s.timeFrac);
    g.pill.Show(s.pill.visible);
    if (s.pill.visible) { g.pillText.Set(s.pill.text); g.pillText.Color(s.pill.color); }
    Line(g.clock, s.clock, s.timeBar && !s.clock.empty());
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

void ApplyFeed(const std::vector<FeedView>& feed) {
    for (size_t i = 0; i < g.feedLines.size(); ++i) {
        FeedLine& f = g.feedLines[i];
        f.panel.Show(i < feed.size());
        if (i >= feed.size()) continue;
        const FeedView& v = feed[i];
        f.panel.Color(v.row.mine ? kFeedMine : kFeedOther);
        f.panel.Opacity(v.opacity);
        for (size_t k = 0; k < f.spans.size(); ++k) {
            bool has = k < v.row.spans.size();
            f.spans[k].Show(has);
            if (has) { f.spans[k].Set(v.row.spans[k].text); f.spans[k].Color(v.row.spans[k].color); }
        }
    }
}

void ApplyBoardLine(BoardLine& l, const BoardRow& r) {
    bool band = r.kind == BoardRow::Kind::Band;
    l.frame.Color(r.local ? style::kBlood : kClear);
    l.panel.Color(band ? Rgba{r.swatch.r, r.swatch.g, r.swatch.b, 0.45f} : kClear);
    l.swatch.Color(band ? kClear : r.swatch);
    l.name.Set(r.name); l.name.Color(r.ink);
    l.badge.Set(r.host ? "HOST" : "");
    l.kills.Set(band ? "" : std::to_string(r.kills)); l.kills.Color(r.ink);
    l.deaths.Set(band ? "" : std::to_string(r.deaths)); l.deaths.Color(r.ink);
    l.score.Set(std::to_string(r.score)); l.score.Color(r.ink);
    l.ping.Set(band || !r.pingMs ? "" : std::to_string(r.pingMs) + " ms");
    l.status.Set(r.dead ? "DEAD" : "");
}

void ApplyBoard(const Board& b) {
    g.board.Show(b.visible);
    if (!b.visible) return;
    g.boardTitle.Set(b.title);
    for (size_t i = 0; i < g.boardLines.size(); ++i) {
        g.boardLines[i].frame.Show(i < b.rows.size());
        if (i < b.rows.size()) ApplyBoardLine(g.boardLines[i], b.rows[i]);
    }
    Line(g.boardFooter, b.footer, !b.footer.empty());
}
}  // namespace

bool HudTabHeld() {
    if (!(GetAsyncKeyState(VK_TAB) & 0x8000)) return false;   // the key first: asking whose window is in front costs more
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void HudTick(const HudScene& s) {
    if (!g.hudClass) g.hudClass = FindClass("UI_HUD_C");
    if (!g.hudClass) return;
    if (!g.hud || ObjectAt(g.hudIndex) != g.hud || ClassOf(g.hud) != g.hudClass) {
        if (g.hud) { UClass* cls = g.hudClass; ULONGLONG next = g.nextScan; g = State{}; g.hudClass = cls; g.nextScan = next; }
        ULONGLONG now = GetTickCount64();
        if (!s.visible || now < g.nextScan) return;
        g.nextScan = now + 1000;
        g.hud = OnScreen(FindInstances("UI_HUD_C"));
        if (!g.hud) return;
        g.hudIndex = ObjectIndex(g.hud);
    }
    if (!g.built && !Build()) return;
    ApplyTop(s);
    Apply(g.banner, s.banner);
    Apply(g.bannerSub, s.bannerSub);
    ApplyDead(s.dead);
    ApplyFeed(s.feed);
    ApplyBoard(s.board);
}

}  // namespace hsm::ui

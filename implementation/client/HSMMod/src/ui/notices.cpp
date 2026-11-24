#include "ui/notices.h"
#include "ui/umg.h"
#include "ui/skin.h"
#include "ui/hud_host.h"
#include "log.h"
#include <windows.h>
#include <array>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr int32_t kHudZ = 50;                     // the kill feed's old layer; the chat is above (60), the scoreboard at 52
constexpr int32_t kMenuZ = 120;                   // over the multiplayer page (Z 100)
constexpr float kWidth = 440, kEdge = 4, kGap = 6, kBelow = 10;   // the mock: a 440 px notice, a 4 px edge, 6 px between, 10 px to the feed
constexpr float kRight = 29, kHudTop = 32, kMenuTop = 140;        // from the screen's right edge; from its top (the page header ends at 140)
constexpr float kTitleSize = style::type::kHint, kTextSize = style::type::kBody;
constexpr size_t kFeedSpans = 3;
constexpr Rgba kOutline = style::kOutlineInk;

struct Row { Fixed box; Panel back, edge; Text title, text; };
struct FeedLine { Panel panel; std::array<Text, kFeedSpans> spans; };

// One mounted copy of the stack: a column at the top right of a user widget's root canvas. The HUD's also holds the kill feed.
struct Mount {
    Kit kit;
    Box column;
    std::array<Row, kNoticeShown> rows;
    Panel moreBack; Text more;
    Box feed; std::array<FeedLine, kFeedRows> feedLines;
    bool ok = false;
};

struct HudMount {
    HudHost host{"notices"};   // the UI_HUD the column sits on (kept when the column is made again)
    bool built = false;
    Mount m;
};

struct MenuMount {
    UObject* obj = nullptr; int32_t index = -1; UClass* cls = nullptr;
    bool built = false;
    Mount m;
};

struct Data {
    NoticeStack stack;
    std::string lastLine;       // the last line logged: a notice that is pushed again unchanged is not logged again
    HudMount hud;
    MenuMount menu;
} d;

uint32_t NowMs() { return uint32_t(GetTickCount64()); }   // wrap-safe for elapsed-time math, as the chat's

const char* KindName(NoticeKind k) {
    switch (k) {
        case NoticeKind::Info: return "info";
        case NoticeKind::Done: return "done";
        case NoticeKind::Warn: return "warn";
        case NoticeKind::Error: return "error";
    }
    return "info";
}

// Text in the game's font with a dark outline, never taking clicks (the chat's look).
Text MakeText(Kit& kit, float size, Rgba color) {
    Text t = kit.MakeText();
    if (!t) return t;
    t.Font(size, 1, kOutline);
    t.Color(color);
    t.Align(Justify::Left);
    return t;
}

// The kill feed's ink: text written on a plate of the game's art, as the HUD's score strip writes (hud.cpp).
Text MakeInk(Kit& kit, float size) {
    Text t = kit.MakeText();
    if (!t) return t;
    if (!style::ParchmentMenus()) t.Font(size, 1, kOutline); else t.Font(size);
    skin::Font(t, skin::Face::Body);
    t.Color(style::OnParchment(style::kMenuText));
    t.Align(Justify::Left);
    return t;
}

void Ink(Text& t, Rgba c) { t.Color(style::OnParchment(c)); }

// The column at the top right of `root`: the notices, newest first, then (HUD) the kill feed under them. Widgets for the most
// the stack shows; the rest of the time they are collapsed.
bool BuildColumn(Mount& m, Canvas& root, int32_t z, float top, bool withFeed) {
    Kit& kit = m.kit;
    m.column = kit.MakeColumn();
    root.Add(m.column).Anchor({{1, 0}, {1, 0}}, {1, 0}).AutoSize().Z(z).Position({-kRight, top});
    for (size_t i = 0; i < m.rows.size(); ++i) {
        Row& r = m.rows[i];
        r.box = kit.MakeFixed();
        r.box.Width(kWidth);
        m.column.Add(r.box).Align(HAlign::Right, VAlign::Top).Padding({0, i ? kGap : 0.f, 0, 0});
        r.back = kit.MakePanel();
        r.back.Color(style::kSmoke);
        r.box.Content(r.back).Align(HAlign::Fill, VAlign::Fill);
        Box line = kit.MakeRow();
        r.back.Content(line).Align(HAlign::Fill, VAlign::Fill);
        Fixed edgeBox = kit.MakeFixed();
        edgeBox.Width(kEdge);
        line.Add(edgeBox).Align(HAlign::Left, VAlign::Fill);
        r.edge = kit.MakePanel();
        edgeBox.Content(r.edge).Align(HAlign::Fill, VAlign::Fill);
        Box body = kit.MakeColumn();
        line.Add(body).Fill().Align(HAlign::Fill, VAlign::Center).Padding({12, 8, 12, 9});
        r.title = MakeText(kit, kTitleSize, style::kIron);
        body.Add(r.title).Align(HAlign::Left, VAlign::Top);
        r.text = MakeText(kit, kTextSize, style::kBone);
        r.text.Wrap(true);
        body.Add(r.text).Align(HAlign::Fill, VAlign::Top).Padding({0, 2, 0, 0});
        r.box.Show(false);
    }
    m.moreBack = kit.MakePanel();
    m.moreBack.Color(style::kSmoke);
    m.moreBack.Padding({10, 3, 10, 3});
    m.column.Add(m.moreBack).Align(HAlign::Right, VAlign::Top).Padding({0, kGap, 0, 0});
    m.more = MakeText(kit, kTitleSize, style::kIron);
    m.moreBack.Content(m.more);
    m.moreBack.Show(false);
    if (withFeed) {
        m.feed = kit.MakeColumn();
        m.column.Add(m.feed).Align(HAlign::Right, VAlign::Top).Padding({0, kBelow, 0, 0});
        for (FeedLine& f : m.feedLines) {
            f.panel = kit.MakePanel();
            skin::Paint(f.panel, skin::Art::Plate);
            skin::Pad(f.panel, skin::Art::Plate, {14, 4, 14, 4});
            m.feed.Add(f.panel).Align(HAlign::Right, VAlign::Top).Padding({0, 0, 0, 3});
            Box row = kit.MakeRow();
            f.panel.Content(row);
            for (Text& t : f.spans) { t = MakeInk(kit, style::type::kEmphasis); row.Add(t).Align(HAlign::Left, VAlign::Center); }
            f.panel.Show(false);
        }
    }
    return m.rows[0].text && m.more;
}

void ApplyNotices(Mount& m, const NoticeScene& s) {
    for (size_t i = 0; i < m.rows.size(); ++i) {
        Row& r = m.rows[i];
        const bool has = i < s.rows.size();
        r.box.Show(has);
        if (!has) continue;
        const NoticeView& v = s.rows[i];
        r.edge.Color(v.ink);
        r.title.Show(!v.title.empty());
        r.title.Set(v.title); r.title.Color(v.ink);
        r.text.Set(v.text);
        r.box.Opacity(v.opacity);
    }
    m.moreBack.Show(s.more > 0);
    if (s.more > 0) m.more.Set("+" + std::to_string(s.more) + " more");
}

// The kill feed, as the HUD drew it before the notices took the corner.
void ApplyFeed(Mount& m, const std::vector<FeedView>& feed) {
    for (size_t i = 0; i < m.feedLines.size(); ++i) {
        FeedLine& f = m.feedLines[i];
        f.panel.Show(i < feed.size());
        if (i >= feed.size()) continue;
        const FeedView& v = feed[i];
        skin::Paint(f.panel, v.row.mine ? skin::Art::PlatePicked : skin::Art::Plate);   // our own kills on the amber button
        f.panel.Opacity(v.opacity);
        for (size_t k = 0; k < f.spans.size(); ++k) {
            const bool has = k < v.row.spans.size();
            f.spans[k].Show(has);
            if (has) { f.spans[k].Set(v.row.spans[k].text); Ink(f.spans[k], v.row.mine && !style::ClassicMenus() ? style::kMenuOnPicked : v.row.spans[k].color); }
        }
    }
}

bool BuildHud(HudMount& h) {
    h.built = true;
    h.m.kit = Kit(h.host.Obj(), Vis::HitTestInvisible);   // the notices never take a click
    Canvas root = h.m.kit.Root();
    if (!root) { HSM_LOG("notices: UI_HUD root is not a CanvasPanel; notices disabled"); return false; }
    ProbeFont(h.m.kit, h.host.Obj());
    h.m.ok = BuildColumn(h.m, root, kHudZ, kHudTop, true);
    if (!h.m.ok) { HSM_LOG("notices: could not make the notice widgets"); return false; }
    if (style::HudOpacity() < 1.f) h.m.column.Opacity(style::HudOpacity());   // hud_opacity, as the kill feed took it
    HSM_LOG("notices: stack added to UI_HUD");
    return true;
}

void BuildMenu(MenuMount& mm) {
    mm.built = true;
    mm.m.kit = Kit(mm.obj, Vis::HitTestInvisible);
    Canvas root = mm.m.kit.Root();
    if (!root) { HSM_LOG("notices: the main menu root is not a CanvasPanel; no notices on the multiplayer page"); return; }
    ProbeFont(mm.m.kit, mm.obj);
    mm.m.ok = BuildColumn(mm.m, root, kMenuZ, kMenuTop, false);
    if (mm.m.ok) HSM_LOG("notices: stack added to the main menu");
    else HSM_LOG("notices: could not make the notice widgets on the main menu");
}

}  // namespace

void Notify(NoticeKind kind, const std::string& title, const std::string& text, const std::string& key) {
    const std::string clean = SanitizeNotice(text);
    if (clean.empty()) return;
    Notice n;
    n.kind = kind; n.title = title; n.text = text; n.key = key;
    const bool kept = d.stack.Push(std::move(n), NowMs());
    const std::string line = std::string("[") + KindName(kind) + "] " + SanitizeNotice(title, kNoticeTitleBytes) + ": " + clean;
    if (line == d.lastLine) return;
    d.lastLine = line;
    HSM_LOG("notices: %s%s", line.c_str(), kept ? "" : " (not kept: the stack is full of warnings)");
}

void Notify(const hsm::notices::Msg& m) { Notify(m.kind, m.title, m.text, m.key); }
void Notify(hsm::notices::Id id) { Notify(hsm::notices::Make(id)); }
void NoticeClear(const std::string& key) { d.stack.Clear(key); }

void NoticesTick(const HudScene& s) {
    HudMount& h = d.hud;
    // The column sits on the UI_HUD; when the game swaps it the column went with it and is made again on the new one.
    if (h.host.Follow(s.visible, h.built) == HudHost::Step::Changed) { h.built = false; h.m = Mount{}; }
    if (!h.host.Obj()) return;
    if (!h.built && !BuildHud(h)) return;
    if (!h.m.ok) return;
    h.m.column.Show(s.visible);
    if (!s.visible) return;
    ApplyNotices(h.m, d.stack.View(NowMs()));
    ApplyFeed(h.m, s.feed);
}

void NoticesDrop() {
    if (d.hud.m.ok) d.hud.m.column.Show(false);
}

void NoticesMenuTick(UObject* menu, bool panelOpen) {
    if (!menu) return;
    MenuMount& mm = d.menu;
    if (mm.obj != menu || ObjectAt(mm.index) != menu || ClassOf(menu) != mm.cls) {   // a new menu (the settings screens make one)
        mm = MenuMount{};
        mm.obj = menu; mm.index = ObjectIndex(menu); mm.cls = ClassOf(menu);
    }
    if (!mm.built) {
        if (!panelOpen) return;   // built when the page first opens
        BuildMenu(mm);
    }
    if (!mm.m.ok) return;
    mm.m.column.Show(panelOpen);
    if (panelOpen) ApplyNotices(mm.m, d.stack.View(NowMs()));
}

}  // namespace hsm::ui

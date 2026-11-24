#include "ui/dropdown.h"
#include "gameplay/fetch_link.h"
#include "ui/notices.h"
#include "log.h"
#include "match/hsm_hud_style.h"
#include <windows.h>
#include <algorithm>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

constexpr float kRowHeight = 40;                 // one option, in px
constexpr float kRowGap = 2;                     // the frame colour shows between the options
constexpr float kRowPitch = kRowHeight + kRowGap;
constexpr float kMinWidth = 200;                 // a list is as wide as its control, and never narrower than this
constexpr float kCharWidth = 12;                 // and wide enough for its longest label: about this wide a character at the row type size,
constexpr float kRowSides = 44;                  // plus the row's own sides and room for a scroll bar (DropWidth)
constexpr float kEdge = 8;                       // and stays this far inside the screen
constexpr double kFallbackWidth = 1920, kFallbackHeight = 1080;   // a screen whose size could not be read
constexpr int kRevealFrames = 3;                 // the scroll to the picked option is repeated until the list has been laid out
constexpr int kMeasureTries = 30;                // a control that had not been laid out yet is measured again for this many frames
constexpr Rgba kClear{1, 1, 1, 0};

// Every built layer, so DropdownOpen() sees whichever one has a list up.
std::vector<DropdownLayer*>& Layers() {
    static std::vector<DropdownLayer*>& v = *new std::vector<DropdownLayer*>;   // never destroyed: a layer in a global may outlive a static
    return v;
}

}  // namespace

bool DropdownOpen() {
    for (const DropdownLayer* l : Layers()) if (l->IsOpen()) return true;
    return false;
}

void CloseDropdowns(const char* reason) {
    for (DropdownLayer* l : Layers()) if (l->IsOpen()) l->Close(reason);
}

DropdownLayer::~DropdownLayer() {
    std::vector<DropdownLayer*>& all = Layers();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

bool DropdownLayer::Build(Kit& kit, Canvas root, int32_t z) {
    if (built_) return true;
    maker_ = mp::Maker(&kit);
    root_ = root;
    backdrop_ = kit.MakeButton();
    Panel fill = kit.MakePanel();
    frame_ = kit.MakePanel();
    scroll_ = kit.MakeScroll();
    list_ = kit.MakeColumn();
    if (!root || !backdrop_ || !fill || !frame_ || !scroll_ || !list_) { HSM_LOG("ui: dropdown widgets could not be made"); return false; }
    backdrop_.Background(kClear);
    fill.Color({0, 0, 0, 0});                    // nothing to see; it only gives the button a face to be hit on
    backdrop_.Content(fill).Align(HAlign::Fill, VAlign::Fill);
    root.Add(backdrop_).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(z);
    slot_ = root.Add(frame_);
    slot_.Anchor({{0, 0}, {0, 0}}, {0, 0}).Z(z + 1);
    frame_.Color(style::kMenuFrame);
    frame_.Padding({0, 0, 0, 0});
    frame_.Content(scroll_).Align(HAlign::Fill, VAlign::Fill);
    scroll_.Add(list_);
    for (size_t i = 0; i < kDropdownPool; ++i) {
        mp::Flat f = maker_.MakeFlat("", style::type::kRow, {14, 3, 14, 3});
        Fixed cell = kit.MakeFixed();
        if (!f.card || !cell) { HSM_LOG("ui: dropdown rows could not be made"); return false; }
        cell.Height(kRowHeight);
        cell.Content(f.card.button).Align(HAlign::Fill, VAlign::Fill);
        list_.Add(cell).Padding({0, i ? kRowGap : 0, 0, 0});
        cell.Show(false);
        rows_.push_back(f);
        cells_.push_back(cell);
    }
    backdrop_.Show(false);
    frame_.Show(false);
    built_ = true;
    std::vector<DropdownLayer*>& all = Layers();
    if (std::find(all.begin(), all.end(), this) == all.end()) all.push_back(this);
    HSM_LOG("ui: dropdown layer built");
    return true;
}

void DropdownLayer::Hide() {
    if (!built_) return;
    backdrop_.Show(false);
    frame_.Show(false);
}

// The widget's box in the root canvas, from the engine's cached layout of both (the cached geometry is the last one drawn: not ok
// before the widget has been). view: the canvas' own size, the space PlaceDropdown works in.
DropdownLayer::Rect DropdownLayer::Measure(const Widget& w, Vec2* view) const {
    Rect r;
    *view = {kFallbackWidth, kFallbackHeight};
    UObject* cls = FindObject("SlateBlueprintLibrary", "Class");
    UObject* lib = cls ? DefaultObject(reinterpret_cast<UClass*>(cls)) : nullptr;
    if (!lib || !w || !root_) return r;
    Call mine(w.get(), "GetCachedGeometry"), canvas(root_.get(), "GetCachedGeometry");
    if (!mine.valid() || !canvas.valid() || !mine.invoke() || !canvas.invoke()) return r;
    const void* geo = mine.paramPtr("ReturnValue");
    const void* canvasGeo = canvas.paramPtr("ReturnValue");
    if (!geo || !canvasGeo) return r;
    Call size(lib, "GetLocalSize"), top(lib, "LocalToAbsolute"), canvasSize(lib, "GetLocalSize");
    if (!size.valid() || !top.valid() || !canvasSize.valid()) return r;
    size.setFrom("Geometry", geo);
    canvasSize.setFrom("Geometry", canvasGeo);
    top.setFrom("Geometry", geo).set<Vec2>("LocalCoordinate", Vec2{});
    if (!size.invoke() || !canvasSize.invoke() || !top.invoke()) return r;
    const Vec2 s = size.get<Vec2>("ReturnValue"), v = canvasSize.get<Vec2>("ReturnValue"), abs = top.get<Vec2>("ReturnValue");
    Call local(lib, "AbsoluteToLocal");
    if (!local.valid()) return r;
    local.setFrom("Geometry", canvasGeo).set<Vec2>("AbsoluteCoordinate", abs);
    if (!local.invoke()) return r;
    const Vec2 at = local.get<Vec2>("ReturnValue");
    if (!(s.x > 0) || !(s.y > 0) || !(v.x > 0) || !(v.y > 0)) return r;
    *view = v;
    r.ok = true; r.x = at.x; r.y = at.y; r.w = s.x; r.h = s.y;
    return r;
}

// Puts the list under or over `r` (the whole screen's middle when the control could not be measured).
void DropdownLayer::Lay(const Rect& r, const Vec2& view) {
    Rect c = r;
    if (!c.ok) { c.w = kMinWidth; c.x = (view.x - kMinWidth) / 2; c.y = view.y / 2; c.h = 0; }
    const DropPlace place = PlaceDropdown(float(c.y), float(c.y + c.h), float(view.y), kRowPitch, model_.Options().size());
    model_.Place(place);
    const double w = DropWidth(float(c.w), model_.Options(), kCharWidth, kRowSides, kMinWidth, float(view.x - 2.0 * kEdge));
    const double x = std::max(double(kEdge), std::min(c.x, view.x - w - kEdge));
    slot_.Position({x, place.top}).Size({w, place.height});
    scroll_.ScrollTo(float(model_.First()) * kRowPitch);
}

bool DropdownLayer::Open(uint32_t owner, const std::string& name, const Widget& anchor, const std::vector<DropOption>& options, size_t picked) {
    if (!built_ || options.empty()) return false;
    std::vector<DropOption> shown = options;
    if (shown.size() > kDropdownPool) shown.resize(kDropdownPool);
    name_ = name;
    anchor_ = anchor;
    Vec2 view;
    const Rect r = Measure(anchor, &view);
    const DropPlace first = r.ok ? PlaceDropdown(float(r.y), float(r.y + r.h), float(view.y), kRowPitch, shown.size()) : DropPlace{};
    if (!model_.Open(owner, shown, picked, context_, first)) return false;
    for (size_t i = 0; i < rows_.size(); ++i) {
        cells_[i].Show(i < shown.size());
        if (i < shown.size()) rows_[i].label.Set(shown[i].label);
    }
    Lay(r, view);
    measured_ = r.ok;
    measureTries_ = 0;
    reveal_ = kRevealFrames;
    escDown_ = mp::KeyDown(VK_ESCAPE);            // the Esc that opened a page is not this list's
    backdrop_.Show(true);
    frame_.Show(true);
    HSM_LOG("ui: dropdown opened: %s (%zu options, %s picked, %s%s)", name.c_str(), shown.size(), shown[model_.Picked()].label.c_str(),
            model_.Place().side == DropSide::Below ? "below" : "above", r.ok ? "" : ", control not measured");
    return true;
}

void DropdownLayer::Close(const char* reason) {
    const DropEvent e = model_.Close(reason);
    if (e.kind == DropEvent::Kind::Closed) Log(e);
    Hide();
}

void DropdownLayer::Log(const DropEvent& e) {
    switch (e.kind) {
        case DropEvent::Kind::Picked:
            HSM_LOG("ui: dropdown picked: %s = %s", name_.c_str(), model_.Options()[e.index].label.c_str());
            break;
        case DropEvent::Kind::Explain:
            HSM_LOG("ui: dropdown: %s is not available: %s", model_.Options()[e.index].label.c_str(), e.why.empty() ? "(no reason)" : e.why.c_str());
            break;
        case DropEvent::Kind::Closed:
            HSM_LOG("ui: dropdown closed: %s (%s)", name_.c_str(), e.reason);
            break;
        case DropEvent::Kind::None: break;
    }
}

DropEvent DropdownLayer::Tick(uint32_t context) {
    context_ = context;
    DropEvent e;
    if (!built_) return e;
    const bool esc = mp::KeyDown(VK_ESCAPE) && !game::fetchlink::KeysHeld();   // Esc in Fetch's menu is Fetch's
    const bool escEdge = esc && !escDown_;
    escDown_ = esc;
    if (!model_.IsOpen()) { Hide(); return e; }
    // A control that could not be measured when the list opened (its layout was not drawn yet) is measured again.
    if (!measured_ && measureTries_++ < kMeasureTries) {
        Vec2 view;
        const Rect r = Measure(anchor_, &view);
        if (r.ok) { measured_ = true; Lay(r, view); }
    }
    if (reveal_ > 0) { --reveal_; scroll_.ScrollTo(float(model_.First()) * kRowPitch); }
    // The list first, in this order: a change of page, Esc, an option, the backdrop. Rows are polled every frame they show.
    e = model_.Context(context);
    if (e.kind == DropEvent::Kind::None && escEdge) e = model_.Esc();
    const size_t n = model_.Options().size();
    bool clickedRow = false;
    for (size_t i = 0; i < n; ++i) {
        mp::Flat& f = rows_[i];
        if (!mp::Clicked(f.card)) continue;
        clickedRow = true;
        if (e.kind == DropEvent::Kind::None) e = model_.ClickOption(i);
    }
    if (e.kind == DropEvent::Kind::None && !clickedRow && backdrop_.Clicked()) e = model_.ClickOutside();
    if (e.kind == DropEvent::Kind::Explain) Notify(notices::StartWhy(e.why));
    Log(e);
    if (!model_.IsOpen()) { Hide(); return e; }
    for (size_t i = 0; i < n; ++i) {                // the picked option highlighted, a disabled one dimmed
        mp::Flat& f = rows_[i];
        const bool ok = model_.Options()[i].enabled;
        f.card.selected = i == model_.Picked();
        f.label.Color(mp::InkOn(f.card, ok ? style::kMenuText : style::kMenuDim));
        mp::Dimmed(f.card, ok);
        mp::Paint(f.card);
    }
    return e;
}

}  // namespace hsm::ui

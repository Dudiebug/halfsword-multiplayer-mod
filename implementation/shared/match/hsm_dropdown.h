// The dropdown list as pure rules: a control with named choices (a filter, a RULES row, a setting) opens a list; a pick
// sets the choice, a click anywhere else, Esc, or a change of page closes it. Only one list is open at a time, it opens
// upward when there is no room below, shows about eight rows and scrolls past that, and a disabled option explains why
// and leaves the list open. The model holds no widgets: ui/dropdown.cpp draws what it says and hands it the clicks.
// Game-independent and unit-tested (test_hsm_dropdown.cpp).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hsm {

// The list shows this many rows; a longer list scrolls. The widget pool holds the most options a list can have (the browser's
// mode filter has 21): a longer list shows its first kDropdownPool options.
inline constexpr size_t kDropdownRows = 8;
inline constexpr size_t kDropdownPool = 24;

struct DropOption {
    std::string label;
    bool enabled = true;
    std::string why;                    // a disabled option: what a click on it says (empty: nothing)
};

// ---------------------------------------------------------------- where the list goes
enum class DropSide : uint8_t { Below, Above };
struct DropPlace {
    DropSide side = DropSide::Below;
    float top = 0;                      // the list's top edge, in the same space as the control's edges
    float height = 0;                   // rows * rowHeight
    size_t rows = 0;                    // rows on screen at once
};

// Below the control when the rows fit there, else above it; when neither side has room for them all, the roomier side with
// as many whole rows as fit (at least one). controlTop and controlBottom are the control's edges and viewHeight the space
// they are in (the overlay's height); rowHeight is one row.
inline DropPlace PlaceDropdown(float controlTop, float controlBottom, float viewHeight, float rowHeight, size_t count, size_t maxRows = kDropdownRows) {
    DropPlace p;
    const size_t want = std::min(count, maxRows);
    if (want == 0 || !(rowHeight > 0)) return p;
    const float below = std::max(0.f, viewHeight - controlBottom);
    const float above = std::max(0.f, controlTop);
    const float need = float(want) * rowHeight;
    p.side = below >= need || (above < need && below >= above) ? DropSide::Below : DropSide::Above;
    const float room = p.side == DropSide::Below ? below : above;
    p.rows = room >= need ? want : std::max<size_t>(1, std::min(want, size_t(room / rowHeight)));
    p.height = float(p.rows) * rowHeight;
    p.top = p.side == DropSide::Below ? controlBottom : controlTop - p.height;
    return p;
}

// The first row on screen when the list opens: the picked option shows (about the middle of a list that scrolls).
inline size_t RevealFirst(size_t picked, size_t count, size_t rows) {
    if (rows == 0 || count <= rows) return 0;
    const size_t half = rows / 2;
    return std::min(picked > half ? picked - half : size_t(0), count - rows);
}

// How wide the list is: as wide as its control, never narrower than minWidth, and wide enough for its longest label (about
// charWidth per character, plus padding for the row's sides and a scroll bar), but never wider than maxWidth. A label that
// does not fit in a list as wide as the screen is cut by the screen, not by this.
inline float DropWidth(float controlWidth, const std::vector<DropOption>& options, float charWidth, float padding, float minWidth, float maxWidth) {
    size_t longest = 0;
    for (const DropOption& o : options) {
        size_t n = 0;
        for (unsigned char c : o.label) if ((c & 0xC0) != 0x80) ++n;     // characters, not UTF-8 bytes
        longest = std::max(longest, n);
    }
    const float need = float(longest) * charWidth + padding;
    return std::max(std::min(std::max({controlWidth, minWidth, need}), maxWidth), 0.f);
}

// ---------------------------------------------------------------- the open list
struct DropEvent {
    enum class Kind : uint8_t { None, Picked, Explain, Closed };
    Kind kind = Kind::None;
    uint32_t owner = 0;
    size_t index = 0;                   // Picked: the option
    std::string why;                    // Explain: what the disabled option says
    const char* reason = "";            // Closed: "outside", "esc", "page", "pick" or "other" (for the log)
};

class Dropdown {
public:
    // Opens `owner`'s list, closing any other (one list at a time). `context` is the page the control is on (Context). False
    // for no options. A list of one option opens too: the control decides whether that is worth a click.
    bool Open(uint32_t owner, std::vector<DropOption> options, size_t picked, uint32_t context, const DropPlace& place) {
        if (options.empty()) return false;
        open_ = true; owner_ = owner; options_ = std::move(options);
        picked_ = std::min(picked, options_.size() - 1);
        context_ = context; place_ = place;
        first_ = RevealFirst(picked_, options_.size(), place_.rows ? place_.rows : kDropdownRows);
        pick_ = false;
        return true;
    }
    // The list opened before its row height was known: the real placement, once the widgets have been measured.
    void Place(const DropPlace& place) {
        if (!open_) return;
        place_ = place;
        first_ = RevealFirst(picked_, options_.size(), place_.rows ? place_.rows : kDropdownRows);
    }
    bool IsOpen() const { return open_; }
    bool IsOpen(uint32_t owner) const { return open_ && owner_ == owner; }
    uint32_t Owner() const { return owner_; }
    const std::vector<DropOption>& Options() const { return options_; }
    size_t Picked() const { return picked_; }
    const DropPlace& Place() const { return place_; }
    size_t First() const { return first_; }                      // the row to scroll to when it opens

    // A click on option `i`: a usable one is picked and the list closes; a disabled one explains and stays open.
    DropEvent ClickOption(size_t i) {
        DropEvent e;
        if (!open_ || i >= options_.size()) return e;
        e.owner = owner_;
        if (!options_[i].enabled) { e.kind = DropEvent::Kind::Explain; e.index = i; e.why = options_[i].why; return e; }
        e.kind = DropEvent::Kind::Picked; e.index = i; e.reason = "pick";
        pickOwner_ = owner_; pickIndex_ = i; pick_ = true;
        open_ = false;
        return e;
    }
    // A click anywhere but on the list (the screen-wide backdrop, which includes the control that opened it).
    DropEvent ClickOutside() { return Close("outside"); }
    // Esc: closes the list and nothing else. None when no list is open (the key is someone else's).
    DropEvent Esc() { return Close("esc"); }
    // The page the control is on, every frame: another page (a tab, a sub-tab, the lobby left) closes the list.
    DropEvent Context(uint32_t now) { return open_ && now != context_ ? Close("page") : DropEvent{}; }
    DropEvent Close(const char* reason = "other") {
        DropEvent e;
        if (!open_) return e;
        e.kind = DropEvent::Kind::Closed; e.owner = owner_; e.reason = reason;
        open_ = false;
        return e;
    }
    // The option picked from `owner`'s list since it was last asked, once. A list opened since forgets an unclaimed pick.
    bool TakePick(uint32_t owner, size_t& index) {
        if (!pick_ || pickOwner_ != owner) return false;
        pick_ = false; index = pickIndex_;
        return true;
    }

private:
    bool open_ = false, pick_ = false;
    uint32_t owner_ = 0, context_ = 0, pickOwner_ = 0;
    size_t picked_ = 0, first_ = 0, pickIndex_ = 0;
    std::vector<DropOption> options_;
    DropPlace place_;
};

}  // namespace hsm

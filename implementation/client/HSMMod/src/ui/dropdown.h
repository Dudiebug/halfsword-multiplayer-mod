// The dropdown list's widgets (docs/UI_STYLE.md "Dropdowns"): a control with named choices (a RULES row, a browser filter, a
// setting) shows its value on a plate; a click opens a list of every option under it (over it, when there is no room below),
// and a pick sets the choice. What a list does is decided in match/hsm_dropdown.h; this draws it and hands it the clicks.
// One layer per widget tree (the multiplayer page, the pause panel, the Esc menu): built once, hidden with Show(false) and
// never removed (a widget out of the tree is freed by garbage collection, 0.8.0-pre26). Game thread only.
#pragma once
#include "match/hsm_dropdown.h"
#include "ui/mp_widgets.h"
#include "ui/umg.h"
#include <cstdint>
#include <string>
#include <vector>

namespace hsm::ui {

// Who opened a list: a control's number, so a pick finds its way back to it (Dropdown::TakePick).
namespace dropdown_owner {
inline constexpr uint32_t kRulesRow = 0x100;     // + the LobbyRow
inline constexpr uint32_t kModRule = 0x200;      // + the mod rule's row
inline constexpr uint32_t kFilterChip = 0x300;   // + the browser filter chip
inline constexpr uint32_t kMyRegion = 0x400;     // SETTINGS: MY REGION
inline constexpr uint32_t kMenuLook = 0x401;     // SETTINGS and the pause panel: MENU LOOK
inline constexpr uint32_t kVoiceRow = 0x500;     // + the VoiceRow
}  // namespace dropdown_owner

class DropdownLayer {
public:
    DropdownLayer() = default;
    DropdownLayer(const DropdownLayer&) = default;
    DropdownLayer& operator=(const DropdownLayer&) = default;
    ~DropdownLayer();

    // Makes the backdrop (a button over the whole tree: a click anywhere else closes the list) and the list in `root`, hidden.
    // z: the backdrop's canvas order; the list sits one above. Over the page, under its own modal windows.
    bool Build(Kit& kit, Canvas root, int32_t z);
    explicit operator bool() const { return built_; }

    // Every frame the page is up, before the page reads its own clicks: an open list takes the click first (an option, or the
    // backdrop), Esc closes it, and a change of `context` (the page, the tab, a window over it: PageContextKey) closes it.
    // Returns what happened this frame; Picked events are also kept for TakePick.
    DropEvent Tick(uint32_t context);

    // Opens `owner`'s list under (or over) `anchor`, the control that was clicked, with `picked` highlighted. `name` is for the log.
    bool Open(uint32_t owner, const std::string& name, const Widget& anchor, const std::vector<DropOption>& options, size_t picked);
    // The option picked from `owner`'s list since last asked, once.
    bool TakePick(uint32_t owner, size_t& index) { return model_.TakePick(owner, index); }
    bool IsOpen() const { return model_.IsOpen(); }
    bool IsOpen(uint32_t owner) const { return model_.IsOpen(owner); }
    void Close(const char* reason);

private:
    struct Rect { bool ok = false; double x = 0, y = 0, w = 0, h = 0; };
    Rect Measure(const Widget& w, Vec2* view) const;
    void Lay(const Rect& r, const Vec2& view);
    void Hide();
    void Log(const DropEvent& e);

    Dropdown model_;
    mp::Maker maker_;
    Canvas root_;
    Button backdrop_;
    Panel frame_;
    Box scroll_, list_;
    Slot slot_{nullptr};
    std::vector<mp::Flat> rows_;
    std::vector<Fixed> cells_;
    Widget anchor_;
    std::string name_;
    uint32_t context_ = 0;
    bool built_ = false, escDown_ = false, measured_ = true;
    int reveal_ = 0, measureTries_ = 0;
};

// Any list open, in any layer: Esc then closes the list and nothing else (match/hsm_input_mode.h EscOwner::Dropdown).
bool DropdownOpen();
// Closes whichever list is open (the match's Esc: match/hsm_input_mode.h EscOwner::Dropdown).
void CloseDropdowns(const char* reason);

}  // namespace hsm::ui

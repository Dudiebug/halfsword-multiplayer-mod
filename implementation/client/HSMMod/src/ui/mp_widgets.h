// The multiplayer widgets shared by the multiplayer page (ui/menu.cpp) and the in-match Esc menu (ui/pause.cpp): the
// page's card look, the voice settings block, and the Esc menu's panel (players with mute, volume, kick and votekick,
// voice, network). What they show is decided in match/hsm_mp_scene.h; this maps it to UMG widgets. Game thread only.
#pragma once
#include "ui/umg.h"
#include "match/hsm_mp_scene.h"
#include "match/hsm_hud_style.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hsm {
class Session;
namespace ui {
class DropdownLayer;
namespace mp {

// Everything clickable: an invisible Button around a frame around a face (the multiplayer page's look).
struct Card {
    Button button;
    Panel frame, face;
    bool selected = false, live = true;
    bool arrow = false;                       // MakeArrow: the game's arrow art, no card behind it
    float glow = 0;                           // the hover fade, 0..1 (Paint)
    explicit operator bool() const { return bool(button) && bool(frame) && bool(face); }
};
struct Flat { Card card; Text label; };
Rgba Mix(Rgba a, Rgba b, float t);
bool KeyDown(int vk);   // the key is held and the game window is in front: a key typed in another program is not the menu's            // a to b by t

// Makes the page's widgets in one kit's tree.
class Maker {
public:
    Maker() = default;
    explicit Maker(Kit* kit) : kit_(kit) {}
    Kit& kit() { return *kit_; }
    Text NewText(const std::string& text, float size, Rgba color);
    Text Section(Box& column, const std::string& text, float padTop);
    Card MakeCard(const Widget& body, Margin pad);
    Flat MakeFlat(const std::string& text, float size, Margin pad = {16, 7, 16, 7});
    // A stepper's < or > (next: >): the game's ornate arrow on a bare button, or the text on a card when the art is not
    // loaded. Its label stays in the tree (empty beside the arrow) so callers treat it as any Flat.
    Flat MakeArrow(bool next, float size);
    // A dropdown's closed control (docs/UI_STYLE.md): a card across its cell, the value centred. PaintPlate writes it.
    Flat MakePlate(float size);
private:
    Kit* kit_ = nullptr;
};

void Paint(Card& c);                          // per frame: the frame and face for the card's state and the mouse
// The ink for text on a card: the picked ink once the card is chosen (it reads on the picked colour in every look), else base.
inline Rgba InkOn(const Card& c, Rgba base) { return c.selected ? style::kMenuOnPicked : base; }
bool Clicked(Card& c);
void Live(Flat& f, bool on);                  // clickable, or dimmed
// Dimmed, not disabled: a dimmed card still sees the click, so it can say why it does nothing.
void Dimmed(Card& c, bool on);
// A plate's text, picked look and ink: the value and a chevron (v closed, ^ open), the picked card while its list is open.
// usable false (a row only the host changes, seen by a member): the plain value, dimmed, no chevron. Call Paint(f.card) after the poll.
void PaintPlate(Flat& f, const std::string& value, bool open, bool usable);
Slot AddV(Box& column, const Widget& child, float padTop);
Slot AddH(Box& row, const Widget& child, float fill, float padRight);   // fill 0: the child's own width

// The voice settings: voice chat reach (the host's), microphone mode, device, threshold or push-to-talk key, volume,
// HEAR MYSELF, and a level meter. Changes go straight to the session (and HSMMod.ini).
class VoiceBlock {
public:
    // drops: the layer the voice chat, microphone and device lists open in (ui/dropdown.h); without one the rows keep their arrows.
    bool Build(Maker& m, Box& column, float padTop, DropdownLayer* drops = nullptr);
    void Tick(Session& session);              // each frame while shown
    void Show(bool on);
    bool Binding() const { return binding_; }
    void StopBinding() { binding_ = false; }
private:
    struct RowView { Box box; Text label, value, hint; Flat prev, next, bind, plate; };
    void PollBinding(Session& session, const MpVoice& v);
    Box root_;
    std::array<RowView, size_t(VoiceRow::Count)> rows_;
    Bar meter_;
    Icon mark_;
    Slot markSlot_{nullptr};
    float markAt_ = -1;
    Text meterText_, note_;
    bool binding_ = false;
    std::vector<int> heldAtBind_;             // keys already down when binding began (they must be let go first)
    DropdownLayer* drops_ = nullptr;
};

// The Esc menu in a multiplayer match: a full-screen panel on the stock pause menu, which GAME MENU shows again
// (Layout::Overlay), or the PLAYERS page of the mod's own pause panel (Layout::Pause, ui/pause_panel.h): the players and
// network tabs (the voice settings are in MOD SETTINGS), with BACK TO MENU alone in the footer.
class EscPanel {
public:
    enum class Action : uint8_t { None, Resume, GameMenu, ReportBug, Leave, Back };
    enum class Layout : uint8_t { Overlay, Pause };
    bool Build(Kit& kit, Canvas root, Layout layout = Layout::Overlay);
    Action Tick(Session& session, bool reportOn);   // each frame while shown
    void Show(bool on);
    bool Shown() const { return shown_; }
    explicit operator bool() const { return bool(panel_); }
private:
    static constexpr size_t kRows = 20;   // everyone in a full lobby of 20 (the page scrolls)
    struct PlayerView { Box row; Icon talk; Text name, detail, volume; Flat mute, down, up, kick, ban, vote, voteBan; };
    void Refresh(Session& session);
    void HandlePlayers(Session& session, uint32_t now);
    Maker m_;
    Panel panel_;
    bool shown_ = false;
    Text title_, sub_;
    std::array<Flat, 3> tabs_;
    std::array<Box, 3> pages_;
    int tab_ = 0;
    Panel voteBar_;
    Text voteText_;
    Flat voteYes_, voteNo_;
    std::array<PlayerView, kRows> rows_;
    std::vector<MpPlayer> players_;           // what the rows show: their slots and listen settings
    MpVote vote_;
    Text alone_;
    VoiceBlock voice_;
    std::shared_ptr<DropdownLayer> drops_;    // the Esc menu's own layer (Layout::Overlay: its VOICE tab opens lists)
    std::array<Text, 10> netLines_;
    Flat resume_, loadout_, gameMenu_, report_, help_, leave_, back_;   // loadout_: CHANGE LOADOUT (session/hsm_loadout_change.h; the overlay's)
    Layout layout_ = Layout::Overlay;
    Text loadoutNote_;   // the overlay's reason under the buttons while CHANGE LOADOUT is dimmed
    uint32_t nextData_ = 0, leaveArmedAt_ = 0, confirmAt_ = 0;
    int confirmRow_ = -1;
    bool confirmBan_ = false;
};

}  // namespace mp
}  // namespace ui
}  // namespace hsm

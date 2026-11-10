#include "ui/skin.h"
#include "log.h"
#include <array>

namespace hsm::ui::skin {
using namespace hsm::ue;
namespace {

// slice: the shares of the texture that keep their size (its frame, rods or torn edges); scale: how large those are
// drawn, as a share of the texture's own pixels.
struct ArtSpec { const char* texture; Margin slice; float scale; };

constexpr std::array<ArtSpec, size_t(Art::Count)> kArt{{
    {"Tavern", {}, 1},
    {"swatch_picking_paper", {0.04f, 0.09f, 0.04f, 0.11f}, 0.5f},
    {"T_Old-Paper-Texture-2", {0.06f, 0.09f, 0.06f, 0.09f}, 0.35f},
    {"scroll_question_big", {0.08f, 0.06f, 0.05f, 0.09f}, 0.4f},
    {"UI_Button_Empty", {0.075f, 0.21f, 0.075f, 0.21f}, 0.24f},
    {"UI_Button_Pressed_001", {0.075f, 0.21f, 0.075f, 0.21f}, 0.24f},
    {"UI_Button_Denied_001", {0.075f, 0.21f, 0.075f, 0.21f}, 0.24f},
    {"banner_bottom", {}, 1},
    {"Inventory_UI_stripe_item_deselected", {0.03f, 0.2f, 0.03f, 0.2f}, 0.4f},
    {"UI_Button_Empty", {0.075f, 0.21f, 0.075f, 0.21f}, 0.14f},
    {"UI_Button_Pressed_001", {0.075f, 0.21f, 0.075f, 0.21f}, 0.14f},
}};

// The plain fill that stands for the art: in the classic look always (its dark panels: the HUD's smoke, the menus'
// faces), in the game's look when the art is not loaded (its parchment colours).
Rgba Plain(Art art) {
    constexpr Rgba kAmber{0.85f, 0.45f, 0.03f, 1}, kMineClassic{0.22f, 0.025f, 0.02f, 0.80f}, kNone{0, 0, 0, 0};
    const bool classic = style::ClassicMenus();
    switch (art) {
        case Art::Backdrop: return style::kMenuBg;
        case Art::Scroll: case Art::Note: return style::kMenuPanel;
        case Art::Sheet: return classic ? style::kSmoke : style::kMenuPanel;
        case Art::Card: case Art::Strip: return style::kMenuCard;
        case Art::CardPicked: return classic ? style::kMenuAccent : kAmber;
        case Art::CardRed: return style::kMenuAccent;
        case Art::Plate: return classic ? style::kSmoke : style::kMenuCard;
        case Art::PlatePicked: return classic ? kMineClassic : kAmber;
        case Art::Ribbon: case Art::Count: break;
    }
    return kNone;
}

// The art's texture and its own size, followed by object slot: found again after the engine unloads it (a level change).
struct Found { UObject* tex = nullptr; int32_t index = -1; Vec2 size{}; };

const Found& Texture(Art art) {
    static std::array<Found, size_t(Art::Count)> found;
    Found& f = found[size_t(art)];
    if (f.tex && ObjectAt(f.index) == f.tex) return f;
    f = Found{};
    f.tex = GameTexture(kArt[size_t(art)].texture);
    if (!f.tex) return f;
    f.index = ObjectIndex(f.tex);
    f.size = TextureSize(f.tex);
    if (f.size.x <= 0) HSM_LOG("skin: %s has no readable size; its frame is not padded", kArt[size_t(art)].texture);
    return f;
}

}  // namespace

bool Loaded(Art art) { return !style::ClassicMenus() && size_t(art) < kArt.size() && Texture(art).tex != nullptr; }

void Paint(Panel& p, Art art, Rgba tint) {
    if (!p || size_t(art) >= kArt.size()) return;
    const ArtSpec& a = kArt[size_t(art)];
    if (!Loaded(art)) {                         // the tint's alpha still fades it (the chat's log)
        const Rgba c = Plain(art);
        p.Texture(Look{});
        p.Color({c.r, c.g, c.b, c.a * tint.a});
        return;
    }
    const Found& f = Texture(art);
    p.Texture(Look{f.tex, a.slice, {f.size.x * a.scale, f.size.y * a.scale}, false});
    p.Color(tint);
}

void Pad(Panel& p, Art art, Margin room) {
    if (!p || size_t(art) >= kArt.size()) return;
    const ArtSpec& a = kArt[size_t(art)];
    if (!Loaded(art)) { p.Padding(room); return; }
    const Found& f = Texture(art);
    const float w = float(f.size.x) * a.scale, h = float(f.size.y) * a.scale;
    p.Padding({room.left + a.slice.left * w, room.top + a.slice.top * h, room.right + a.slice.right * w, room.bottom + a.slice.bottom * h});
}

void Font(Text& t, Face face) {
    if (!t || style::ClassicMenus()) return;    // classic: the font the kit copied from the game's own widgets
    static bool logged = false;
    UObject* font = GameFont(face == Face::Heading ? "Falkenburger_Roman_Textura_01_b_Font" : "Willies_Way_02_Font");
    if (!font) {
        if (!logged) HSM_LOG("skin: the game's typefaces are not loaded; text keeps the copied font");
        logged = true;
        return;
    }
    t.Face(font);
}

std::string Title(const std::string& upper) { return style::MenuTitle(upper); }

Rgba CardTint(float glow) {
    constexpr Rgba kNormal{0.86f, 0.84f, 0.80f, 1}, kHover{1, 0.97f, 0.88f, 1};
    return {kNormal.r + (kHover.r - kNormal.r) * glow, kNormal.g + (kHover.g - kNormal.g) * glow, kNormal.b + (kHover.b - kNormal.b) * glow, 1};
}

}  // namespace hsm::ui::skin

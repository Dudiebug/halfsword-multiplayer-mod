// Who gets the keys when HSMMod's chat and HS Enhancer's menu meet (#1290).
//
// Both read the same keyboard, so with both on a key typed in the chat also fired HS Enhancer's hotkeys, and a key typed
// in HS Enhancer's menu opened the chat or fired HSMMod's own. Two rules:
//   - While an HSMMod text box is open (the chat, the lobby chat box, a text field of its menus), HS Enhancer gets no
//     input. A key going UP still reaches it: a key it saw go down before the box opened must not stay down for it.
//   - While HS Enhancer's menu is open, HSMMod's chat and hotkeys wait, as they do for Fetch's menu
//     (gameplay/fetch_link.h KeysHeld).
// The lobby hold (a lobby that blocks HS Enhancer) is the older rule: all input held, but its menu key.
// hse_hold.cpp sorts each window message into an HseMsg and asks; the DLL passes the facts in.
#pragma once
#include <cstdint>

namespace hsm {

// A window message by what the hold does with it.
enum class HseMsg : uint8_t {
    Other,      // not input: always reaches HS Enhancer
    Input,      // a key or mouse button going down, a character, a mouse move, the wheel, raw input
    Release,    // a key or mouse button going up
};

struct HseHolds {
    bool lobby = false;     // the lobby blocks HS Enhancer
    bool typing = false;    // an HSMMod text box is open (and hse_share_keys is on)
};

// `menuKeyDown`: this is HS Enhancer's menu key going down.
inline bool HseGetsMessage(const HseHolds& h, HseMsg m, bool menuKeyDown) {
    if (m == HseMsg::Other) return true;
    if (h.typing) return m == HseMsg::Release;
    if (h.lobby) return menuKeyDown;        // the menu can still close, and open (uselessly: nothing else reaches it)
    return true;
}

struct HseMenuFacts {
    bool share = false;         // hse_share_keys
    bool ready = false;         // HSMMod sits in HS Enhancer's window hook and knows where it passes things on
    bool reports = false;       // this HS Enhancer (0.6 on) keeps raw input to itself while its menu is open
    bool keptInput = false;     // the latest raw input it was given, it kept
    bool held = false;          // its input is held now, so it has no state to read
};

// HS Enhancer's menu is open (it kept the latest raw input), and says so reliably enough to take keys from HSMMod.
inline bool HseMenuHasKeys(const HseMenuFacts& f) {
    return f.share && f.ready && f.reports && f.keptInput && !f.held;
}

}  // namespace hsm

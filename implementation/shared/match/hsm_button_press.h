// A menu button read by polling (pressed, under the mouse, has the keyboard focus, once a frame): when it counts as
// clicked, and whether the keys or a gamepad are on it.
//
// A mouse click counts on release with the mouse still over the button (dragging off cancels it). A press by Enter, Space
// or a gamepad counts on release while the button keeps the focus, but only when the focus came by the keys: a button
// keeps the focus after a mouse click, and Enter pressed later (the chat key) must not click it again.
#pragma once

namespace hsm {

struct ButtonPress {
    bool wasPressed = false;
    bool focused = false;
    bool keyFocus = false;   // the focus came by the keyboard or a gamepad, not by a mouse click on the button
    bool byKey = false;      // the press under way began by a key

    // true once per press, on its release.
    bool Step(bool pressed, bool hovered, bool focusedNow) {
        if (!focusedNow) keyFocus = false;
        else if (!focused) keyFocus = !hovered;        // focus arrived: by the mouse when the mouse is on the button
        focused = focusedNow;
        if (pressed && !wasPressed) {
            if (hovered) keyFocus = false;             // a mouse press takes the button over
            byKey = keyFocus;
        }
        const bool clicked = wasPressed && !pressed && (byKey ? keyFocus : hovered);
        wasPressed = pressed;
        return clicked;
    }
};

}  // namespace hsm

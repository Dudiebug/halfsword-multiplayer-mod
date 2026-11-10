// HS Enhancer's keys held back (docs/PLAN_MOD_RULES.md "HS Enhancer, off at the start"). HSE takes its keys, mouse
// buttons and wheel from the game window's procedure, which it subclasses (and from nowhere else: no GetAsyncKeyState).
// HSMMod points HSE's own imports of SetWindowLongPtr, GetWindowLongPtr and CallWindowProc at its own, so the window
// gets HSMMod's procedure where HSE asked for its own, with HSE's right behind it: while held, keyboard, mouse and raw
// input go past HSE to what HSE would have passed them to, and everything else reaches HSE as before. HSE installed
// already when HSMMod finds it: HSMMod takes its place the same way, if HSE's is the window's procedure still.
#pragma once

namespace hsm::enhancer::hold {
void Attach(void* hseModule);   // game thread, once: HSE's module (the one exporting HSE_Initialize) was found
void Tick();                    // game thread, each frame: takes HSE's place when it can, logs what changed
bool Ready();                   // HSMMod sits in HSE's place, and knows where HSE passes things on: Set works
void Set(bool on);              // on: HSE gets no keyboard, mouse or raw input (game thread)
bool On();                      // Ready and Set(true)
}  // namespace hsm::enhancer::hold

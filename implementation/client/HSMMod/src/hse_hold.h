// HS Enhancer's keys held back (docs/PLAN_MOD_RULES.md "HS Enhancer, off at the start"). HSE takes its keys, mouse
// buttons and wheel from the game window's procedure, which it subclasses (and from nowhere else: no GetAsyncKeyState).
// HSMMod points HSE's own imports of SetWindowLongPtr, GetWindowLongPtr and CallWindowProc at its own, so the window
// gets HSMMod's procedure where HSE asked for its own, with HSE's right behind it: while held, keyboard, mouse and raw
// input go past HSE to what HSE would have passed them to, and everything else reaches HSE as before. HSE installed
// already when HSMMod finds it: HSMMod takes its place the same way, if HSE's is the window's procedure still.
// HSE's menu opens at every start and only its menu key or its X closes it, so its menu key goes through a hold, and a
// locked game (its menu key one no key sends) closes the menu once for the player (#2788: it stayed open, stuck, all game).
#pragma once

namespace hsm::enhancer::hold {
// HSE's own module: the one exporting HSE_Initialize (0.6 on), or, before 0.6 (0.5.3, still its release), the module
// named or versioned as HSE that subclasses a window itself (imports CallWindowProc and SetWindowLongPtr; its loader doesn't).
bool Subclasses(void* module);
void Attach(void* hseModule, bool exported);   // game thread, once: HSE's own module was found; `exported`: 0.6 on
void Tick();                    // game thread, each frame: takes HSE's place when it can, logs what changed
bool Ready();                   // HSMMod sits in HSE's place, and knows where HSE passes things on: Set works
void Set(bool on);              // on: HSE gets no keyboard, mouse or raw input (game thread)
bool On();                      // Ready and Set(true)
void MenuKey(int vk);           // HSE's menu key (match/hsm_online_rules.h HseMenuKey): a hold lets it through to HSE
void CloseAtStart(int vk);      // a locked game: HSE's menu, open, closed once with this key (its menu key, which no key sends)
}  // namespace hsm::enhancer::hold

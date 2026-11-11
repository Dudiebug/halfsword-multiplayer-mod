#include "hse_hold.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "log.h"
#include "session/hsm_hse_keys.h"

namespace hsm::enhancer::hold {
namespace {
// From HSE's source (2026-11, Mod/src/Gui.cpp): Gui::Setup sets its WndProc with SetWindowLongPtr and keeps what that
// returns as originalWndProc; WndProc hands everything it doesn't take on with CallWindowProc(originalWndProc, ...);
// Gui::Shutdown puts originalWndProc back only while GetWindowLongPtr still reads its own WndProc. A renderer reset runs
// Setup again. HSE calls these from its render thread and the window's thread, so every shared value is atomic.
using SetFn = LONG_PTR(WINAPI*)(HWND, int, LONG_PTR);
using GetFn = LONG_PTR(WINAPI*)(HWND, int);
using CallFn = LRESULT(WINAPI*)(WNDPROC, HWND, UINT, WPARAM, LPARAM);

std::atomic<uintptr_t> g_hseFrom{0}, g_hseTo{0};               // HSE's module image
std::atomic<void*> g_realSet[2], g_realGet[2], g_realCall[2];   // what HSE's import slots held, [1]: the ANSI ones
std::atomic<HWND> g_window{nullptr};                              // the window HSE subclasses
std::atomic<WNDPROC> g_hse{nullptr}, g_below{nullptr};            // HSE's WndProc, and what it passes things on to
std::atomic<bool> g_installed{false}, g_ansi{false};              // HSMMod's procedure is in HSE's place, and as which kind
std::atomic<bool> g_hold{false};
// An HSMMod text box is open (the chat, the lobby chat box, a text field of its menus): HSE gets no input meanwhile, but key
// and button releases (session/hsm_hse_keys.h). Apart from g_hold, which is a lobby's rule and feeds On() and the build card.
std::atomic<bool> g_chat{false}, g_share{true};
std::atomic<uint32_t> g_chatHeld{0};   // messages the chat held back from HSE since it opened (logged when it closes)
std::atomic<int> g_menuKey{0};        // let through a hold: HSE's menu can still close (and open, uselessly: nothing reaches it)
// A locked game's menu: open at the start (Gui::isVisible starts true, every version). Closed by HSMMod once, on the
// window's thread (a registered message), with the key the lock gave it. From 0.6 HSE keeps raw input (WM_INPUT) to
// itself while its menu is open, and passes it on while closed: that says whether it is open still (a version popup can
// keep it so). 0.5.3 passes WM_INPUT on either way: one close, and the menu counts as closed.
std::atomic<int> g_closeKey{0};
std::atomic<bool> g_menuShown{true}, g_inputTells{false};
std::atomic<bool> g_keptInput{false};   // HSE kept the latest raw input it was given: its menu is open (false until it has been given one)
std::atomic<UINT> g_closeMsg{0};
ULONGLONG g_nextClose = 0;
thread_local bool t_inputPassed = false;   // HSE passed this WM_INPUT on
thread_local bool t_closing = false;       // HSE handed its menu key by HSMMod: nothing it passes on goes further
bool g_attached = false, g_patched = false, g_loggedReady = false, g_loggedTaken = false;
ULONGLONG g_nextTry = 0;

bool InHse(const void* p) {
    const auto a = reinterpret_cast<uintptr_t>(p);
    return a && a >= g_hseFrom.load() && a < g_hseTo.load();
}

// Keyboard, mouse buttons, wheel and moves, and raw input: everything HSE reads a key from (KeybindManager, ImGui).
bool IsInput(UINT msg) {
    return msg == WM_INPUT || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST);
}

// A key or mouse button going up (the plain messages; a raw input packet is looked into).
bool IsRelease(UINT msg, LPARAM l) {
    switch (msg) {
        case WM_KEYUP: case WM_SYSKEYUP: case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: case WM_XBUTTONUP: return true;
        case WM_INPUT: {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return false;
            if (ri.header.dwType == RIM_TYPEKEYBOARD) return (ri.data.keyboard.Flags & RI_KEY_BREAK) != 0;
            if (ri.header.dwType == RIM_TYPEMOUSE) {
                const USHORT up = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
                const USHORT down = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN | RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN;
                return (ri.data.mouse.usButtonFlags & up) != 0 && (ri.data.mouse.usButtonFlags & down) == 0;
            }
            return false;
        }
        default: return false;
    }
}

template <bool A> LRESULT CallProc(WNDPROC p, HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!p) return A ? DefWindowProcA(h, m, w, l) : DefWindowProcW(h, m, w, l);
    return A ? CallWindowProcA(p, h, m, w, l) : CallWindowProcW(p, h, m, w, l);
}

// The window's procedure where HSE's was: HSE right behind it, or, held, input straight to what HSE passes things on to.
// Installed as the kind HSE's was (ANSI or Unicode), so each message reaches HSE as it did before.
// HSE's menu key pressed, straight to HSE: open, it closes (both versions take the press before anything else).
template <bool A> void CloseMenu(HWND h, WNDPROC hse) {
    const int key = g_closeKey.load();
    if (!hse || key <= 0 || !g_menuShown.load()) return;
    const LPARAM scan = LPARAM(MapVirtualKeyW(UINT(key), MAPVK_VK_TO_VSC) & 0xFF) << 16;
    t_closing = true;
    CallProc<A>(hse, h, WM_KEYDOWN, WPARAM(key), 1 | scan);
    t_closing = false;
    g_menuShown.store(false);
    g_keptInput.store(false);
    HSM_LOG("enhancer: HS Enhancer is locked and its menu was open: closed for you");
}

template <bool A> LRESULT CALLBACK HoldProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const WNDPROC hse = g_hse.load(std::memory_order_acquire), below = g_below.load(std::memory_order_acquire);
    if (m != 0 && m == g_closeMsg.load(std::memory_order_relaxed)) { CloseMenu<A>(h, hse); return 0; }
    const int key = g_menuKey.load(std::memory_order_relaxed);
    const bool menuKey = key > 0 && int(w) == key && (m == WM_KEYDOWN || m == WM_SYSKEYDOWN);
    if (below) {
        HseHolds holds;
        holds.lobby = g_hold.load(std::memory_order_relaxed);
        holds.typing = g_chat.load(std::memory_order_relaxed);
        const HseMsg kind = !IsInput(m) ? HseMsg::Other : IsRelease(m, l) ? HseMsg::Release : HseMsg::Input;
        const bool gets = HseGetsMessage(holds, kind, menuKey);
        if (!hse || !gets) {
            if (hse && holds.typing) {   // held by the chat: would a lobby hold alone have held it?
                HseHolds lobbyOnly = holds;
                lobbyOnly.typing = false;
                if (HseGetsMessage(lobbyOnly, kind, menuKey)) g_chatHeld.fetch_add(1, std::memory_order_relaxed);
            }
            return CallProc<A>(below, h, m, w, l);
        }
    }
    if (hse && m == WM_INPUT && g_inputTells.load(std::memory_order_relaxed)) {
        t_inputPassed = false;
        const LRESULT r = CallProc<A>(hse, h, m, w, l);
        g_menuShown.store(!t_inputPassed, std::memory_order_relaxed);
        g_keptInput.store(!t_inputPassed, std::memory_order_relaxed);
        return r;
    }
    return CallProc<A>(hse ? hse : below, h, m, w, l);
}
bool IsHoldProc(LONG_PTR p) { return p == reinterpret_cast<LONG_PTR>(&HoldProc<false>) || p == reinterpret_cast<LONG_PTR>(&HoldProc<true>); }

// HSE's SetWindowLongPtr. Its own WndProc going in: HoldProc goes in instead, and HSE is told what was there before
// (never HoldProc: a Setup without a Shutdown keeps what HSE passed on to). Anything else going in (Shutdown putting
// its original back): as asked, and HoldProc is gone with HSE.
template <bool A> LONG_PTR WINAPI SetHook(HWND h, int index, LONG_PTR v) {
    const auto real = reinterpret_cast<SetFn>(g_realSet[A].load());
    if (index != GWLP_WNDPROC) return real(h, index, v);
    if (InHse(reinterpret_cast<const void*>(v))) {
        g_hse.store(reinterpret_cast<WNDPROC>(v), std::memory_order_release);
        const LONG_PTR prev = real(h, index, reinterpret_cast<LONG_PTR>(&HoldProc<A>));
        if (!prev) { g_hse.store(nullptr); return 0; }
        if (!IsHoldProc(prev)) g_below.store(reinterpret_cast<WNDPROC>(prev), std::memory_order_release);
        g_window.store(h); g_ansi.store(A); g_installed.store(true);
        return reinterpret_cast<LONG_PTR>(g_below.load());
    }
    const LONG_PTR prev = real(h, index, v);
    if (h == g_window.load() && IsHoldProc(prev)) { g_installed.store(false); return reinterpret_cast<LONG_PTR>(g_hse.load()); }
    return prev;
}

// HSE's GetWindowLongPtr: HoldProc reads as HSE's own WndProc, so Shutdown still knows it is on top.
template <bool A> LONG_PTR WINAPI GetHook(HWND h, int index) {
    const LONG_PTR v = reinterpret_cast<GetFn>(g_realGet[A].load())(h, index);
    if (index == GWLP_WNDPROC && IsHoldProc(v)) if (const WNDPROC hse = g_hse.load()) return reinterpret_cast<LONG_PTR>(hse);
    return v;
}

// HSE's CallWindowProc: what it passes things on to, learned here when HSMMod took HSE's place after it had installed.
// Never back into HoldProc, which would loop.
template <bool A> LRESULT WINAPI CallHook(WNDPROC p, HWND h, UINT m, WPARAM w, LPARAM l) {
    if (t_closing) return 0;
    if (m == WM_INPUT) t_inputPassed = true;
    if (IsHoldProc(reinterpret_cast<LONG_PTR>(p))) p = g_below.load();
    else if (p && h == g_window.load() && !g_below.load()) g_below.store(p, std::memory_order_release);
    if (!g_window.load()) g_window.store(h);   // HSE passes this window's messages on: it subclasses it
    if (!p) return A ? DefWindowProcA(h, m, w, l) : DefWindowProcW(h, m, w, l);
    return reinterpret_cast<CallFn>(g_realCall[A].load())(p, h, m, w, l);
}

// Every slot in `mod`'s import table for user32's `name`, pointed at `hook`; `real` takes what the first held first.
int PatchImport(HMODULE mod, const char* name, void* hook, std::atomic<void*>& real) {
    auto* base = reinterpret_cast<uint8_t*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    const void* user32 = reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), name));
    int n = 0;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "user32.dll") != 0) continue;
        const bool named = d->OriginalFirstThunk != 0;   // without its name table, a slot is known by user32's address
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (named ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; slots->u1.Function; ++names, ++slots) {
            const bool match = named ? !IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal) &&
                                           std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, name) == 0
                                     : user32 && reinterpret_cast<const void*>(slots->u1.Function) == user32;
            if (!match) continue;
            if (!hook) { ++n; continue; }   // only counted
            void** at = reinterpret_cast<void**>(&slots->u1.Function);
            DWORD was = 0;
            if (!VirtualProtect(at, sizeof(void*), PAGE_READWRITE, &was)) continue;
            if (*at != hook) {
                if (!real.load()) real.store(*at);   // before the slot points here: a call through it needs it
                *at = hook;
            }
            VirtualProtect(at, sizeof(void*), was, &was);
            ++n;
        }
    }
    return n;
}

struct Found { HWND hwnd = nullptr; long area = 0; };
BOOL CALLBACK Biggest(HWND h, LPARAM p) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetClientRect(h, &r);
    const long area = long(r.right - r.left) * long(r.bottom - r.top);
    auto* f = reinterpret_cast<Found*>(p);
    if (area > f->area) { f->hwnd = h; f->area = area; }
    return TRUE;
}

// HSE installed before HSMMod found it: HoldProc goes in its place if HSE's WndProc is the window's procedure still.
// Anything on top of it, and HSE's keys can't be held this game (its lobbies need the restart that locks it).
void TakeOver() {
    HWND h = g_window.load();
    if (!h) { Found f; EnumWindows(Biggest, reinterpret_cast<LPARAM>(&f)); h = f.hwnd; }
    if (!h) return;
    const bool ansi = !IsWindowUnicode(h);
    const LONG_PTR top = ansi ? GetWindowLongPtrA(h, GWLP_WNDPROC) : GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!InHse(reinterpret_cast<const void*>(top))) {
        if (g_window.load() && !g_loggedTaken) { g_loggedTaken = true; HSM_LOG("enhancer: HS Enhancer's window hook sits under another one: its keys can't be held this game"); }
        return;
    }
    g_hse.store(reinterpret_cast<WNDPROC>(top), std::memory_order_release);
    const LONG_PTR hold = ansi ? reinterpret_cast<LONG_PTR>(&HoldProc<true>) : reinterpret_cast<LONG_PTR>(&HoldProc<false>);
    const LONG_PTR prev = ansi ? SetWindowLongPtrA(h, GWLP_WNDPROC, hold) : SetWindowLongPtrW(h, GWLP_WNDPROC, hold);
    if (prev != top) {   // changed in between: put back what was there, and try again later
        if (prev) { if (ansi) SetWindowLongPtrA(h, GWLP_WNDPROC, prev); else SetWindowLongPtrW(h, GWLP_WNDPROC, prev); }
        return;
    }
    g_window.store(h); g_ansi.store(ansi); g_installed.store(true);
}
}  // namespace

bool Subclasses(void* module) {
    const auto mod = static_cast<HMODULE>(module);
    std::atomic<void*> none{nullptr};
    if (!mod) return false;
    return PatchImport(mod, "CallWindowProcW", nullptr, none) + PatchImport(mod, "CallWindowProcA", nullptr, none) > 0 &&
           PatchImport(mod, "SetWindowLongPtrW", nullptr, none) + PatchImport(mod, "SetWindowLongPtrA", nullptr, none) > 0;
}

void Attach(void* hseModule, bool exported) {
    if (g_attached || !hseModule) return;
    g_attached = true;
    g_inputTells.store(exported);
    const auto mod = static_cast<HMODULE>(hseModule);
    const auto* base = reinterpret_cast<const uint8_t*>(mod);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    g_hseFrom.store(reinterpret_cast<uintptr_t>(base));
    g_hseTo.store(reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage);
    // CallWindowProc first: the moment SetWindowLongPtr goes through HSMMod, HSE's WndProc may run.
    const int calls = PatchImport(mod, "CallWindowProcW", reinterpret_cast<void*>(&CallHook<false>), g_realCall[0]) +
                      PatchImport(mod, "CallWindowProcA", reinterpret_cast<void*>(&CallHook<true>), g_realCall[1]);
    const int gets = PatchImport(mod, "GetWindowLongPtrW", reinterpret_cast<void*>(&GetHook<false>), g_realGet[0]) +
                     PatchImport(mod, "GetWindowLongPtrA", reinterpret_cast<void*>(&GetHook<true>), g_realGet[1]);
    const int sets = PatchImport(mod, "SetWindowLongPtrW", reinterpret_cast<void*>(&SetHook<false>), g_realSet[0]) +
                     PatchImport(mod, "SetWindowLongPtrA", reinterpret_cast<void*>(&SetHook<true>), g_realSet[1]);
    g_patched = calls > 0 && sets > 0;
    HSM_LOG("enhancer: HS Enhancer's window calls: %d CallWindowProc, %d GetWindowLongPtr, %d SetWindowLongPtr%s", calls, gets, sets,
            g_patched ? "" : ": not the ones it subclasses with, so its keys can't be held this game");
}

void Tick() {
    if (!g_patched) return;
    if (g_closeKey.load() > 0 && Ready() && g_menuShown.load()) {   // once a second at most, in case it is lost
        const ULONGLONG now = GetTickCount64();
        if (now >= g_nextClose) {
            g_nextClose = now + 1000;
            if (!g_closeMsg.load()) g_closeMsg.store(RegisterWindowMessageW(L"HSMMod.CloseHseMenu"));
            if (const UINT msg = g_closeMsg.load()) PostMessageW(g_window.load(), msg, 0, 0);
        }
    }
    if (!g_installed.load()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= g_nextTry) { g_nextTry = now + 1000; TakeOver(); }
    }
    const bool ready = Ready();
    if (ready != g_loggedReady) {
        g_loggedReady = ready;
        if (ready) HSM_LOG("enhancer: HSMMod sits in HS Enhancer's place in the game window (%s): lobbies that block it hold its keys, no restart",
                           g_ansi.load() ? "ANSI" : "Unicode");
        else HSM_LOG("enhancer: HS Enhancer's window hook is gone: its keys can't be held until it is back");
    }
}

bool Ready() { return g_installed.load() && g_hse.load() && g_below.load(); }
void Set(bool on) { g_hold.store(on, std::memory_order_relaxed); }

void Share(bool on) { g_share.store(on, std::memory_order_relaxed); }

void Typing(bool typing) {
    if (!g_attached) return;
    const bool on = typing && g_share.load(std::memory_order_relaxed);
    if (on == g_chat.load(std::memory_order_relaxed)) return;
    g_chat.store(on, std::memory_order_relaxed);
    if (on) {
        g_chatHeld.store(0, std::memory_order_relaxed);
        if (Ready()) HSM_LOG("enhancer: an HSMMod text box is open: HS Enhancer gets no keyboard or mouse input until it closes");
        else HSM_LOG("enhancer: an HSMMod text box is open, but HSMMod is not in HS Enhancer's window hook: its keys can't be held (typing may reach it)");
    } else if (Ready()) {
        HSM_LOG("enhancer: the HSMMod text box closed: HS Enhancer gets input again (%u message(s) held back from it)",
                unsigned(g_chatHeld.load(std::memory_order_relaxed)));
    }
}

bool MenuOpen() {
    HseMenuFacts f;
    f.share = g_share.load(std::memory_order_relaxed);
    f.ready = Ready();
    f.reports = g_inputTells.load(std::memory_order_relaxed);
    f.keptInput = g_keptInput.load(std::memory_order_relaxed);
    f.held = g_hold.load(std::memory_order_relaxed) || g_chat.load(std::memory_order_relaxed);
    return HseMenuHasKeys(f);
}
void MenuKey(int vk) { g_menuKey.store(vk); }
void CloseAtStart(int vk) { g_closeKey.store(vk); }
bool On() { return g_hold.load(std::memory_order_relaxed) && Ready(); }
}  // namespace hsm::enhancer::hold

#include "gameplay/fetch_link.h"
#include "hse_hold.h"
#include "log.h"
#include <windows.h>
#include <string>

namespace hsm::game::fetchlink {
namespace {

uint8_t* g_page = nullptr;    // the view, kept (with its handle) for the life of the process
bool g_tried = false;
interop::LinkWatch g_watch;   // Fetch's half
interop::LinkNotes g_notes;
bool g_held = false;
bool g_hseMenu = false;        // HS Enhancer's menu is open and has the keyboard (hse_hold.h MenuOpen)
int g_replaying = 0;          // Replaying guards alive

// Either mod may make the page; the other opens the same one (CreateFileMappingW returns it with ERROR_ALREADY_EXISTS).
uint8_t* Page() {
    if (g_tried) return g_page;
    g_tried = true;
    const DWORD pid = GetCurrentProcessId();
    const std::wstring name = interop::LinkName(pid);
    SetLastError(0);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, interop::kLinkBytes, name.c_str());
    if (!h) { HSM_LOG("fetch link: could not make the link page (error %lu); carrying on without it", GetLastError()); return nullptr; }
    const bool opened = GetLastError() == ERROR_ALREADY_EXISTS;
    void* view = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, interop::kLinkBytes);
    if (!view) {
        HSM_LOG("fetch link: could not map the link page (error %lu); carrying on without it", GetLastError());
        CloseHandle(h);
        return nullptr;
    }
    g_page = static_cast<uint8_t*>(view);
    HSM_LOG("fetch link: page Local\\HSMLink-%lu mapped (%s)", static_cast<unsigned long>(pid), opened ? "opened, Fetch made it" : "made here");
    return g_page;
}

}  // namespace

void Tick(const interop::HsmFacts& facts, uint32_t dtMs) {
    // Before the page: HS Enhancer's menu holds the keys with or without Fetch (#1290).
    const bool hse = enhancer::hold::MenuOpen();
    if (hse != g_hseMenu) {
        g_hseMenu = hse;
        if (hse) HSM_LOG("enhancer: HS Enhancer's menu is open: HSMMod's keys held back until it closes");
        else HSM_LOG("enhancer: HS Enhancer's menu closed: HSMMod's keys back");
    }
    uint8_t* page = Page();
    if (!page) return;
    const interop::HsmState mine = interop::HsmLinkState(facts);
    interop::WriteHsm(page, mine);
    const bool live = g_watch.Tick(interop::FetchBeat(page), dtMs);
    g_held = interop::HsmKeysBlocked(live, interop::ReadFetch(page));
    const interop::LinkNews n = g_notes.Tick(live, g_held);
    if (n.arrived) HSM_LOG("fetch link: Fetch is here");
    if (n.held) HSM_LOG("fetch link: Fetch's menu has the keyboard: HSMMod's keys held back until it closes%s", mine.typing ? " (the chat stays open)" : "");
    if (n.released) HSM_LOG("fetch link: Fetch's menu closed: HSMMod's keys back");
    if (n.gone) HSM_LOG("fetch link: Fetch is gone (its half stopped for %u ms)", unsigned(interop::kLinkStaleMs));
}

bool KeysHeld() { return g_held || g_hseMenu; }

Replaying::Replaying() {
    if (g_replaying++ != 0) return;
    interop::SetHsmFlag(g_page, interop::kHsmReplayingInput, true);
    static bool logged = false;
    if (!logged && g_page && g_watch.live()) {
        logged = true;
        HSM_LOG("fetch link: playing held-back keys again with the replay flag up, so Fetch does not record them twice (logged once)");
    }
}
Replaying::~Replaying() {
    if (--g_replaying == 0) interop::SetHsmFlag(g_page, interop::kHsmReplayingInput, false);
}

}  // namespace hsm::game::fetchlink

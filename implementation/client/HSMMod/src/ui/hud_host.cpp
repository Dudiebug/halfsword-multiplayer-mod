#include "ui/hud_host.h"
#include "ui/umg.h"
#include "log.h"
#include <windows.h>

namespace hsm::ui {
using namespace hsm::ue;

namespace {
constexpr uint64_t kScanEveryMs = 1000;   // a scan for the HUD walks the object table: at most once a second
}

HudHost::Step HudHost::Follow(bool wanted, bool built, uint32_t liveEveryMs) {
    if (!cls_) {   // not loaded yet (a menu): four full name scans, so at most once per kScanEveryMs
        const uint64_t now = GetTickCount64();
        if (now < nextScan_) return Step::None;
        if (!(cls_ = FindClass("UI_HUD_C"))) { nextScan_ = now + kScanEveryMs; return Step::None; }
    }
    bool changed = false;
    if (!obj_ || ObjectAt(index_) != obj_ || ClassOf(obj_) != cls_) {
        if (obj_) { obj_ = nullptr; index_ = -1; nextLive_ = 0; changed = true; }   // the one we sat on is gone, and our widgets with it
        const uint64_t now = GetTickCount64();
        if (!wanted || now < nextScan_) return changed ? Step::Changed : Step::None;
        nextScan_ = now + kScanEveryMs;
        obj_ = OnScreen(FindInstances("UI_HUD_C"));
        if (!obj_) return changed ? Step::Changed : Step::None;
        index_ = ObjectIndex(obj_);
        return Step::Changed;
    }
    // The game swaps UI_HUD on a respawn or a new round: the old one leaves the screen but lives until garbage
    // collection (about a minute), and our widgets went with it. Move to the one on screen now.
    if (built) {
        const uint64_t now = GetTickCount64();
        if (now >= nextLive_) {
            nextLive_ = now + liveEveryMs;
            if (!InViewport(obj_))
                if (UObject* on = OnScreen(FindInstances("UI_HUD_C")); on && on != obj_) {
                    obj_ = on; index_ = ObjectIndex(on);
                    HSM_LOG("%s: UI_HUD replaced by the game; moving to the new one", name_);
                    return Step::Changed;
                }
        }
    }
    return Step::Same;
}

}  // namespace hsm::ui

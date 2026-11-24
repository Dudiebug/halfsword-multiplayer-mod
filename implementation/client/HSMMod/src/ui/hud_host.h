// The UI_HUD that the mod's overlay widgets (the match HUD, the notices, the chat, the build card, the pause panel) sit on.
// The game swaps UI_HUD on a respawn or a new round: the old one leaves the screen but lives until garbage collection
// (about a minute), and every widget made on it went with it. So each overlay finds the HUD once a second, follows it to
// the one on screen, and builds its widgets again when it changes. One helper does that for all of them. Game thread only.
#pragma once
#include "ue/engine.h"
#include <cstdint>

namespace hsm::ui {

class HudHost {
public:
    // `name` is the overlay's name in the log line written when the game swaps the HUD ("hud", "chat", ...).
    explicit HudHost(const char* name) : name_(name) {}

    enum class Step : uint8_t {
        None,      // there is no UI_HUD to sit on (yet)
        Same,      // sitting on the same one as the last call
        Changed,   // a different one from the last call, or none now where there was one: what the caller built on the old one is gone
    };

    // One call per tick. `wanted`: the overlay is wanted now, so a UI_HUD may be looked for (the scan walks the object table, so
    // it runs at most once a second, and not at all while nothing needs it). `built`: the overlay's widgets stand on Obj(); only
    // then is it asked whether the game moved on to another UI_HUD. `liveEveryMs`: how often that is asked.
    // On Changed the caller drops its widgets (they are on a dead tree or not made) and builds again on Obj() if there is one.
    Step Follow(bool wanted, bool built, uint32_t liveEveryMs = 1000);

    ue::UObject* Obj() const { return obj_; }
    int32_t Index() const { return index_; }
    // The held UI_HUD is still the live object in its slot.
    bool Alive() const { return obj_ && ue::ObjectAt(index_) == obj_; }

private:
    const char* name_;
    ue::UClass* cls_ = nullptr;
    ue::UObject* obj_ = nullptr;
    int32_t index_ = -1;
    uint64_t nextScan_ = 0, nextLive_ = 0;
};

}  // namespace hsm::ui

// The link page HSMMod shares with Fetch (dsound.dll, a fight recorder), so both can run in one game: a small named file
// mapping per game process (shared/interop/hsm_mod_link.h is the contract; shared/interop/hsm_link_hsm.h what this side
// writes). HSMMod publishes its half once per game tick, in solo play and the menus too, and reads Fetch's. While Fetch's
// menu has the keyboard HSMMod's own hotkeys do nothing (KeysHeld); with no live Fetch half every answer here is as if
// the link were not there. A page that cannot be mapped is logged once and the mod carries on without it. Game thread only.
#pragma once
#include "interop/hsm_link_hsm.h"
#include <cstdint>

namespace hsm::game::fetchlink {

// Once per game tick, before any of the mod's keys are read: maps the page the first time, writes HSMMod's half from
// `facts` and reads Fetch's. `dtMs`: the tick's length.
void Tick(const interop::HsmFacts& facts, uint32_t dtMs);

// Fetch's menu has the keyboard (its half live, its menu open or a name being typed in it): the chat key, the class bite,
// the build card, push to talk, G, Tab, the spectate keys and the rest are Fetch's until it closes. Always false
// without a live Fetch.
bool KeysHeld();

// While one lives, HSMMod is playing held-back keys of this player's again (host input delay, the match input delay, the
// catch-up after a hand-off): the replay flag is set on the page, so Fetch's recorder does not take them as new presses.
class Replaying {
public:
    Replaying();
    ~Replaying();
    Replaying(const Replaying&) = delete;
    Replaying& operator=(const Replaying&) = delete;
};

}  // namespace hsm::game::fetchlink

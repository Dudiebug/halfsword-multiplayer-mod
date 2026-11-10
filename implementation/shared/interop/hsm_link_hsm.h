// HSMMod's side of the link page it shares with Fetch (interop/hsm_mod_link.h, the contract): what it writes in its half
// from what the session knows, and which of the link's changes go in the log. Pure; the mod maps the page and calls these
// once per game tick (gameplay/fetch_link.cpp).
#pragma once
#include "interop/hsm_mod_link.h"

namespace hsm::interop {

// What the session knows this tick.
struct HsmFacts {
    bool inLobby = false;       // in a Steam lobby (joined or hosting), the menu or a match
    bool joined = false;        // a session: hosting, or a client the host welcomed
    bool isHost = false;
    bool hostPhysics = false;   // the session's sync mode is host physics (sync_mode=host)
    bool typing = false;        // the match chat, or one of the mod's text boxes, has the keyboard
};

// HSMMod's half. The replay flag is not here: it is set in place around each replay (SetHsmFlag), and every full write
// clears it. Outside a session there is no sync mode and nobody else plays this machine's fighter.
inline HsmState HsmLinkState(const HsmFacts& f) {
    HsmState s;
    s.inSession = f.inLobby || f.joined;
    s.typing = f.typing;
    s.hostPlaysMine = s.inSession && f.hostPhysics && !f.isHost;
    s.syncMode = !s.inSession ? SyncMode::None : f.hostPhysics ? SyncMode::Host : SyncMode::Owner;
    return s;
}

// The link's news since the last tick, one log line each: Fetch came or went, and its menu took or gave back the keyboard.
struct LinkNews {
    bool arrived = false, gone = false, held = false, released = false;
};
class LinkNotes {
public:
    // `fetchLive`: LinkWatch's answer; `keysHeld`: HsmKeysBlocked's.
    LinkNews Tick(bool fetchLive, bool keysHeld) {
        LinkNews n;
        n.arrived = fetchLive && !live_;
        n.gone = !fetchLive && live_;
        n.held = keysHeld && !held_;
        n.released = !keysHeld && held_;
        live_ = fetchLive;
        held_ = keysHeld;
        return n;
    }

private:
    bool live_ = false, held_ = false;
};

}  // namespace hsm::interop

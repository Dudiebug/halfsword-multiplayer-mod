// Lobby chat (the multiplayer page's chat box), game-independent: the per-sender rate limit for lines that arrive and
// the colour a sender's name gets. The text travels as a Steam lobby chat message (every member is told of it, the
// sender included), so it needs no session transport and no slot: a member the host has not welcomed yet can read and
// write. Lines are sanitized as the in-match chat's are (hsm_chat.h).
#pragma once
#include "hsm_chat.h"
#include "match/hsm_hud_style.h"
#include "match/hsm_lobby_scene.h"
#include <cstdint>
#include <map>
#include <string>

namespace hsm {

// The same token bucket the in-match chat uses (hsm_chat.h), one per sender id, so a member flooding the lobby cannot
// flood the others' pages. Ids are not remembered after Forget (a member who left).
class LobbyChatGate {
public:
    bool Allow(uint64_t sender, uint32_t nowMs) { return senders_[sender].Allow(nowMs); }
    void Forget(uint64_t sender) { senders_.erase(sender); }
    void Clear() { senders_.clear(); }
    size_t tracked() const { return senders_.size(); }
private:
    std::map<uint64_t, ChatRateLimiter> senders_;
};

// A sender's name colour: their team's ink in the team formats (the host's move over their own pick, as the roster
// shows), else iron, as ChatNameInk does in a match.
inline Rgba LobbyChatInk(Format f, int team, int hostOverride) {
    if (lobby_detail::ValidTeam(hostOverride, f)) return style::TeamInk(hostOverride);
    if (lobby_detail::ValidTeam(team, f)) return style::TeamInk(team);
    return style::kIron;
}

}  // namespace hsm

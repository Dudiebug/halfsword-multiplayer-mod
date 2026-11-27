// A machine left out of its own lobby (public #4296), pure part. Steam drops a member that loses its connection to the
// Steam servers. When the connection comes back the member is no longer in the lobby, but the mod still thought so: a
// host lost its internet for 20 s, the others carried on under the new host Steam named, and the old host went on
// "hosting" a lobby nobody else could see, its lobby data writes failing every second, until the player left by hand.
// LobbyStrand notices that state and answers Rejoin: leave and join the same lobby again, as a client of whoever owns it
// now (a host's reserved slots let a client back into the match it dropped out of).
#pragma once
#include <cstdint>

namespace hsm {

constexpr uint32_t kStrandConfirmMs = 3000;    // left out this long, with Steam connected, before we rejoin
constexpr uint32_t kStrandRetryMs = 60000;     // one rejoin of a lobby per this long: no loop if Steam keeps refusing us

enum class StrandStep : uint8_t {
    Member,    // Steam lists us in the lobby, with an owner
    Waiting,   // left out, or Steam is disconnected: not sure yet
    Rejoin,    // leave and join this lobby again now (returned once)
    GaveUp,    // left out for good: rejoined this lobby less than kStrandRetryMs ago (returned once)
};

class LobbyStrand {
public:
    // Called while the mod thinks it is in `lobby`. listed: Steam's member list of it has our id; owner: Steam's owner of it
    // (0 when Steam names none, as it does for a lobby we are not in); steamUp: connected to the Steam servers (no
    // disconnect since the last connect). After Rejoin or GaveUp it answers Waiting until we are a member again.
    StrandStep Tick(uint64_t lobby, bool listed, uint64_t owner, bool steamUp, uint32_t nowMs) {
        if (lobby != lobby_) { lobby_ = lobby; out_ = false; acted_ = false; }
        if (listed && owner) { out_ = false; acted_ = false; return StrandStep::Member; }
        if (!steamUp || acted_) { out_ = false; return StrandStep::Waiting; }   // the wait starts over once Steam is back
        if (!out_) { out_ = true; since_ = nowMs; return StrandStep::Waiting; }
        if (uint32_t(nowMs - since_) < kStrandConfirmMs) return StrandStep::Waiting;
        acted_ = true; out_ = false;
        if (rejoined_ && rejoinedLobby_ == lobby && uint32_t(nowMs - rejoinedAt_) < kStrandRetryMs) return StrandStep::GaveUp;
        rejoined_ = true; rejoinedLobby_ = lobby; rejoinedAt_ = nowMs;
        return StrandStep::Rejoin;
    }

private:
    uint64_t lobby_ = 0, rejoinedLobby_ = 0;
    uint32_t since_ = 0, rejoinedAt_ = 0;
    bool out_ = false, acted_ = false, rejoined_ = false;
};

}  // namespace hsm

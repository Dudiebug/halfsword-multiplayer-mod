// The Abyss in a session (sync audit 36, 37; session/hsm_abyss.h). Every machine, every frame on the Abyss while in a
// session: the level's own enemy spawner is held shut (the host spawns every zombie), and the floor is kept under every
// fighter that is not player 0 here. Game thread only.
#pragma once
#include "gameplay/adapter.h"
#include "session/hsm_abyss.h"
#include <map>
#include <string>
#include <vector>

namespace hsm::game {

class AbyssCoop {
public:
    // me: player 0 here, or null. others: every other fighter this machine shows (stand-ins, the host's bots).
    void Tick(Adapter& adapter, double nowMs, ue::UObject* me, const std::vector<ue::UObject*>& others);
    void Clear(Adapter& adapter);   // session over: our tiles go; a level change: they went with the level (pass the new one)
    std::string HealthLine() const;
private:
    struct Ref { ue::UObject* obj = nullptr; int32_t index = -1; };
    bool Live(const Ref& r) const;
    Ref level_;
    std::map<TileCell, Ref> ours_;
    std::map<TileCell, double> localSince_;   // player 0's 3x3 here, and since when each cell is in it
    double nextFloorMs_ = 0;
    uint64_t pinned_ = 0, added_ = 0, dropped_ = 0, addFailed_ = 0;
    bool loggedPin_ = false;
};

}  // namespace hsm::game

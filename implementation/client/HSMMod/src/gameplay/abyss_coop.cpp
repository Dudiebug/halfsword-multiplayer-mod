#include "gameplay/abyss_coop.h"
#include "log.h"
#include <cstdio>
#include <set>

namespace hsm::game {

using namespace hsm::ue;

namespace {
constexpr const char* kLevelClass = "Abyss_Map_Open_EA_C";
constexpr const char* kTileClass = "BP_Floor_Tile_10x10x_C";   // the level's Spawn Tile: no script, the same tile everywhere
constexpr double kFloorEveryMs = 250;   // a fighter crosses at most a few metres of its 3x3 in that time
// The level fills player 0's 3x3 one cell a frame, then waits 1 s: a cell in it this long has the level's tile. Counted
// so, never looked up (an object scan costs milliseconds); a leftover of the level's outside it just lies under ours.
constexpr double kLevelSettleMs = 2000;
constexpr uint64_t kLogFirst = 10;
}  // namespace

bool AbyssCoop::Live(const Ref& r) const { return r.obj && r.index >= 0 && ObjectAt(r.index) == r.obj && !IsGarbage(r.obj); }

void AbyssCoop::Tick(Adapter& adapter, double nowMs, UObject* me, const std::vector<UObject*>& others) {
    if (!Live(level_)) {
        level_ = {};
        std::vector<UObject*> levels = FindInstances(kLevelClass);
        if (levels.empty()) return;
        level_ = {levels[0], ObjectIndex(levels[0])};
    }
    // The spawner's check (ReceiveTick): player 0 at least 250 + 250 x enemies from Last Spawn Location, both flat. Pinned
    // to player 0 each frame, it never passes; with no player 0 the level reads the origin, and so does this.
    if (FVector* last = Field<FVector>(level_.obj, "Last Spawn Location")) {
        FVector at = me ? adapter.ActorLocation(me) : FVector{};
        *last = FVector{at.x, at.y, 0};
        ++pinned_;
        if (!loggedPin_) { loggedPin_ = true; HSM_LOG("abyss: the level's own enemy spawner is held shut for this session (the host spawns every zombie)"); }
    }
    if (nowMs < nextFloorMs_) return;
    nextFloorMs_ = nowMs + kFloorEveryMs;
    std::vector<FloorPoint> needs;
    for (UObject* w : others) if (w && w != me) { FVector v = adapter.ActorLocation(w); needs.push_back({v.x, v.y}); }
    for (auto it = ours_.begin(); it != ours_.end();) it = Live(it->second) ? std::next(it) : ours_.erase(it);
    std::optional<FloorPoint> self;
    std::set<TileCell> local;
    if (me) {
        FVector v = adapter.ActorLocation(me);
        self = FloorPoint{v.x, v.y};
        const TileCell c = AbyssCellOf(v.x, v.y);
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) local.insert({c.x + dx, c.y + dy});
    }
    for (auto it = localSince_.begin(); it != localSince_.end();) it = local.count(it->first) ? std::next(it) : localSince_.erase(it);
    std::set<TileCell> level, ours;
    for (TileCell c : local) if (nowMs - localSince_.emplace(c, nowMs).first->second >= kLevelSettleMs) level.insert(c);
    if (needs.empty() && ours_.empty()) return;
    for (auto& [c, r] : ours_) ours.insert(c);
    const FloorPlan plan = PlanAbyssFloor(self, needs, level, ours);
    for (TileCell c : plan.drop) {
        auto it = ours_.find(c);
        if (it == ours_.end()) continue;
        adapter.DestroyActor(it->second.obj);
        ours_.erase(it);
        ++dropped_;
    }
    UClass* cls = plan.add.empty() ? nullptr : FindClass(kTileClass);
    for (TileCell c : plan.add) {
        const FloorPoint o = AbyssCellCenter(c);
        UObject* t = cls ? adapter.SpawnActorAt(cls, FVector{o.x, o.y, 0}, FRotator{}) : nullptr;
        if (!t) { if (++addFailed_ <= kLogFirst) HSM_LOG("abyss: no floor tile could be placed at cell %d,%d", c.x, c.y); continue; }
        ours_[c] = {t, ObjectIndex(t)};
        if (++added_ <= kLogFirst) HSM_LOG("abyss: floor tile placed at cell %d,%d under a remote fighter (%zu of ours)", c.x, c.y, ours_.size());
    }
}

void AbyssCoop::Clear(Adapter& adapter) {
    for (auto& [c, r] : ours_) if (Live(r)) adapter.DestroyActor(r.obj);
    ours_.clear();
    level_ = {};
    localSince_.clear();
    nextFloorMs_ = 0;
    loggedPin_ = false;
}

std::string AbyssCoop::HealthLine() const {
    char b[160];
    std::snprintf(b, sizeof b, "abyss: spawner pinned %llu frame(s); floor tiles of ours %zu (placed %llu, dropped %llu, failed %llu)",
                  (unsigned long long)pinned_, ours_.size(), (unsigned long long)added_, (unsigned long long)dropped_, (unsigned long long)addFailed_);
    return b;
}

}  // namespace hsm::game

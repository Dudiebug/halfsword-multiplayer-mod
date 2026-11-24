#include "gameplay/world_context.h"
#include "log.h"
#include "session/hsm_world_context.h"

namespace hsm::game::worldctx {
using namespace hsm::ue;
namespace {
UObject* g_live = nullptr;
int32_t g_liveIndex = -1;
// The last stale context logged, and its outcome: logged once until either changes (a paused load asks every 50 ms).
const void* g_loggedWanted = nullptr;
WorldContextPick g_loggedPick = WorldContextPick::Wanted;
uint32_t g_redirected = 0, g_skipped = 0;
}  // namespace

void NoteTicking(UObject* mode) {
    g_live = mode;
    g_liveIndex = mode ? ObjectIndex(mode) : -1;
}

UObject* For(UObject* wanted, std::source_location at) {
    const bool inSlot = g_live && g_liveIndex >= 0 && ObjectAt(g_liveIndex) == g_live && !IsGarbage(g_live);
    const WorldContextPick pick = PickWorldContext(wanted, g_live, inSlot);
    if (pick == WorldContextPick::Wanted) return wanted;
    if (pick == WorldContextPick::Live) ++g_redirected;
    else if (wanted) ++g_skipped;
    if (wanted && (wanted != g_loggedWanted || pick != g_loggedPick)) {
        g_loggedWanted = wanted; g_loggedPick = pick;
        if (pick == WorldContextPick::Live)
            HSM_LOG("world context: %s held an earlier level's game mode; it runs with the current one (redirected %u, skipped %u)",
                    at.function_name(), g_redirected, g_skipped);
        else
            HSM_LOG("world context: %s skipped: no live game mode (the level is loading; redirected %u, skipped %u)",
                    at.function_name(), g_redirected, g_skipped);
    }
    return static_cast<UObject*>(const_cast<void*>(WorldContextFor(pick, wanted, g_live)));
}

}  // namespace hsm::game::worldctx

// The world context the mod hands UGameplayStatics' functions (public #855; the rule is session/hsm_world_context.h).
// Session::Tick records the game mode whose ReceiveTick runs before anything else; every mod call that passes a cached
// game mode as WorldContextObject (game::OpenLevel, the adapter's calls) asks For() first. Game thread only.
#pragma once
#include "ue/engine.h"
#include <source_location>

namespace hsm::game::worldctx {

// The game mode whose ReceiveTick runs now: alive by definition. Recorded with its object-array slot, so it can be
// checked later (a paused game, a widget's callback) without reading it.
void NoteTicking(ue::UObject* mode);
// The context to hand the engine for `wanted` (a cached game mode): `wanted` when it is the live mode; the live mode in
// its place when `wanted` is an earlier level's (logged); nullptr when no mode is known alive (logged), which the engine
// treats as "no world" and does nothing with.
ue::UObject* For(ue::UObject* wanted, std::source_location at = std::source_location::current());

}  // namespace hsm::game::worldctx

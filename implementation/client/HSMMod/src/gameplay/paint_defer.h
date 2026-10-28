// The armour vertex-paint crash (T5, #5-#42): deferred destroy of a swapped-out armour piece. Remove Armor / Set Up Armor
// (Willie_BP.cpp:5208-5219) starts a GetAllVertexColorsOnly task per worn piece, and the callback event
// BndEvt__Willie_BP_BPC_RuntimeVertexPaintAndDetectionComponent_K2Node_ComponentBoundEvent_0_VertexColorGetAllVertexColorsOnly
// (:8002-8015) destroys the piece the moment its own task is done, while blood and wound tasks queued on the same skinned
// mesh still read it on worker threads. K2_DestroyComponent is native (never through the script VM), so this hooks the
// Blueprint callback instead. When another task still references the piece, the callback runs with a decoy component in
// its result struct: it records the piece's vertex paint (Map_Add keyed by the mesh asset) and destroys the decoy as
// always, and the real piece, already hidden, without collision and out of "Worn Armor", is destroyed here once no task
// references it (combat/hsm_paintdefer.h) or after 3 s. Runs for every Willie the game builds, in single player too, and
// fails open: any step that cannot be verified lets the game destroy the piece as before. Game thread only.
#pragma once
#include <string>

namespace hsm::game {

// Hooks the callback. Needs the script VM hook (InstallScriptHook) to be up; rules resolve with each level load
// (ue::ResolveScriptHooks). False when it could not be registered.
bool InstallPaintDefer();
// Every tick, in or out of a session: destroys parked pieces that are free, gone or late. Costs nothing while none is parked.
void TickPaintDefer();
// A new level: parked pieces went with the old world.
void PaintDeferLevelChanged();
// Counters since the previous call, one line for the health log; empty when nothing happened.
std::string PaintDeferHealthLine();

}  // namespace hsm::game

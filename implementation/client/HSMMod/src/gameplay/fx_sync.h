// Blood and wound paint mirrored from each fighter's owner (owner sync, protocol rev 10, combat/hsm_fx.h).
//
// A blow that lands on a stand-in paints nothing there: the stand-in is Invulnerable, and Get Damage (blood, wound
// paint) and Box Vertex Paint (a stuck stab's wound) both start with !Invulnerable. The fighter's owner is the one
// machine where the damage really happens, so it watches its own fighters' "Get Damage", "Spawn Blood" and
// "Box Vertex Paint" (script-VM observers: they also see the victim's replay of a hit claim, which runs under
// ScriptHookBypass) and reports each as a PaintFx in the bone's frame. Every other machine replays it on its stand-in:
//   Blood  its own "Spawn Blood" with the owner's gate state and Bleeding written on the stand-in first;
//   Wound  its own "Box Vertex Paint" over a private box, Invulnerable lowered for that one call and restored.
// Game thread only.
#pragma once
#include "combat/hsm_fx.h"
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "ue/engine.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace hsm::ue { class ScriptCall; }

namespace hsm::game {

class FxSync {
public:
    struct Env {
        std::function<uint8_t(const ue::UObject*)> ownedEntity;     // the entity of a fighter simulated here, 0 if none
        std::function<void(const combat::PaintFxMsg&)> send;        // a report made here (the session routes it)
    };
    // Installs the observers (owner "fxsync"; they resolve on every level load). False without the script VM hook.
    bool Start(Adapter& adapter, StandIns& standIns, Env env);
    void Stop();
    void Apply(const combat::PaintFxMsg& m);                         // a fighter simulated elsewhere: shown on its stand-in
    void Forget(uint8_t entity) { limiter_.Forget(entity); }
    std::string HealthLine();                                        // since the previous call

private:
    struct Box { ue::UObject* actor = nullptr; int32_t actorIndex = -1; ue::UObject* box = nullptr; int32_t boxIndex = -1; };
    struct Counters { uint64_t blood = 0, wounds = 0, limited = 0, noBone = 0, noName = 0, applied = 0, skipped = 0, failed = 0; };
    void OnGetDamage(ue::ScriptCall& c);
    void OnSpawnBlood(ue::ScriptCall& c);
    void OnBoxVertexPaint(ue::ScriptCall& c);
    void SendWound(uint8_t entity, ue::UObject* willie, ue::FName bone, const combat::Xform& boxWorld, const sync::V3& extent);
    bool Send(combat::PaintFxMsg& m, ue::UObject* willie, ue::FName bone);
    bool BoneXform(ue::UObject* mesh, ue::FName bone, combat::Xform& out) const;
    void ApplyBlood(const combat::PaintFxMsg& m, ue::UObject* standIn, ue::UObject* mesh, const combat::Xform& boneWorld);
    void ApplyWound(const combat::PaintFxMsg& m, ue::UObject* standIn, ue::UObject* mesh, const combat::Xform& boneWorld);
    void Count(bool ok, const combat::PaintFxMsg& m);
    ue::UObject* PaintBox(uint8_t entity, ue::UObject* standIn);

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    Env env_;
    ue::UClass* boxClass_ = nullptr;
    bool boxFailLogged_ = false;
    uint16_t seq_ = 0;
    combat::FxLimiter limiter_;
    std::map<int, Box> boxes_;                                       // stand-in entity -> its private paint box
    Counters n_, reported_;
};

}  // namespace hsm::game

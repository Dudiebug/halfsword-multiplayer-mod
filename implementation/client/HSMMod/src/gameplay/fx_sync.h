// Blood and wound paint mirrored from each fighter's owner (owner sync, protocol rev 10, combat/hsm_fx.h).
//
// A blow that lands on a stand-in paints nothing there: the stand-in is Invulnerable, and Get Damage (blood, wound
// paint) and Box Vertex Paint (a stuck stab's wound) both start with !Invulnerable. The fighter's owner is the one
// machine where the damage really happens, so it watches its own fighters' "Get Damage", "Spawn Blood" and
// "Box Vertex Paint" (script-VM observers: they also see the victim's replay of a hit claim, which runs under
// ScriptHookBypass) and reports each as a PaintFx in the bone's frame. Every other machine replays it on its stand-in:
//   Blood  its own "Spawn Blood" with the owner's gate state and Bleeding written on the stand-in first;
//   Wound  its own "Box Vertex Paint" over a private box, Invulnerable lowered for that one call and restored.
// Rev 15 adds what the stand-in's Invulnerable and its blocked tick also keep silent or unchanged: the owner's pain voice
// (every line its Voice Audio plays, seen at Helmet Muffle Voice, played on the stand-in's Voice Audio), the blunt and bone sounds of each blow its Get Damage took
// (played at the hit on the stand-in's bone, combat::PlanHitSound) every "Remove Armor" (replayed on the stand-in,
// whose loose piece the prop census then pairs with the owner's), every "Equip Armor" (Pick Up Armor before build 26568321), and strip requests for a fallen
// fighter stripped elsewhere (honoured by its owner).
// Rev 43 adds the crossbow (combat/hsm_crossbow_fx.h): the owner polls its fighters' held crossbows for String Loaded and
// Bolt Loaded and reports each draw, bolt and shot; the stand-in's crossbow replays Event Load String, Event Load Bolt
// (with a bolt of its own) and Event Shoot, so everyone sees the bolt go in and leave.
// Game thread only.
#pragma once
#include "combat/hsm_crossbow_fx.h"
#include "combat/hsm_fx.h"
#include "combat/hsm_paintgate.h"
#include "gameplay/adapter.h"
#include "gameplay/owner_sync.h"
#include "session/hsm_budget.h"
#include "ue/engine.h"
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hsm::ue { class ScriptCall; }

namespace hsm::game {

class FxSync {
public:
    struct Env {
        std::function<uint8_t(const ue::UObject*)> ownedEntity;     // the entity of a fighter simulated here, 0 if none
        std::function<void(const combat::PaintFxMsg&)> send;        // a report made here (the session routes it)
        std::function<ue::UObject*(uint8_t)> ownedActor;            // the live Willie of an entity simulated here
        std::function<void(uint8_t)> gearChanged;                   // an entity's worn armour changed (put on here or on a stand-in): the team kit is put back. Optional.
        std::function<void()> noteSpawn;                            // armour came off a fighter: a new loose object (ObjectSync::NoteSpawn). Optional.
    };
    // Installs the observers (owner "fxsync"; they resolve on every level load). False without the script VM hook.
    bool Start(Adapter& adapter, StandIns& standIns, Env env);
    void Stop();
    void Apply(const combat::PaintFxMsg& m);                         // a fighter simulated elsewhere: shown on its stand-in
    // The session calls this on every PaintFx it decoded, before it applies or relays it: false drops the message (a report
    // over the receive limit of kFxMaxRxPerSecond per fighter and kind, or one naming a cue / class off the allowlists). M1, M2.
    bool Accept(const combat::PaintFxMsg& m);
    void Forget(uint8_t entity);
    // Every frame: reports the crossbow changes of fighters simulated here (a few field reads per held crossbow), then
    // applies armour changes held back by Apply while their fighter's vertex-paint tasks were still running (T5), each once
    // the tasks are done or after kPaintWaitMs.
    void Tick();
    std::string HealthLine();                                        // since the previous call

private:
    struct Box { ue::UObject* actor = nullptr; int32_t actorIndex = -1; ue::UObject* box = nullptr; int32_t boxIndex = -1; };
    struct Counters { uint64_t blood = 0, wounds = 0, pains = 0, hits = 0, armor = 0, limited = 0, noBone = 0, noName = 0, passedOn = 0, applied = 0,
                      skipped = 0, failed = 0, rxLimited = 0, badName = 0, badBone = 0, scanLimited = 0, stripTries = 0, stripHeld = 0,
                      crossbow = 0, crossbowReplayed = 0, bloodGated = 0, paintGated = 0, stripWeapon = 0, stripBlocked = 0, woundDeferred = 0,
                      woundGaveUp = 0; };
    void OnGetDamage(ue::ScriptCall& c);
    void OnSpawnBlood(ue::ScriptCall& c);
    void OnBoxVertexPaint(ue::ScriptCall& c);
    void OnVoice(ue::ScriptCall& c);
    void OnEquipArmor(ue::ScriptCall& c);
    void OnRemoveArmor(ue::ScriptCall& c);
    void OnStripBegin(ue::ScriptCall& c);   // a stand-in its owner reports down counts as Fallen for the stock strip
    void OnStripEnd();
    void SendHitSound(uint8_t entity, ue::UObject* willie, ue::ScriptCall& c, double damageRawScaled);
    void ApplyVoice(const combat::PaintFxMsg& m, ue::UObject* standIn);
    void CrossbowTick();                                             // the owner's side: poll, report changes
    void ApplyCrossbow(const combat::PaintFxMsg& m, ue::UObject* standIn);
    ue::UObject* CrossbowIn(ue::UObject* willie, int hand);          // the crossbow in that hand (0 right, 1 left), null if none
    bool ReadCrossbow(ue::UObject* crossbow, combat::CrossbowState& out);
    bool LoadBolt(ue::UObject* crossbow, ue::UObject* standIn);      // seats a bolt of the stand-in's own
    void ApplyStrip(const combat::PaintFxMsg& m);
    void ApplyArmorOn(const combat::PaintFxMsg& m, ue::UObject* standIn, const combat::Xform& boneWorld);
    void ApplyHitSound(const combat::PaintFxMsg& m, ue::UObject* standIn, const combat::Xform& boneWorld);
    void ApplyArmor(const combat::PaintFxMsg& m, ue::UObject* standIn, ue::UObject* mesh, const combat::Xform& boneWorld);
    bool PlaySoundAt(ue::UObject* standIn, const char* cue, const sync::V3& at, double volume);
    void SendWound(uint8_t entity, ue::UObject* willie, ue::FName bone, const combat::Xform& boxWorld, const sync::V3& extent);
    bool Send(combat::PaintFxMsg& m, ue::UObject* willie, ue::FName bone, bool directions = true, const std::string& label = {});
    // reason 0: the bone is not on the mesh; 1: too far from the bone or degenerate (boneWorld: the bone measured from)
    void NoBone(const combat::PaintFxMsg& m, int reason, const combat::Xform* boneWorld = nullptr);
    bool BoneXform(ue::UObject* mesh, ue::FName bone, combat::Xform& out) const;
    void ApplyBlood(const combat::PaintFxMsg& m, ue::UObject* standIn, ue::UObject* mesh, const combat::Xform& boneWorld);
    void ApplyWound(const combat::PaintFxMsg& m, ue::UObject* standIn, ue::UObject* mesh, const combat::Xform& boneWorld);
    void Count(bool ok, const combat::PaintFxMsg& m);
    void Refuse(const char* why, const combat::PaintFxMsg& m, uint64_t& counter);   // counts a refused peer message, logs the first few
    ue::UObject* PaintBox(uint8_t entity, ue::UObject* standIn);
    void ApplyNow(const combat::PaintFxMsg& m);
    ue::UObject* ArmorTarget(const combat::PaintFxMsg& m);           // the fighter an armour change acts on, null if none
    bool PaintBusy(ue::UObject* willie);                             // its VertexPaintDetectionComponent has tasks queued up

    bool active_ = false;
    Adapter* adapter_ = nullptr;
    StandIns* standIns_ = nullptr;
    Env env_;
    ue::UClass* boxClass_ = nullptr;
    bool boxFailLogged_ = false;
    bool cueFailLogged_ = false;
    uint32_t noBoneLogged_ = 0;                                      // one bit per (kind, reason) already logged
    bool equipLogged_ = false;                                       // the Equip Armor observer has fired once (logged)
    bool replaying_ = false;                                         // inside our own Remove / Equip Armor on a stand-in
    ue::UObject* stripFallen_ = nullptr;                              // the stand-in made Fallen for the running strip call
    ue::UObject* stripWatch_ = nullptr;                               // the stand-in the running strip call acts on, its weapons noted below
    uint8_t stripWatchEntity_ = 0;
    ue::UObject* stripWeapons_[3] = {};                               // its Weapon R, Weapon L, Weapon Slot Back before the call
    std::set<std::pair<uint8_t, uint8_t>> stripped_;                 // (entity, slot) stripped here, awaiting the owner's report
    std::map<std::string, ue::UObject*> cues_;                       // Get Damage's sound cues, found once
    // A peer-named object, found once (or not found: remembered so a bad name costs one scan, not one per message).
    struct ObjRef { ue::UObject* obj = nullptr; int32_t index = -1; };
    combat::NameCache<ObjRef> voiceCues_{256};                       // voice cues named by peers: at most 256, oldest out
    combat::NameCache<bool> missingArmor_{256, 5000.0};              // armour classes no loaded object has, forgotten after 5 s
    TokenBucket cueScans_{4.0, 8.0};                                 // object scans for new cue names: 4/s, burst 8 (a scan is ~8 ms)
    TokenBucket armorWalks_{10.0, 40.0};                             // loose-armour walks: 10/s, burst 40 (a stand-in's whole kit at once)
    uint16_t seq_ = 0;
    combat::FxLimiter limiter_;                                      // sender side: kFxMaxPerSecond per (fighter, kind)
    combat::FxLimiter rxLimiter_{combat::kFxMaxRxPerSecond};         // receiver side, per fighter and kind (one fighter = one peer)
    std::map<int, Box> boxes_;                                       // stand-in entity -> its private paint box
    // T5: the game's paint tasks read a fighter's skinned mesh on worker threads; an armour piece taken off or put on
    // under them frees what they read. Armour changes on a busy fighter wait here, in arrival order.
    struct Held { combat::PaintFxMsg m; double since = 0; };
    std::vector<Held> held_;
    std::vector<Held> woundRetry_;                                   // wounds whose stand-in's Box Vertex Paint gate was shut (Tick retries them)
    combat::PaintGate gate_;                                         // quiet time before an armour replay; no blood right after one
    ue::UClass* paintClass_ = nullptr;
    bool paintClassLooked_ = false;
    uint32_t paintWaitsLogged_ = 0, paintBusyOwnLogged_ = 0;
    Counters n_, reported_;
    // The owner's crossbows, per entity and hand: the object last seen there and its flags then.
    struct CrossbowWatch { ue::UObject* crossbow = nullptr; int32_t index = -1; combat::CrossbowState state; };
    std::array<std::array<CrossbowWatch, 2>, kMaxWillies + 1> crossbows_{};
    std::unordered_map<const ue::UClass*, bool> crossbowClass_;      // a weapon class with String Loaded and Bolt Loaded
    bool crossbowFailLogged_ = false;
};

}  // namespace hsm::game

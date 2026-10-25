// Half Sword game adapter: the only place that knows Half Sword class, function, property and bone
// names. Translates between protocol types (hsm_protocol.h) and the running game. Game thread only.
#pragma once
#include "hsm_protocol.h"
#include "ue/engine.h"
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

namespace hsm { struct VitalsMsg; }

namespace hsm::game {

struct InputBinding { bool isAxis = false; size_t index = 0; bool pressed = false; };

class Adapter {
public:
    bool Resolve(ue::UObject* worldContext);       // per tick; cheap after the first success
    ue::UObject* World() const { return world_; }

    // Players
    ue::UObject* PlayerWillie(int localIndex);     // GameplayStatics.GetPlayerCharacter
    bool ControlRotation(int localIndex, float& yaw, float& pitch);   // the local player's camera/aim rotation

    // Loadouts: the local player's passport (Free Mode or Progression fighter) carried in an SG_GameProgress_C
    // SaveGame serialized to memory, so the engine deep-copies its TMaps/TArrays and reloads classes by path.
    bool ExportPassport(std::vector<uint8_t>& out);
    ue::UObject* LoadPassportSave(const std::vector<uint8_t>& blob);  // nullptr when the bytes are not our SaveGame
    bool SaveToMemory(ue::UObject* saveGame, std::vector<uint8_t>& out);

    // Fighters spawned by the mod (deferred spawn, like BP_SpawnerPoint_Willies).
    struct SpawnOptions {
        const std::vector<uint8_t>* blob = nullptr;   // nullptr = default loadout (GI "Free Mode Player Character 2")
        ue::FVector at; double yawDeg = 0;
        int teamInt = 1;                              // stock: 1 = player side
        bool brain = true;                            // true: plain AIController (host-driven input); false: puppet, no controller
        const void* passport = nullptr;               // raw Str_Passport_Character1 used instead of blob (bots)
        bool stockBrain = false;                      // keep Willie's own NPC brain (bots on the host)
    };
    ue::UObject* SpawnFighter(const SpawnOptions& o);
    void KeepRemoteFighter(ue::UObject* willie, float yawDeg, float pitchDeg);   // per tick on the host
    void RestoreGameInput();                               // game-only input, no cursor (as the stock death screens do)
    // The chat's keyboard: on, UI-only input with the pressed keys flushed (the game's own UI_DeathDoor on open), so the
    // viewport ignores keys while the player types; off, RestoreGameInput.
    void CaptureInput(bool on);
    // First hit straight down from just above `at`, looking through `ignore`; `hitName` gets the hit actor's name.
    bool FloorZ(const ue::FVector& at, const std::vector<ue::UObject*>& ignore, double& z, std::string* hitName = nullptr);
    // A random point on the navmesh within `radius` of `origin`, as the Abyss places its enemies; false: no navmesh there.
    bool NavigablePoint(const ue::FVector& origin, double radius, ue::FVector& out);
    void DestroyActor(ue::UObject* actor);
    // Destroys a fighter (when it is still the object spawned at index) with what it carries: its weapons ("Weapon R",
    // "Weapon L") and quiver are actors of their own and its armour pieces are attached actors, none of which the
    // engine or Willie_BP removes with it. Attached fighters are never destroyed.
    void DestroyFighter(ue::UObject* willie, int32_t index);
    bool StillValid(ue::UObject* o, int32_t index) const;  // index from ue::ObjectIndex at spawn time
    // Owner sync: StillValid and AllWillies also treat an object the game is destroying (BeingDestroyed) or has destroyed
    // (IsGarbage) as gone, instead of until garbage collection frees its slot. Off in host sync, which keeps its old behaviour.
    void SetDestroyAware(bool on) { destroyAware_ = on; }
    // Owner sync: IsWeapon (and so FighterOf) also counts every Blueprint derived from ModularWeaponBP_C: the forged and
    // premade weapons, shields, fists and feet. Host sync keeps the old test, ModularWeaponBP_C exactly (the level's and
    // the procedural weapons only).
    void SetSubclassWeapons(bool on) { subclassWeapons_ = on; }
    // An actor the game is destroying (AActor::IsActorBeingDestroyed) or a component (UActorComponent::IsBeingDestroyed);
    // false for any other object. Each is asked its own class's function: the actor's reads garbage on a component.
    static bool BeingDestroyed(ue::UObject* o);
    std::vector<ue::FVector> SpawnCandidates();            // BP_SpawnerPoint_Willies actors in the loaded level
    ue::FVector ActorLocation(ue::UObject* actor);
    double ActorYaw(ue::UObject* actor);
    double WorldSeconds();                                 // game seconds since this world began play, -1 unreadable
    void Teleport(ue::UObject* actor, const ue::FVector& to);
    ue::UObject* ControllerOf(ue::UObject* pawn);

    // Match support (Phase 3)
    bool IsWillie(ue::UObject* o);
    bool WillieClassKnown() const { return willieClass_ != nullptr; }   // until then IsWillie answers false for everything
    bool FreeModeActive();
    void PrepareFreeModeFight(int foes = -1);
    int FreeModeFoes();                                    // GI "Free Mode Foes Amount", -1 if unreadable
    void SetFreeModeFoes(int foes);                           // GI settings the stock arena select uses for a Free Mode fight
    void QuitGame();
    bool GamePaused();
    void SetGamePaused(bool paused);
    void ConsoleCommand(const std::string& command);      // KismetSystemLibrary.ExecuteConsoleCommand                                 // GI_Settings.Free Mode Activated (false = Progression)
    bool IsDead(ue::UObject* willie);
    ue::UObject* FighterOf(ue::UObject* actor);            // the Willie an actor belongs to (itself, a held weapon, worn armor)
    bool IsWeapon(ue::UObject* actor);
    // Fists and feet: weapon actors a fighter spawns as part of its own body ("Weapon R/L" with an empty hand, "Foot R/L
    // Weapon"), never dropped, picked up or synced as props.
    bool IsBodyWeapon(ue::UObject* actor);
    std::vector<ue::UObject*> AllWillies();                // live Willie_BP_C instances in the loaded level
    std::vector<ue::UObject*> WeaponActors();              // live weapon actors (IsWeapon), sheathed, held and dropped
    // Destroys every weapon lying loose that a fighter once held: no "Parent Actor" (a drop clears it, Willie_BP.cpp:5987,
    // 6299) but a "Last Parent" (set by every grab, :6073, 6394). Level weapons nobody picked up stay. Returns how many.
    int DestroyDroppedWeapons();
    std::vector<ue::UObject*> BodiesAndGear();             // every Willie, what is attached to it (armor, weapons) and every weapon
    // Re-tints the fabric of every armor piece attached to the Willie; returns how many pieces changed.
    int TintArmor(ue::UObject* willie, float r, float g, float b);
    // Spawns a fresh fighter for local player 0 and possesses it; the old pawn is destroyed.
    ue::UObject* RespawnLocalPlayer(const SpawnOptions& o);
    // Host: a stock NPC fighter with a passport from the game's random generator (Enum_Ranks tier 0..7).
    ue::UObject* SpawnAbyssNpc(const ue::FVector& at);   // the Abyss level's own enemy spawner; null elsewhere
    ue::UObject* SpawnBot(int rank, const ue::FVector& at, double yawDeg, int teamInt);
    // The Willie's own passport serialized like ExportPassport (bots are relayed as blobs too).
    bool ExportPassportOf(ue::UObject* willie, std::vector<uint8_t>& out);
    // Per tick: UI_DED / UI_DeathDoor / UI_WinScreen / UI_WIN never construct while on. ownerMatch (owner sync) also
    // suppresses UI_Lose / UI_GiveUp / UI_NextFIght, the single-player lose flow that pauses the game.
    void SuppressStockScreens(bool on, bool ownerMatch = false);

    // Input (resolved from Willie_BP_C event functions)
    bool BindInputs(ue::UObject* willie);
    const std::unordered_map<ue::UFunction*, InputBinding>& Bindings() const { return bindings_; }
    void ApplyAxis(ue::UObject* willie, Axis a, float value);
    void FireAction(ue::UObject* willie, Action a, bool pressed);
    static float ReadAxisParam(ue::UFunction* fn, void* params);

    // State
    bool ReadState(ue::UObject* willie, uint8_t entity, WillieState& out);
    // Owner-authority vitals (protocol rev 9): dead ("DED"), fallen, knocked out (Consciousness <= 0), the health
    // fields, the severed-part mask; dislocations are not read yet (Phase 4). False without a readable Willie.
    bool ReadVitals(ue::UObject* willie, uint8_t entity, VitalsMsg& out);
    ue::UObject* Mesh(ue::UObject* willie) { return MeshOf(willie); }
    void DriveTowards(ue::UObject* willie, const WillieState& target, float dt);
    bool FirstBodyPosition(ue::UObject* willie, ue::FVector& out);   // world position of the first synced bone (hip)
    void NudgeTowards(ue::UObject* willie, const WillieState& target, float blend);   // blends bone velocities, keeps the local motion
    void ApplyVitals(ue::UObject* willie, const WillieState& s);
    void TriggerDeath(ue::UObject* willie);
    size_t BoneCount() const { return bones_.size(); }

private:
    bool ResolveBones(ue::UObject* mesh);
    ue::UObject* GameInstance();
    std::wstring SaveGamesDir();
    std::string TempSlot();
    void DeleteSlot(const std::string& slot);
    std::wstring saveDir_;
    uint32_t slotCounter_ = 0;
    bool CopyPassport(ue::UObject* willie, const SpawnOptions& o, double& height);
    bool SavePassportFrom(ue::UObject* owner, ue::FProperty* src, std::vector<uint8_t>& out);
    ue::UObject* MeshOf(ue::UObject* willie);
    ue::UObject* world_ = nullptr;
    ue::UObject* statics_ = nullptr;               // GameplayStatics CDO
    ue::UClass* willieClass_ = nullptr;
    bool destroyAware_ = false;
    bool inputCaptured_ = false;   // CaptureInput(true) holds: a respawn keeps the chat's keyboard
    bool subclassWeapons_ = false;
    ue::FName weaponName_{};                       // ModularWeaponBP_C, once a class chain has shown it
    bool weaponNameKnown_ = false;
    std::array<ue::UFunction*, kAxisCount> axisFn_{};
    std::array<ue::UFunction*, kActionCount> pressFn_{}, releaseFn_{};
    std::unordered_map<ue::UFunction*, InputBinding> bindings_;
    std::vector<ue::FName> bones_;
    std::vector<std::string> boneNames_;
    bool bonesResolved_ = false;
};

}  // namespace hsm::game

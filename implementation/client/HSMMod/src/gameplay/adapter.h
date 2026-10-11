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
    void RestoreGameInput();
    bool FloorZ(const ue::FVector& at, ue::UObject* ignore, double& z);   // first hit straight down from just above `at`                               // game-only input, no cursor (as the stock death screens do)
    void DestroyActor(ue::UObject* actor);
    // Destroys a fighter (when it is still the object spawned at index) with what it carries: its weapons ("Weapon R",
    // "Weapon L") and quiver are actors of their own and its armour pieces are attached actors, none of which the
    // engine or Willie_BP removes with it. Attached fighters are never destroyed.
    void DestroyFighter(ue::UObject* willie, int32_t index);
    bool StillValid(ue::UObject* o, int32_t index) const;  // index from ue::ObjectIndex at spawn time
    std::vector<ue::FVector> SpawnCandidates();            // BP_SpawnerPoint_Willies actors in the loaded level
    ue::FVector ActorLocation(ue::UObject* actor);
    double ActorYaw(ue::UObject* actor);
    void Teleport(ue::UObject* actor, const ue::FVector& to);
    ue::UObject* ControllerOf(ue::UObject* pawn);

    // Match support (Phase 3)
    bool IsWillie(ue::UObject* o);
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
    std::vector<ue::UObject*> AllWillies();                // live Willie_BP_C instances in the loaded level
    // Re-tints the fabric of every armor piece attached to the Willie; returns how many pieces changed.
    int TintArmor(ue::UObject* willie, float r, float g, float b);
    // Spawns a fresh fighter for local player 0 and possesses it; the old pawn is destroyed.
    ue::UObject* RespawnLocalPlayer(const SpawnOptions& o);
    // Host: a stock NPC fighter with a passport from the game's random generator (Enum_Ranks tier 0..7).
    ue::UObject* SpawnAbyssNpc(const ue::FVector& at);   // the Abyss level's own enemy spawner; null elsewhere
    ue::UObject* SpawnBot(int rank, const ue::FVector& at, double yawDeg, int teamInt);
    // The Willie's own passport serialized like ExportPassport (bots are relayed as blobs too).
    bool ExportPassportOf(ue::UObject* willie, std::vector<uint8_t>& out);
    void SuppressStockScreens(bool on);                    // per tick: UI_DED / UI_DeathDoor / UI_WinScreen / UI_WIN never construct while on

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
    std::array<ue::UFunction*, kAxisCount> axisFn_{};
    std::array<ue::UFunction*, kActionCount> pressFn_{}, releaseFn_{};
    std::unordered_map<ue::UFunction*, InputBinding> bindings_;
    std::vector<ue::FName> bones_;
    std::vector<std::string> boneNames_;
    bool bonesResolved_ = false;
};

}  // namespace hsm::game

// In-game probes for the Phase 2/3 runtime assumptions that cannot be checked offline:
//   selftest                 reflection layout + ProcessEvent (engine SelfTest)
//   passport_export          the local passport serializes through SG_GameProgress_C
//   passport_roundtrip       LoadGameFromMemory + SaveGameToMemory reproduces the exact bytes
//   passport_reject_garbage  non-SaveGame bytes are refused (negative control for the import path)
//   spawn_points             the level has BP_SpawnerPoint_Willies actors (0 is expected on non-arena maps)
//   spawn_fighter            deferred spawn with the blob, plain AIController possession, Player forced on
//   spawn_puppet             deferred spawn without a controller
//   puppet_drive             snapshot driving moves a puppet's pelvis toward a target 150 cm away
//   tint_armor               team tint reaches the test fighter's attached armor (0 pieces = bare loadout, fails)
//   spawn_bot                the random generator yields a passport and the bot keeps the stock NPC brain
//   weapon_owner             a weapon held by the local fighter resolves back to that fighter (kill credit)
// tools/check_hsmtest.py validates the resulting HSMTest.json.
#include "test/harness.h"
#include "gameplay/lobby.h"
#include "log.h"
#include "paths.h"
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace hsm::test {
using namespace hsm::ue;
namespace {

constexpr const char* kProbes[] = {"selftest", "passport_export", "passport_roundtrip", "passport_reject_garbage",
                                   "spawn_points", "spawn_fighter", "spawn_puppet", "puppet_drive",
                                   "tint_armor", "spawn_bot", "weapon_owner"};
constexpr float kSettleSeconds = 3.f, kPossessSeconds = 1.f, kDriveSeconds = 2.f;
constexpr double kDriveOffset = 150.0, kDriveMinMoved = 75.0;

struct Result { std::string name; bool pass = false; std::string detail; };

enum class Phase { Off, WaitLevel, Settle, Possess, Drive, Done };

Phase g_phase = Phase::Off;
std::string g_selection;
std::vector<Result> g_results;
float g_timer = 0;
UObject* g_fighter = nullptr; int32_t g_fighterIndex = -1;
UObject* g_puppet = nullptr; int32_t g_puppetIndex = -1;
UObject* g_bot = nullptr; int32_t g_botIndex = -1;
WillieState g_driveTarget;
double g_driveStartX = 0;

bool Selected(const char* probe) {
    if (g_selection == "all") return true;
    std::string list = "," + g_selection + ",";
    return list.find(std::string(",") + probe + ",") != std::string::npos;
}

void Record(const char* name, bool pass, const std::string& detail) {
    if (!Selected(name)) return;
    g_results.push_back({name, pass, detail});
    HSM_LOG("hsmtest: %s %s (%s)", name, pass ? "PASS" : "FAIL", detail.c_str());
}

std::string Escape(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o.push_back('\\'); if (static_cast<unsigned char>(c) >= 0x20) o.push_back(c); }
    return o;
}

void WriteReport() {
    std::string j = "{\n  \"schema\": 1,\n  \"mod_version\": \"" + std::string(game::kModVersion) + "\",\n";
    char key[32]; std::snprintf(key, sizeof key, "%016llx", static_cast<unsigned long long>(BuildKey()));
    j += "  \"build_key\": \"" + std::string(key) + "\",\n  \"selection\": \"" + Escape(g_selection) + "\",\n  \"results\": [\n";
    for (size_t i = 0; i < g_results.size(); ++i) {
        const Result& r = g_results[i];
        j += "    {\"name\": \"" + r.name + "\", \"pass\": " + (r.pass ? "true" : "false") + ", \"detail\": \"" + Escape(r.detail) + "\"}";
        j += i + 1 < g_results.size() ? ",\n" : "\n";
    }
    j += "  ],\n  \"complete\": true\n}\n";
    std::wstring path = InstanceFilePath(L"HSMTest", L".json");
    if (FILE* f = _wfopen(path.c_str(), L"wb")) { std::fwrite(j.data(), 1, j.size(), f); std::fclose(f); HSM_LOG("hsmtest: report written (%zu probes)", g_results.size()); }
    else HSM_LOG("hsmtest: cannot write HSMTest.json");
}

bool Alive(UObject* o, int32_t index) { return o && index >= 0 && ObjectAt(index) == o; }

void RunPassportAndSpawn(game::Adapter& a) {
    std::vector<uint8_t> blob;
    bool exported = a.ExportPassport(blob);
    Record("passport_export", exported && !blob.empty(), std::to_string(blob.size()) + " bytes");
    std::vector<uint8_t> again;
    UObject* sg = exported ? a.LoadPassportSave(blob) : nullptr;
    bool same = sg && a.SaveToMemory(sg, again) && again == blob;
    Record("passport_roundtrip", same, sg ? (std::to_string(again.size()) + " bytes after reload") : "LoadGameFromMemory returned null");
    std::vector<uint8_t> garbage(64, 0x5A);
    Record("passport_reject_garbage", a.LoadPassportSave(garbage) == nullptr, "64 bytes of 0x5A");
    size_t points = a.SpawnCandidates().size();
    Record("spawn_points", points > 0, std::to_string(points) + " BP_SpawnerPoint_Willies actors");

    UObject* me = a.PlayerWillie(0);
    FVector base = me ? a.ActorLocation(me) : FVector{};
    game::Adapter::SpawnOptions o; o.blob = exported ? &blob : nullptr;
    if (Selected("spawn_fighter") || Selected("tint_armor")) {
        o.at = {base.x + 300, base.y, base.z}; o.brain = true;
        g_fighter = a.SpawnFighter(o); g_fighterIndex = ObjectIndex(g_fighter);
    }
    if (Selected("spawn_puppet") || Selected("puppet_drive")) {
        o.at = {base.x - 300, base.y, base.z}; o.brain = false;
        g_puppet = a.SpawnFighter(o); g_puppetIndex = ObjectIndex(g_puppet);
    }
    if (Selected("spawn_bot")) { g_bot = a.SpawnBot(3, {base.x, base.y + 400, base.z}, 0, 2); g_botIndex = ObjectIndex(g_bot); }
    if (Selected("weapon_owner")) {
        int held = 0, credited = 0;
    for (UObject* w : FindInstances("ModularWeaponBP_C", true)) {
            UObject** parent = Field<UObject*>(w, "Parent Actor");
            if (!parent || *parent != me) continue;
            ++held; credited += a.FighterOf(w) == me;
        }
        Record("weapon_owner", held > 0 && credited == held, std::to_string(credited) + "/" + std::to_string(held) + " held weapons credited to you");
    }
}

void CheckSpawns(game::Adapter& a) {
    if (Selected("spawn_fighter")) {
        bool alive = Alive(g_fighter, g_fighterIndex);
        UObject* ctrl = alive ? a.ControllerOf(g_fighter) : nullptr;
        std::string cls = ctrl ? ObjectName(reinterpret_cast<UObject*>(ClassOf(ctrl))) : "none";
        bool player = false;
        if (alive) { a.KeepRemoteFighter(g_fighter, 90.f, 0.f); GetBool(g_fighter, "Player", player); }
        Record("spawn_fighter", alive && cls == "AIController" && player, "controller " + cls + (player ? ", Player=true" : ", Player=false"));
    }
    if (Selected("tint_armor")) {
        int pieces = Alive(g_fighter, g_fighterIndex) ? a.TintArmor(g_fighter, 0.7f, 0.05f, 0.04f) : -1;
        Record("tint_armor", pieces > 0, pieces < 0 ? "test fighter missing" : std::to_string(pieces) + " armor pieces re-tinted red");
    }
    if (Selected("spawn_bot")) {
        bool alive = Alive(g_bot, g_botIndex);
        UObject* ctrl = alive ? a.ControllerOf(g_bot) : nullptr;
        std::string cls = ctrl ? ObjectName(reinterpret_cast<UObject*>(ClassOf(ctrl))) : "none";
        Record("spawn_bot", alive && ctrl && cls != "AIController", alive ? "controller " + cls : "generator/spawn failed");
    }
    if (Selected("spawn_puppet")) {
        bool alive = Alive(g_puppet, g_puppetIndex);
        UObject* ctrl = alive ? a.ControllerOf(g_puppet) : nullptr;
        Record("spawn_puppet", alive && !ctrl, alive ? (ctrl ? "unexpected controller" : "no controller") : "not spawned");
    }
}

bool StartDrive(game::Adapter& a) {
    if (!Alive(g_puppet, g_puppetIndex) || !a.ReadState(g_puppet, 0, g_driveTarget) || g_driveTarget.bodies.empty()) return false;
    g_driveStartX = g_driveTarget.bodies[0].px;
    for (BodyState& b : g_driveTarget.bodies) b.px += float(kDriveOffset);
    return true;
}

void FinishDrive(game::Adapter& a) {
    WillieState now;
    bool ok = Alive(g_puppet, g_puppetIndex) && a.ReadState(g_puppet, 0, now) && !now.bodies.empty();
    double moved = ok ? now.bodies[0].px - g_driveStartX : 0;
    Record("puppet_drive", ok && moved >= kDriveMinMoved, "pelvis moved " + std::to_string(int(std::lround(moved))) + " of 150 cm");
}

void Cleanup(game::Adapter& a) {
    if (Alive(g_fighter, g_fighterIndex)) a.DestroyActor(g_fighter);
    if (Alive(g_puppet, g_puppetIndex)) a.DestroyActor(g_puppet);
    if (Alive(g_bot, g_botIndex)) a.DestroyActor(g_bot);
    g_fighter = g_puppet = g_bot = nullptr;
}
}  // namespace

void Begin(bool selfTestOk) {
    std::optional<std::string> sel = CmdArg(CommandLineUtf8(), "hsmtest");
    if (!sel) return;
    g_selection = *sel;
    bool known = g_selection == "all";
    for (const char* p : kProbes) known = known || Selected(p);
    if (!known) { HSM_LOG("hsmtest: no known probe in '%s'", g_selection.c_str()); return; }
    HSM_LOG("hsmtest: enabled (%s) - load into any fight level", g_selection.c_str());
    Record("selftest", selfTestOk, selfTestOk ? "reflection + ProcessEvent ok" : "engine self-test failed");
    g_phase = selfTestOk ? Phase::WaitLevel : Phase::Done;
    if (!selfTestOk) WriteReport();
}

void Tick(game::Adapter& a, bool inFightLevel, float dt) {
    switch (g_phase) {
        case Phase::Off: case Phase::Done: return;
        case Phase::WaitLevel:
            if (inFightLevel && a.PlayerWillie(0)) { g_phase = Phase::Settle; g_timer = 0; }
            return;
        case Phase::Settle:
            if ((g_timer += dt) < kSettleSeconds) return;
            RunPassportAndSpawn(a);
            g_phase = Phase::Possess; g_timer = 0;
            return;
        case Phase::Possess:
            if ((g_timer += dt) < kPossessSeconds) return;
            CheckSpawns(a);
            g_timer = 0;
            if (Selected("puppet_drive") && StartDrive(a)) { g_phase = Phase::Drive; return; }
            if (Selected("puppet_drive")) Record("puppet_drive", false, "puppet missing or unreadable");
            Cleanup(a); WriteReport(); g_phase = Phase::Done;
            return;
        case Phase::Drive:
            if (Alive(g_puppet, g_puppetIndex)) a.DriveTowards(g_puppet, g_driveTarget, dt);
            if ((g_timer += dt) < kDriveSeconds) return;
            FinishDrive(a); Cleanup(a); WriteReport(); g_phase = Phase::Done;
            return;
    }
}

}  // namespace hsm::test

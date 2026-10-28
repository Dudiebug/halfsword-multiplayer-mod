#include "gameplay/damage_pin.h"
#include <windows.h>
#include "log.h"
#include "ue/engine.h"
#include "ue/script_hook.h"
#include <cstring>
#include <string>

namespace hsm::game {
using namespace ue;

namespace {
constexpr const char* kHookOwner = "damagepin";
// GI_Settings.cpp:4-5; the slider sets them (UI_GameSettings.cpp:268, 277), Get Damage reads them (Willie_BP.cpp:2974).
constexpr const char* kPlayer = "Player Damage Rate";
constexpr const char* kNpc = "NPC Damage Rate";
constexpr uint64_t kSliderScanMs = 15000;

struct Pin {
    UObject* gi = nullptr;                 // the game instance lives as long as the process
    bool hooked = false, hookFailLogged = false, active = false;
    bool stockRead = false, sliderRead = false;
    double stockPlayer = 1.0, stockNpc = 1.0;
    double ownPlayer = 0, ownNpc = 0;      // the player's own values, saved for them and put back after the session
    double forcedPlayer = 0, forcedNpc = 0;
    DamageRange range;
    uint64_t nextSliderScan = 0;
    unsigned repins = 0, saves = 0;
};
Pin g;

double* Rate(UObject* o, const char* name) { return o ? Field<double>(o, name) : nullptr; }

// A float or double property, whichever this build declares (USlider's range is float; the reflection index also
// has a double StepSize on another class).
bool ReadReal(UObject* o, const char* name, double& out) {
    FProperty* p = FindProperty(ClassOf(o), name);
    if (!p) return false;
    const std::string type = PropertyType(p);
    const uint8_t* v = reinterpret_cast<const uint8_t*>(o) + PropertyOffset(p);
    if (type == "FloatProperty") { float f; std::memcpy(&f, v, sizeof f); out = f; return true; }
    if (type == "DoubleProperty") { std::memcpy(&out, v, sizeof out); return true; }
    return false;
}

void ReadStock() {
    if (g.stockRead || !g.gi) return;
    UObject* cdo = DefaultObject(ClassOf(g.gi));
    double* p = Rate(cdo, kPlayer);
    double* n = Rate(cdo, kNpc);
    if (!p || !n) return;
    g.stockPlayer = *p; g.stockNpc = *n;
    g.stockRead = true;
    g.range = DamageRangeFrom(-1, -1, -1, g.stockPlayer);   // the slider's range when it is read; the stock rate now
    HSM_LOG("damage: stock rates player %.3f, NPC %.3f (class default)", g.stockPlayer, g.stockNpc);
}

// The slider's template is loaded with the settings widget's class; its MinValue, MaxValue and StepSize are the range.
void ReadSlider() {
    if (g.sliderRead || !g.stockRead) return;
    const uint64_t now = GetTickCount64();
    if (now < g.nextSliderScan) return;
    g.nextSliderScan = now + kSliderScanMs;
    for (UObject* s : FindInstances("Slider")) {
        if (ObjectName(s) != "Slider_DamagePlayer") continue;
        double lo = 0, hi = 0, step = 0;
        if (!ReadReal(s, "MinValue", lo) || !ReadReal(s, "MaxValue", hi) || !ReadReal(s, "StepSize", step)) continue;
        g.range = DamageRangeFrom(lo, hi, step, g.stockPlayer);
        g.sliderRead = true;
        HSM_LOG("damage: the game's damage slider runs %.3f..%.3f, step %.3f (%s); host range %d..%d%%, step %d%%, stock %d%%",
                lo, hi, step, FullName(s).c_str(), g.range.minPct, g.range.maxPct, g.range.stepPct, g.range.stockPct);
        return;
    }
}

void Hook() {
    if (g.hooked || !ScriptHookActive()) return;
    // GI_Settings.cpp:1949 Save Settings copies every game-instance setting into the Settings save. While pinned, the
    // player's own values go in for it (a slider they moved is theirs from then on), and the pin is back once it ran.
    auto before = [](ScriptCall& call) {
        if (!g.active) return false;
        double* p = Rate(call.self(), kPlayer);
        double* n = Rate(call.self(), kNpc);
        if (p) *p = OwnBeforeSave(g.ownPlayer, *p, g.forcedPlayer);
        if (n) *n = OwnBeforeSave(g.ownNpc, *n, g.forcedNpc);
        if (++g.saves % 20 == 1) HSM_LOG("damage: the game saved its settings with the player's own rates (player %.3f, NPC %.3f)", g.ownPlayer, g.ownNpc);
        return false;
    };
    auto after = [](ScriptCall& call) {
        if (!g.active) return;
        if (double* p = Rate(call.self(), kPlayer)) *p = g.forcedPlayer;
        if (double* n = Rate(call.self(), kNpc)) *n = g.forcedNpc;
    };
    g.hooked = HookScript(kHookOwner, "GI_Settings_C", "Save Settings", before, after) > 0;
    if (!g.hooked && !g.hookFailLogged) HSM_LOG("damage: Save Settings could not be hooked; the session's damage is not pinned");
    g.hookFailLogged = !g.hooked;
}

}  // namespace

const DamageRange& VanillaDamageRange() { return g.range; }

void TickDamagePin(bool active, uint16_t stored) {
    if (!g.gi) {
        auto v = FindInstances("GI_Settings_C");
        if (v.empty()) return;
        g.gi = v[0];
    }
    ReadStock();
    ReadSlider();
    double* p = Rate(g.gi, kPlayer);
    double* n = Rate(g.gi, kNpc);
    if (!p || !n || !g.stockRead) return;
    if (active && !g.active) {
        Hook();
        if (!g.hooked) return;                             // fail closed: an unhooked save would keep the pinned values
        g.ownPlayer = *p; g.ownNpc = *n;
        g.active = true; g.repins = 0; g.saves = 0;
        HSM_LOG("damage: pinned for the session (own rates player %.3f, NPC %.3f)", g.ownPlayer, g.ownNpc);
    } else if (!active && g.active) {
        g.active = false;
        *p = g.ownPlayer; *n = g.ownNpc;
        HSM_LOG("damage: session over; own rates back (player %.3f, NPC %.3f; %u re-pin(s))", g.ownPlayer, g.ownNpc, g.repins);
        return;
    }
    if (!g.active) return;
    const double player = DamageRate(stored, g.range);
    if (player != g.forcedPlayer || g.stockNpc != g.forcedNpc)
        HSM_LOG("damage: everyone's player damage is %.3f (%s), NPC damage the stock %.3f", player, DamageLabel(stored, g.range).c_str(), g.stockNpc);
    g.forcedPlayer = player; g.forcedNpc = g.stockNpc;
    // A level load reads the player's save back into the game instance (BP_HalfSwordGameMode.cpp:163): pin again.
    if (*p != g.forcedPlayer || *n != g.forcedNpc) {
        if (++g.repins <= 3 || g.repins % 50 == 0)
            HSM_LOG("damage: the game instance had player %.3f, NPC %.3f; pinned again (%u)", *p, *n, g.repins);
        *p = g.forcedPlayer; *n = g.forcedNpc;
    }
}

}  // namespace hsm::game

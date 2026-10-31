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
// Willie_BP.cpp:1187: the rate Get Damage multiplies by, cached by BeginPlay (:16490) and Apply Gameplay Settings (:7681)
// from the Player flag, which is only computed 0.1 s after BeginPlay (:13033) or set by the mod after a spawn.
constexpr const char* kCache = "Damagr Rste Var";
constexpr uint64_t kSliderScanMs = 15000, kSweepMs = 5000;   // a full object scan: spawns refresh their own (RefreshDamageCache)

struct Pin {
    UObject* gi = nullptr;                 // the game instance lives as long as the process
    bool hooked = false, hookFailLogged = false, active = false;
    bool stockRead = false, sliderRead = false;
    double stockPlayer = 1.0, stockNpc = 1.0;
    double ownPlayer = 0, ownNpc = 0;      // the player's own values, saved for them and put back after the session
    double forcedPlayer = 0, forcedNpc = 0;
    bool sweepNow = false;                 // the pin changed: every fighter's cache is written again
    uint64_t nextSweep = 0;
    DamageRange range;
    uint64_t nextSliderScan = 0;
    unsigned repins = 0, saves = 0;
};
Pin g;

// The pinned GI_Settings doubles (sync audit 10, 13): Willie_BP.cpp:7680-7703 copies each onto the fighter by name.
struct Phys {
    bool active = false;                   // pinned (the host sent a value) and the player's own saved
    bool restore = false;                  // unpinned: the next sweep writes the own value back onto the fighters
    double own = 0, forced = 0;
};
std::array<Phys, kPinnedPhysics> gp;
bool physSweepNow = false;
uint64_t physNextSweep = 0;

void SweepPhysics() {
    for (UObject* w : FindInstances("Willie_BP_C")) {
        if (IsGarbage(w)) continue;
        for (size_t i = 0; i < kPinnedPhysics; ++i)
            if (gp[i].active || gp[i].restore) if (double* d = Field<double>(w, kPinnedPhysicsNames[i]); d && *d != gp[i].forced) *d = gp[i].forced;
    }
}

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
        if (ObjectName(s) != "Slider_DamagePlayer" || !IsGameSettingsSlider(FullName(s))) continue;   // UI_CameraSettings has one too (0..1)
        double lo = 0, hi = 0, step = 0;
        if (!ReadReal(s, "MinValue", lo) || !ReadReal(s, "MaxValue", hi) || !ReadReal(s, "StepSize", step)) continue;
        g.range = DamageRangeFrom(lo, hi, step, g.stockPlayer);
        g.sliderRead = true;
        HSM_LOG("damage: the game's damage slider runs %.3f..%.3f, step %.3f (%s); host range %d..%d%%, step %d%%, stock %d%%",
                lo, hi, step, FullName(s).c_str(), g.range.minPct, g.range.maxPct, g.range.stepPct, g.range.stockPct);
        return;
    }
}

// The rate a fighter should be hit at: player-controlled fighters (Player) the player rate, the others the NPC rate.
void WriteCache(UObject* willie, double player, double npc) {
    double* c = Field<double>(willie, kCache);
    bool isPlayer = false;
    if (!c || !GetBool(willie, "Player", isPlayer)) return;
    const double want = isPlayer ? player : npc;
    if (*c != want) *c = want;
}

void Sweep(double player, double npc) {
    for (UObject* w : FindInstances("Willie_BP_C")) if (!IsGarbage(w)) WriteCache(w, player, npc);
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
        for (size_t i = 0; i < kPinnedPhysics; ++i)
            if (gp[i].active) if (double* d = Rate(call.self(), kPinnedPhysicsNames[i])) *d = OwnBeforeSave(gp[i].own, *d, gp[i].forced);
        if (++g.saves % 20 == 1) HSM_LOG("damage: the game saved its settings with the player's own rates (player %.3f, NPC %.3f)", g.ownPlayer, g.ownNpc);
        return false;
    };
    auto after = [](ScriptCall& call) {
        if (!g.active) return;
        if (double* p = Rate(call.self(), kPlayer)) *p = g.forcedPlayer;
        if (double* n = Rate(call.self(), kNpc)) *n = g.forcedNpc;
        for (size_t i = 0; i < kPinnedPhysics; ++i)
            if (gp[i].active) if (double* d = Rate(call.self(), kPinnedPhysicsNames[i])) *d = gp[i].forced;
    };
    g.hooked = HookScript(kHookOwner, "GI_Settings_C", "Save Settings", before, after) > 0;
    if (!g.hooked && !g.hookFailLogged) HSM_LOG("damage: Save Settings could not be hooked; the session's damage is not pinned");
    g.hookFailLogged = !g.hooked;
}

}  // namespace

const DamageRange& VanillaDamageRange() { return g.range; }

void RefreshDamageCache(UObject* willie) {
    if (g.active && willie) WriteCache(willie, g.forcedPlayer, g.forcedNpc);
}

void TickDamagePin(bool active, uint16_t stored, bool host) {
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
        Sweep(g.ownPlayer, g.ownNpc);                      // the fighters' cached rates go back too
        HSM_LOG("damage: session over; own rates back (player %.3f, NPC %.3f; %u re-pin(s))", g.ownPlayer, g.ownNpc, g.repins);
        return;
    }
    if (!g.active) return;
    const double player = DamageRate(stored, g.range, !host);   // a client takes the host's value as sent
    if (player != g.forcedPlayer || g.stockNpc != g.forcedNpc) {
        g.sweepNow = true;
        HSM_LOG("damage: everyone's player damage is %.3f (%s), NPC damage the stock %.3f", player, DamageLabel(stored, g.range).c_str(), g.stockNpc);
    }
    g.forcedPlayer = player; g.forcedNpc = g.stockNpc;
    // A level load reads the player's save back into the game instance (BP_HalfSwordGameMode.cpp:163): pin again.
    if (*p != g.forcedPlayer || *n != g.forcedNpc) {
        if (++g.repins <= 3 || g.repins % 50 == 0)
            HSM_LOG("damage: the game instance had player %.3f, NPC %.3f; pinned again (%u)", *p, *n, g.repins);
        *p = g.forcedPlayer; *n = g.forcedNpc;
    }
    // Fighters cache the rate when they spawn, before Player is known: write it again on a pin change and every half second.
    const uint64_t now = GetTickCount64();
    if (g.sweepNow || now >= g.nextSweep) {
        g.sweepNow = false; g.nextSweep = now + kSweepMs;
        Sweep(g.forcedPlayer, g.forcedNpc);
    }
}

std::array<float, kPinnedPhysics> OwnPhysics() {
    std::array<float, kPinnedPhysics> out{ -1.f, -1.f, -1.f };
    for (size_t i = 0; i < kPinnedPhysics; ++i) {
        double* d = Rate(g.gi, kPinnedPhysicsNames[i]);
        const double v = gp[i].active ? gp[i].own : d ? *d : -1.0;
        if (d && ValidPinnedPhysics(float(v))) out[i] = float(v);
    }
    return out;
}

// After TickDamagePin, which finds the game instance and hooks Save Settings (g.active means both happened).
void TickPhysicsPin(const std::array<float, kPinnedPhysics>& forced) {
    bool any = false;
    for (size_t i = 0; i < kPinnedPhysics; ++i) {
        Phys& ph = gp[i];
        double* d = Rate(g.gi, kPinnedPhysicsNames[i]);
        const bool want = g.active && d && forced[i] >= 0.f;
        if (want && !ph.active) {
            ph.own = *d; ph.active = true; ph.restore = false;
            HSM_LOG("physics: %s pinned to the host's %.3f (own %.3f)", kPinnedPhysicsNames[i], double(forced[i]), ph.own);
        } else if (!want && ph.active) {
            ph.active = false; ph.restore = true; ph.forced = ph.own; physSweepNow = true;
            if (d) *d = ph.own;
            HSM_LOG("physics: %s back to the player's own %.3f", kPinnedPhysicsNames[i], ph.own);
        }
        if (ph.active) {
            if (ph.forced != double(forced[i])) { ph.forced = forced[i]; physSweepNow = true; }
            if (*d != ph.forced) *d = ph.forced;   // a level load reads the save back (BP_HalfSwordGameMode.cpp:163)
        }
        any = any || ph.active || ph.restore;
    }
    const uint64_t now = GetTickCount64();
    if (!any || (!physSweepNow && now < physNextSweep)) return;
    SweepPhysics();                                // fighters copy them when they spawn or the settings apply
    physSweepNow = false; physNextSweep = now + kSweepMs;
    for (Phys& ph : gp) ph.restore = false;
}

}  // namespace hsm::game

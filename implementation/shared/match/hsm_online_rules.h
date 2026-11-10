// What Classes lobbies allow, compiled into the mod so it can't be edited like a launcher's files: HS Enhancer's and Half
// Wolverine's settings sheets, read on every player's machine from the card the other player sends. Nothing here judges the
// rest of a player's PC: what's in the game folder is the player's mods, and the host's rule (session/hsm_mod_rules.h)
// decides on those. A modified client can still lie: this raises the cost of cheating and gives the host something to act
// on. Game-independent.
#pragma once
#include "match/hsm_build_card.h"
#include "match/hsm_class.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

constexpr const char* kRuleLockedMenuKey = "143";               // the launcher's lock: a key code no key sends
constexpr long kRuleLockedKeyCode = 143;

inline std::string RuleLower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// A UE4SS mod list ("A, B"): which class mods it holds.
struct RuleMods { bool hse = false, wolverine = false; };
inline RuleMods ReadRuleMods(std::string_view reported) {
    RuleMods out;
    size_t at = 0;
    while (at <= reported.size()) {
        size_t end = reported.find(", ", at);
        if (end == std::string_view::npos) end = reported.size();
        const std::string_view n = reported.substr(at, end - at);
        out.hse = out.hse || n == "HSEnhancer";
        out.wolverine = out.wolverine || n == "HalfWolverine";
        at = end + 2;
    }
    return out;
}

// mods.txt with these mods off, every other byte kept. A mod it doesn't list gets a line. ": 0" wins over an enabled.txt, in UE4SS and in
// EnabledMods, so nothing else has to change, and the player's own file comes back when the game closes.
inline std::string ModsTxtWithOff(std::string text, const std::vector<std::string>& mods) {
    const bool crlf = text.find("\r\n") != std::string::npos;
    for (const std::string& mod : mods) {
        bool found = false;
        size_t at = 0;
        while (at <= text.size() && !found) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string_view line(text.data() + at, end - at);
            const size_t colon = line.find(':');
            if (!line.empty() && line[0] != ';' && colon != std::string_view::npos) {
                std::string_view name = line.substr(0, colon);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
                while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.remove_prefix(1);
                if (RuleLower(name) == RuleLower(mod)) {
                    const size_t lineEnd = !line.empty() && line.back() == '\r' ? end - 1 : end;
                    text.replace(at, lineEnd - at, mod + " : 0");
                    found = true;
                }
            }
            at = end + 1;
        }
        if (!found) {
            if (!text.empty() && text.back() != '\n') text += crlf ? "\r\n" : "\n";
            text += mod + " : 0" + (crlf ? "\r\n" : "\n");
        }
    }
    return text;
}

// The check that a fighter's base stats are the game's. HS Enhancer changes them, so it can't apply where the host allows HS Enhancer.
inline bool BaseStatCheckApplies(bool classesLobby, bool blockHse) { return classesLobby && blockHse; }

// HSE's own loader, by what's inside it, so any HSE version or file name counts. The launcher uses it to name HSE beside the exe.
inline bool LooksLikeHseLoader(std::string_view fileBytes) { return fileBytes.find("HSE_Initialize") != std::string_view::npos; }

// A value from INI text: [section] key = value. Nothing when the section or key is missing.
inline std::optional<std::string> RuleIniValue(std::string_view text, std::string_view section, std::string_view key) {
    bool inSection = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (!line.empty() && line.front() == '[') { inSection = line == "[" + std::string(section) + "]"; continue; }
        if (!inSection) continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.remove_suffix(1);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
        if (k == key) return std::string(v);
    }
    return std::nullopt;
}

// Any hotkey left bound, in any section. A class that paid for Bite may keep Bite's key: the key can't switch on anything else.
inline bool AnyHotkeyBound(std::string_view ini) {
    const bool biteOn = RuleIniValue(ini, "BiteAttack", "enabled") == "true";
    bool inBite = false;
    size_t at = 0;
    while (at < ini.size()) {
        size_t end = ini.find('\n', at);
        if (end == std::string_view::npos) end = ini.size();
        std::string_view line = ini.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.front() == '[') { inBite = line.rfind("[BiteAttack]", 0) == 0; continue; }
        const size_t eq = line.find('=');
        if (line.rfind("key", 0) != 0 || eq == std::string_view::npos || line.substr(0, eq).find_first_not_of("key \t") != std::string_view::npos) continue;
        const long code = std::strtol(std::string(line.substr(eq + 1)).c_str(), nullptr, 10);
        if (code != -1 && code != kRuleLockedKeyCode && !(inBite && biteOn)) return true;
    }
    return false;
}

// HSE's own default keys: a section with no key line keeps these, so a file with none of them written isn't locked.
// Read from HSE's source (2026-09). A new default in a later HSE gets through until it's added here and in the launcher.
inline constexpr const char* kHseDefaultKeySections[] = {"Jump", "ToggleCustomGravity", "ToggleSlowMotion", "FreeCamera", "FreeCameraInput", "InfiniteStamina", "SpeedMultiplier"};
inline bool DefaultKeyLeft(std::string_view ini) {
    for (const char* s : kHseDefaultKeySections) if (!RuleIniValue(ini, s, "key")) return true;
    return false;
}

// Every problem in HSE's settings file. The lock means a launch that skipped the launcher fails here too.
inline std::vector<std::string> HseRuleProblems(std::string_view ini) {
    std::vector<std::string> out;
    for (const char* banned : {"InfiniteStamina", "InfiniteConsciousness", "Invulnerability", "CustomBodyTonus", "EnemyInfiniteStamina",
                               "EnemyInfiniteConsciousness", "EnemyNoPain", "EnemyConsciousnessMultiplier", "EnemyRagdoll", "DrunkEnemies", "EnforceOverrides"})
        if (RuleIniValue(ini, banned, "enabled") == "true") out.push_back(std::string(banned) + " is on");
    struct Stat { const char* section; const char* key; };
    static constexpr Stat kStats[] = {{"SpeedMultiplier", "run_speed_multiplier"}, {"SpeedMultiplier", "walk_speed_multiplier"},
                                      {"StrengthMultiplier", "strength_multiplier"}, {"StrengthMultiplier", "grab_force_multiplier"},
                                      {"StrengthMultiplier", "hands_rigidity_multiplier"}, {"KnockbackMultiplier", "knockback_multiplier"},
                                      {"KickMultiplier", "kick_power_multiplier"}, {"BoneControl", "break_strength_multiplier"}, {"BoneControl", "mass_multiplier"}};
    for (const Stat& s : kStats) {
        if (RuleIniValue(ini, s.section, "enabled") != "true") continue;
        const auto v = RuleIniValue(ini, s.section, s.key);
        const double d = v ? std::strtod(v->c_str(), nullptr) : 0.0;
        const double floor = std::string_view(s.key) == "mass_multiplier" ? kRuleMassMin : kRuleStatMin;
        if (!(d >= floor - 1e-6 && d <= kRuleStatMax + 1e-6)) out.push_back(std::string(s.key) + " is out of range");
    }
    const auto jump = RuleIniValue(ini, "Jump", "force");
    const double j = jump ? std::strtod(jump->c_str(), nullptr) : 0.0;
    if (!(j >= kRuleJumpMin - 1e-6 && j <= kRuleJumpMax + 1e-6)) out.push_back("jump is out of range");
    if (RuleIniValue(ini, "Keybinds", "toggle_gui_key") != kRuleLockedMenuKey) out.push_back("the HSE menu is not locked");
    if (RuleIniValue(ini, "UE", "console_enabled") == "true") out.push_back("the console is on");
    const bool bound = AnyHotkeyBound(ini);
    if (bound) out.push_back("an HSE hotkey is bound");
    return out;
}

inline std::vector<std::string> WolverineRuleProblems(std::string_view ini) {
    std::vector<std::string> out;
    const auto mode = RuleIniValue(ini, "Healing", "Mode");
    if (mode != "Off" && mode != "Logan" && mode != "Weathered") out.push_back("Wolverine healing is too high");
    if (RuleIniValue(ini, "Strength", "Enabled") != "Off") out.push_back("Wolverine strength is on");
    return out;
}

// A class as the settings files of HS Enhancer and Half Wolverine describe it. A section that's switched off isn't in force.
inline ClassSheet ClassFromFiles(std::string_view hse, std::string_view wolverine) {
    ClassSheet c;
    const auto mode = RuleIniValue(wolverine, "Healing", "Mode");
    if (mode == "Logan") c.heal = ClassHeal::Logan;
    else if (mode == "Weathered") c.heal = ClassHeal::Weathered;
    c.metalSkeleton = RuleIniValue(wolverine, "Body", "Skeleton") == "Adamantium";
    c.claws = RuleIniValue(wolverine, "Claws", "Enabled") == "On";
    if (c.claws) {                                              // "Same" follows the skeleton
        const auto material = RuleIniValue(wolverine, "Body", "Claws");
        c.metalClaws = material == "Adamantium" || (c.metalSkeleton && material != "Bone");
    }
    c.muscle = RuleIniValue(wolverine, "Muscle mass", "Enabled") == "On";
    c.jump = 0;
    if (hse.empty()) return c;
    const auto on = [&](const char* section) { return RuleIniValue(hse, section, "enabled") == "true"; };
    c.noPain = on("NoPain"); c.quickKicks = on("NoKickCooldown"); c.bite = on("BiteAttack");
    const auto stat = [&](const char* section, const char* key) {
        if (!on(section)) return 1.0;
        const auto v = RuleIniValue(hse, section, key);
        return v ? std::strtod(v->c_str(), nullptr) : 1.0;
    };
    c.strength = stat("StrengthMultiplier", "strength_multiplier"); c.hands = stat("StrengthMultiplier", "hands_rigidity_multiplier");
    c.grip = stat("StrengthMultiplier", "grab_force_multiplier"); c.kick = stat("KickMultiplier", "kick_power_multiplier");
    c.knockback = stat("KnockbackMultiplier", "knockback_multiplier"); c.bones = stat("BoneControl", "break_strength_multiplier");
    c.mass = stat("BoneControl", "mass_multiplier"); c.run = stat("SpeedMultiplier", "run_speed_multiplier"); c.walk = stat("SpeedMultiplier", "walk_speed_multiplier");
    c.noDislocation = on("BoneControl") && RuleIniValue(hse, "BoneControl", "block_dislocation") == "true";
    const auto jump = RuleIniValue(hse, "Jump", "force");
    c.jump = jump ? std::strtod(jump->c_str(), nullptr) : 0.0;
    return c;
}
inline double ClassPoints(std::string_view hse, std::string_view wolverine) { return ClassPoints(ClassFromFiles(hse, wolverine)); }

// A class as another player reports it, by this machine's copy of the rules. HS Enhancer's menu can change anything, so it stays out
// of Classes lobbies, and classes give every player its stats without it.
// What a fighter is really running, for lobbies where a player's class doesn't say: the class, what HS Enhancer's settings file says if it's
// on, and over both the two stats read off the fighter itself, against the game's own values. Self-reported, so it keeps honest chaos
// visible and won't catch someone who builds a cheat. Nothing is read from HS Enhancer's memory, so its updates can't break this.
inline ClassSheet LiveSheet(const ClassSheet& mine, std::string_view hseFile, double runRate, double musclePower) {
    ClassSheet out = mine;
    if (!hseFile.empty()) {
        const ClassSheet f = ClassFromFiles(hseFile, "");
        out.walk = f.walk; out.grip = f.grip; out.hands = f.hands; out.kick = f.kick; out.knockback = f.knockback; out.bones = f.bones; out.mass = f.mass; out.jump = f.jump;
        out.noPain = f.noPain; out.quickKicks = f.quickKicks; out.bite = f.bite; out.noDislocation = f.noDislocation;
    }
    if (runRate > 0) out.run = runRate / kGameRunRate;
    if (musclePower > 0) out.strength = musclePower / kGameMusclePower;
    return out;
}
// Past what even Unrestricted allows: the likeliest cause when a lobby starts crashing.
inline bool LivePastLimits(const ClassSheet& live) { return !ClassProblems(live, true).empty(); }

// `unrestricted`: the lobby has no class budget. `blockHse`: this player's block_hse; the strictest copy of the rules still wins, because
// everyone in the lobby runs this on everyone else.
inline std::vector<std::string> CheckClassValue(std::string_view value, std::string_view cardMods, bool unrestricted = false, bool blockHse = true) {
    std::vector<std::string> out;
    const std::optional<ClassSheet> c = DecodeClass(value);
    if (!c) { out.push_back("no class data"); return out; }
    out = ClassProblems(*c, unrestricted);
    if (blockHse && cardMods.find("HSEnhancer") != std::string_view::npos) out.push_back("HS Enhancer is on");
    if (ClassNeedsWolverine(*c) && cardMods.find("HalfWolverine") == std::string_view::npos) out.push_back("the class needs Half Wolverine");
    return out;
}

// One setting in an INI's text, every other byte kept. A section or key it doesn't have is added.
inline std::string IniWithValue(std::string_view text, std::string_view section, std::string_view key, std::string_view value) {
    const std::string nl = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        return s;
    };
    const std::string head = "[" + std::string(section) + "]";
    bool in = false;
    size_t after = std::string_view::npos;
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        const size_t next = end == std::string_view::npos ? text.size() : end + 1;
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = trim(text.substr(at, end - at));
        if (!line.empty() && line.front() == '[') {
            in = line == head;
            if (in) after = next;
        } else if (in && !line.empty() && line.front() != ';' && line.front() != '#') {
            const size_t eq = line.find('=');
            if (eq != std::string_view::npos && trim(line.substr(0, eq)) == key) {
                const size_t valueAt = text.find('=', at) + 1;
                size_t from = valueAt;
                while (from < end && (text[from] == ' ' || text[from] == '\t')) ++from;
                size_t to = end;
                while (to > from && (text[to - 1] == '\r' || text[to - 1] == ' ' || text[to - 1] == '\t')) --to;
                return std::string(text.substr(0, from)) + std::string(value) + std::string(text.substr(to));
            }
        }
        at = next;
    }
    const std::string add = std::string(key) + " = " + std::string(value) + nl;
    if (after != std::string_view::npos) return std::string(text.substr(0, after)) + add + std::string(text.substr(after));
    std::string out(text);
    if (!out.empty() && out.back() != '\n') out += nl;
    return out + nl + head + nl + add;
}

// ---- HS Enhancer locked for one game (session/hsm_mod_switch.h SwitchPlan::lockHse) -------------------------------------
// A lobby that blocks HS Enhancer takes it locked: still loaded, with nothing it can do. HSE reads its settings file once,
// when the game starts, and never again, so the file is locked before then (gameplay/mod_rules.cpp) and the player's own
// file comes back when the game closes. From HSE's source (2026-11): a "key" line per hotkey (-1: none), an "enabled" line
// per feature that remembers being on, the menu key [Keybinds] toggle_gui_key, the UE console [UE] console_enabled and
// graphics commands at start [Graphics] apply_on_startup. A section with no key line takes HSE's default key, so those
// sections get one. HSE's cosmetic asset overrides stay: they change how things look, not how anyone fights.

namespace online_detail {
inline std::string_view IniTrim(std::string_view s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    return s;
}
}  // namespace online_detail

// HSE's file with every "key" line at -1 and every "enabled" line false, each other byte kept; then the menu key, the
// console, graphics at start and the default-key sections. A UTF-8 mark at the start goes: HSE (SimpleIni) reads the file
// the same without it, and these lines read sections only without it.
inline std::string LockedHseIni(std::string_view ini) {
    if (ini.substr(0, 3) == "\xEF\xBB\xBF") ini.remove_prefix(3);
    std::string out;
    out.reserve(ini.size() + 256);
    size_t at = 0;
    while (at < ini.size()) {
        size_t end = ini.find('\n', at);
        const size_t next = end == std::string_view::npos ? ini.size() : end + 1;
        if (end == std::string_view::npos) end = ini.size();
        const std::string_view line = ini.substr(at, end - at), body = online_detail::IniTrim(line);
        const size_t eq = line.find('=');
        const bool setting = !body.empty() && body.front() != '[' && body.front() != ';' && body.front() != '#' && eq != std::string_view::npos;
        const std::string_view key = setting ? online_detail::IniTrim(line.substr(0, eq)) : std::string_view();
        if (key == "key" || key == "enabled") {
            size_t from = at + eq + 1, to = end;
            while (from < to && (ini[from] == ' ' || ini[from] == '\t')) ++from;
            while (to > from && (ini[to - 1] == '\r' || ini[to - 1] == ' ' || ini[to - 1] == '\t')) --to;
            out.append(ini.substr(at, from - at));
            out += key == "key" ? "-1" : "false";
            out.append(ini.substr(to, next - to));
        } else {
            out.append(ini.substr(at, next - at));
        }
        at = next;
    }
    out = IniWithValue(out, "Keybinds", "toggle_gui_key", kRuleLockedMenuKey);
    out = IniWithValue(out, "UE", "console_enabled", "false");
    out = IniWithValue(out, "Graphics", "apply_on_startup", "false");
    for (const char* section : kHseDefaultKeySections) out = IniWithValue(out, section, "key", "-1");
    return out;
}

// A file LockedHseIni wrote, locked still or changed since: its menu key is still the one no key sends, which a player's
// own file never has. The next start puts the player's copy back over such a file, and leaves any other alone.
inline bool HseIniFromLock(std::string_view ini) { return RuleIniValue(ini, "Keybinds", "toggle_gui_key") == kRuleLockedMenuKey; }

// What keeps HSE's file from being locked; empty: it is. Stricter than HSE reads it: a value that isn't -1, 143 or
// "false" counts as on.
inline std::vector<std::string> HseLockProblems(std::string_view ini) {
    std::vector<std::string> out;
    if (RuleIniValue(ini, "Keybinds", "toggle_gui_key") != kRuleLockedMenuKey) out.push_back("its menu key is set");
    if (RuleIniValue(ini, "UE", "console_enabled") != "false") out.push_back("the console may be on");
    if (RuleIniValue(ini, "Graphics", "apply_on_startup") == "true") out.push_back("graphics commands run at start");
    if (DefaultKeyLeft(ini)) out.push_back("a default hotkey is left");
    bool bound = false;
    std::string section;
    size_t at = 0;
    while (at < ini.size()) {
        size_t end = ini.find('\n', at);
        if (end == std::string_view::npos) end = ini.size();
        const std::string_view line = online_detail::IniTrim(ini.substr(at, end - at));
        at = end + 1;
        if (!line.empty() && line.front() == '[') { section = std::string(line); continue; }
        const size_t eq = line.find('=');
        if (line.empty() || line.front() == ';' || line.front() == '#' || eq == std::string_view::npos) continue;
        const std::string_view key = online_detail::IniTrim(line.substr(0, eq)), value = online_detail::IniTrim(line.substr(eq + 1));
        if (key == "key" && value != "-1" && value != kRuleLockedMenuKey) bound = true;
        if (key == "enabled" && value != "false") out.push_back(section + " is on");
    }
    if (bound) out.push_back("a hotkey is bound");
    return out;
}

// ---- HS Enhancer off at the start of every game ---------------------------------------------------------------------
// Every game starts with HSE's features off (gameplay/mod_rules.cpp writes this before HSE reads its file), so a lobby
// that refuses HS Enhancer needs no restart: HSMMod holds HSE's keys back there (hse_hold.cpp), and nothing is on to begin
// with. The keys stay as the player set them, for solo play and lobbies that allow it, where the player turns on what
// they want again: nothing puts the features back by itself. From HSE's source (2026-11): "enabled" is false when missing, and so are [UE] console_enabled and
// [Graphics] apply_on_startup, so the start adds nothing. [Discord] and [Notifications] keep theirs: they switch nothing.
namespace online_detail {
inline std::string_view WithoutBom(std::string_view ini) { return ini.substr(0, 3) == "\xEF\xBB\xBF" ? ini.substr(3) : ini; }
// A setting the start turns off. `section` as its line reads, brackets and all.
inline bool HseStartTurnsOff(std::string_view section, std::string_view key) {
    if (key == "enabled") return section != "[Discord]" && section != "[Notifications]";
    return (section == "[UE]" && key == "console_enabled") || (section == "[Graphics]" && key == "apply_on_startup");
}
// Each setting line of an INI, in order: its section line, key and value, trimmed, and where the value sits in the text.
template <class F> void ForEachIniSetting(std::string_view ini, F&& f) {
    std::string_view section;
    size_t at = 0;
    while (at < ini.size()) {
        size_t end = ini.find('\n', at);
        const size_t next = end == std::string_view::npos ? ini.size() : end + 1;
        if (end == std::string_view::npos) end = ini.size();
        const std::string_view line = ini.substr(at, end - at), body = IniTrim(line);
        const size_t eq = line.find('=');
        if (!body.empty() && body.front() == '[') section = body;
        else if (eq != std::string_view::npos) {   // a comment line's key starts with ; or #, so it is no setting of HSE's
            const std::string_view value = IniTrim(line.substr(eq + 1));
            const size_t from = static_cast<size_t>(value.data() - ini.data());
            f(section, IniTrim(line.substr(0, eq)), value, from, from + value.size());
        }
        at = next;
    }
}
}  // namespace online_detail

// HSE's file for the start of a game: each setting HseStartTurnsOff false, every other byte kept, a UTF-8 mark gone.
inline std::string HseStartOffIni(std::string_view ini) {
    ini = online_detail::WithoutBom(ini);
    std::string out;
    out.reserve(ini.size());
    size_t copied = 0;
    online_detail::ForEachIniSetting(ini, [&](std::string_view section, std::string_view key, std::string_view, size_t from, size_t to) {
        if (!online_detail::HseStartTurnsOff(section, key)) return;
        out.append(ini.substr(copied, from - copied));
        out += "false";
        copied = to;
    });
    out.append(ini.substr(copied));
    return out;
}

// What is on in HSE's file that the start turns off; empty: nothing. Stricter than HSE reads it: anything but "false" is on.
inline std::vector<std::string> HseStartOffProblems(std::string_view ini) {
    std::vector<std::string> out;
    online_detail::ForEachIniSetting(online_detail::WithoutBom(ini), [&](std::string_view section, std::string_view key, std::string_view value, size_t, size_t) {
        if (!online_detail::HseStartTurnsOff(section, key) || value == "false") return;
        if (key == "enabled") out.push_back(std::string(section) + " is on");
        else if (key == "console_enabled") out.push_back("the console is on");
        else out.push_back("graphics commands run at start");
    });
    return out;
}

// Half Wolverine's file with a class's parts in it. It reads the file once when the game starts, so this is written before then.
inline std::string WolverineFileFor(std::string_view text, const ClassSheet& c) {
    const struct { const char* section; const char* key; const char* value; } set[] = {
        {"Healing", "Mode", c.heal == ClassHeal::Logan ? "Logan" : c.heal == ClassHeal::Weathered ? "Weathered" : "Off"},
        {"Body", "Skeleton", c.metalSkeleton ? "Adamantium" : "Bone"},
        {"Body", "Claws", c.metalClaws ? "Adamantium" : "Bone"},
        {"Claws", "Enabled", c.claws ? "On" : "Off"},
        {"Muscle mass", "Enabled", c.muscle ? "On" : "Off"},
        {"Strength", "Enabled", "Off"},
        {"ScentTrail", "Enabled", "false"},
    };
    std::string out(text);
    for (const auto& s : set) out = IniWithValue(out, s.section, s.key, s.value);
    return out;
}
// Half Wolverine's file gives this class's parts, and nothing the rules refuse.
inline bool WolverineFileMatches(std::string_view text, const ClassSheet& c) {
    const ClassSheet f = ClassFromFiles("", text);
    return f.heal == c.heal && f.metalSkeleton == c.metalSkeleton && f.claws == c.claws && (!c.claws || f.metalClaws == c.metalClaws) && f.muscle == c.muscle && WolverineRuleProblems(text).empty();
}

// A fighter with claws out can't hold a weapon as well.
inline bool ClawsForbidWeapons(std::string_view wolverine) { return RuleIniValue(wolverine, "Claws", "Enabled") == "On"; }
// Weapons go only while the claws are out. A sheathed fighter may hold one. With no way to see the blades (claw_mirror=0, or
// before the fight has started), the claws count as out, so turning that off doesn't let a fighter keep both.
inline bool ClawsDropWeapons(bool clawsAllowed, bool bladesWatched, bool bladesOut) { return clawsAllowed && (!bladesWatched || bladesOut); }

// The settings a client reports for mod_rule=classes: the parts of HSE's and Wolverine's files the rules read, small
// enough for lobby member data. Each machine checks every other player's sheet with its own compiled rules.
constexpr size_t kMaxRuleSheet = 4096;

inline std::string HseSheetFrom(std::string_view ini) {
    std::string out;
    auto put = [&](const char* section, std::initializer_list<const char*> keys) {
        out += "["; out += section; out += "]\n";
        for (const char* k : keys) if (const auto v = RuleIniValue(ini, section, k)) { out += k; out += " = "; out += *v; out += "\n"; }
    };
    for (const char* s : {"InfiniteStamina", "InfiniteConsciousness", "Invulnerability", "CustomBodyTonus", "EnemyInfiniteStamina", "EnemyInfiniteConsciousness",
                          "EnemyNoPain", "EnemyConsciousnessMultiplier", "EnemyRagdoll", "DrunkEnemies", "EnforceOverrides"}) put(s, {"enabled"});
    for (const char* s : {"NoPain", "NoKickCooldown", "BiteAttack"}) put(s, {"enabled"});
    put("SpeedMultiplier", {"enabled", "run_speed_multiplier", "walk_speed_multiplier"});
    put("StrengthMultiplier", {"enabled", "strength_multiplier", "grab_force_multiplier", "hands_rigidity_multiplier"});
    put("KnockbackMultiplier", {"enabled", "knockback_multiplier"});
    put("KickMultiplier", {"enabled", "kick_power_multiplier"});
    put("BoneControl", {"enabled", "break_strength_multiplier", "mass_multiplier", "block_dislocation"});
    put("Jump", {"force"});
    put("UE", {"console_enabled"});
    put("Keybinds", {"toggle_gui_key"});
    // One line stands in for every hotkey.
    const bool bound = AnyHotkeyBound(ini) || DefaultKeyLeft(ini);
    out += bound ? "key = 1\n" : "key = -1\n";
    return out;
}

inline std::string WolverineSheetFrom(std::string_view ini) {
    std::string out;
    const std::pair<const char*, const char*> keys[] = {{"Healing", "Mode"}, {"Body", "Skeleton"}, {"Body", "Claws"}, {"Strength", "Enabled"}, {"Muscle mass", "Enabled"}, {"Claws", "Enabled"}};
    for (const auto& [section, key] : keys) {
        out += "["; out += section; out += "]\n";
        if (const auto v = RuleIniValue(ini, section, key)) { out += key; out += " = "; out += *v; out += "\n"; }
    }
    return out;
}

// "<HSE sheet>\x1f<Wolverine sheet>": an empty part means that mod isn't in use.
inline std::string BuildRuleSheet(const std::string& hseSheet, const std::string& wolverineSheet) { return hseSheet + "\x1f" + wolverineSheet; }

// Everything wrong with a player's report under mod_rule=classes. The card's mod list says what must be accounted for.
inline std::vector<std::string> CheckRuleSheet(std::string_view sheet, std::string_view cardMods) {
    std::vector<std::string> out;
    const size_t cut = sheet.find('\x1f');
    if (sheet.empty() || cut == std::string_view::npos || sheet.size() > kMaxRuleSheet) { out.push_back("no class data"); return out; }
    const std::string_view hse = sheet.substr(0, cut), wolverine = sheet.substr(cut + 1);
    const bool usesHse = cardMods.find("HSEnhancer") != std::string_view::npos, usesWolverine = cardMods.find("HalfWolverine") != std::string_view::npos;
    if (usesHse && hse.empty()) out.push_back("HSE settings missing");
    if (usesWolverine && wolverine.empty()) out.push_back("Wolverine settings missing");
    if (!hse.empty()) for (std::string& p : HseRuleProblems(hse)) out.push_back(std::move(p));
    if (!wolverine.empty()) for (std::string& p : WolverineRuleProblems(wolverine)) out.push_back(std::move(p));
    if (ClassPoints(hse, wolverine) > kRuleMaxPoints + 1e-6) out.push_back("the class is too strong");
    return out;
}

}  // namespace hsm

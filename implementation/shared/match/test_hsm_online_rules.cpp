// Unit tests for hsm_online_rules.h. Run via implementation/shared/run_tests.sh test_hsm_online_rules.
#include "match/hsm_online_rules.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

// What the launcher's lock adds: a key line for each of HSE's default-key sections.
static std::string LockedDefaults() {
    std::string s;
    for (const char* n : kHseDefaultKeySections) s += std::string("[") + n + "]\nkey = 143\n";
    return s;
}

static const char* kGood =
    "[InfiniteStamina]\r\nkey = -1\r\nenabled = false\r\n\r\n[SpeedMultiplier]\r\nkey = -1\r\nenabled = true\r\nrun_speed_multiplier = 0.750000\r\nwalk_speed_multiplier = 0.800000\r\n"
    "[StrengthMultiplier]\r\nenabled = true\r\nstrength_multiplier = 1.250000\r\ngrab_force_multiplier = 1.000000\r\nhands_rigidity_multiplier = 1.000000\r\n"
    "[Jump]\r\nforce = 5000.000000\r\n[UE]\r\nconsole_enabled = false\r\n[Keybinds]\r\ntoggle_gui_key = 143\r\n";

static std::string Swap(std::string s, const std::string& a, const std::string& b) { s.replace(s.find(a), a.size(), b); return s; }

// HSE's menu key, read as HSE reads it: a locked file's 143, the player's own, or Insert.
static void TestHseMenuKey() {
    CHECK(HseMenuKey("") == 0x2D && HseMenuKey("[Keybinds]\r\nunbind_key = 8\r\n") == 0x2D);
    CHECK(HseMenuKey("[Keybinds]\r\ntoggle_gui_key = 143\r\n") == 143);
    CHECK(HseMenuKey(LockedHseIni("[Keybinds]\r\ntoggle_gui_key = 45\r\n")) == 143);
    CHECK(HseMenuKey("\xEF\xBB\xBF[Keybinds]\ntoggle_gui_key=112\n") == 112);
    CHECK(HseMenuKey("[Keybinds]\r\ntoggle_gui_key = 0x70\r\n") == 0x70 && HseMenuKey("[Keybinds]\r\ntoggle_gui_key = 36abc\r\n") == 36);
    for (const char* bad : {"-1", "0", "256", "91", "92", "93", "abc", "", "-45"})
        CHECK(HseMenuKey(std::string("[Keybinds]\r\ntoggle_gui_key = ") + bad + "\r\n") == 0x2D);
    CHECK(HseMenuKey("[Other]\r\ntoggle_gui_key = 112\r\n") == 0x2D);   // only under [Keybinds]
}

// HSE's file locked for one game: LockedHseIni writes it, HseLockProblems checks it.
static void TestHseLock() {
    const std::string real =
        "[Keybinds]\r\ntoggle_gui_key = 45\r\nunbind_key = 8\r\n\r\n[InfiniteStamina]\r\nkey = 73\r\nenabled = true\r\n"
        "[SpeedMultiplier]\r\nkey=80\r\nenabled=true  \r\nrun_speed_multiplier = 3.000000\r\n; key = 5\r\n"
        "[UE]\r\nconsole_enabled = true\r\n[Graphics]\r\napply_on_startup = true\r\nfog = 0\r\n[Discord]\r\nenabled = true\r\n";
    CHECK(!HseLockProblems(real).empty());
    const std::string locked = LockedHseIni(real);
    CHECK(HseLockProblems(locked).empty());
    // Every other byte kept: the params, the unbind key, the comment, the spaces after a value, CRLF line ends.
    CHECK(locked.find("unbind_key = 8\r\n") != std::string::npos && locked.find("run_speed_multiplier = 3.000000\r\n") != std::string::npos);
    CHECK(locked.find("; key = 5\r\n") != std::string::npos && locked.find("fog = 0\r\n") != std::string::npos);
    CHECK(locked.find("[InfiniteStamina]\r\nkey = -1\r\nenabled = false\r\n") != std::string::npos);
    CHECK(locked.find("[SpeedMultiplier]\r\nkey=-1\r\nenabled=false  \r\n") != std::string::npos);
    CHECK(locked.find("toggle_gui_key = 143\r\n") != std::string::npos && locked.find("console_enabled = false\r\n") != std::string::npos);
    CHECK(locked.find("apply_on_startup = false\r\n") != std::string::npos && locked.find("[Discord]\r\nenabled = false\r\n") != std::string::npos);
    CHECK(locked.rfind(real.substr(0, 10), 0) == 0);   // the file starts as it did
    for (const char* section : kHseDefaultKeySections) CHECK(RuleIniValue(locked, section, "key") == "-1");
    for (size_t i = 0; i < locked.size(); ++i) if (locked[i] == '\n') CHECK(i > 0 && locked[i - 1] == '\r');
    CHECK(LockedHseIni(locked) == locked);   // locking twice changes nothing
    CHECK(LockedHseIni("[A]\r\nkey =\r\n").find("[A]\r\nkey =-1\r\n") == 0);   // a key with no value gets one
    // A UTF-8 mark at the start: dropped, and the file locked as without it
    CHECK(LockedHseIni("\xEF\xBB\xBF" + real) == locked && HseLockProblems(LockedHseIni("\xEF\xBB\xBF" + real)).empty());
    CHECK(LockedHseIni("\xEF\xBB").rfind("\xEF\xBB", 0) == 0);   // not the whole mark: kept
    // No file at all: a whole locked one, LF like the empty text.
    const std::string fresh = LockedHseIni("");
    CHECK(HseLockProblems(fresh).empty() && fresh.find('\r') == std::string::npos);
    CHECK(!HseLockProblems("").empty());

    // Each thing that unlocks it, named.
    const auto one = [](const std::string& text, const char* problem) {
        const std::vector<std::string> v = HseLockProblems(text);
        return v.size() == 1 && v[0] == problem;
    };
    CHECK(one(Swap(locked, "toggle_gui_key = 143", "toggle_gui_key = 45"), "its menu key is set"));
    CHECK(one(Swap(locked, "console_enabled = false", "console_enabled = true"), "the console may be on"));
    CHECK(one(Swap(locked, "console_enabled = false", "console_enabled = 1"), "the console may be on"));   // HSE reads 1 as on
    CHECK(one(Swap(locked, "apply_on_startup = false", "apply_on_startup = true"), "graphics commands run at start"));
    CHECK(one(Swap(locked, "[InfiniteStamina]\r\nkey = -1", "[InfiniteStamina]\r\nkey = 73"), "a hotkey is bound"));
    CHECK(one(Swap(locked, "[InfiniteStamina]\r\nkey = -1", "[InfiniteStamina]\r\nkey = "), "a hotkey is bound"));   // HSE would take its default
    CHECK(one(Swap(locked, "[SpeedMultiplier]\r\nkey=-1\r\nenabled=false", "[SpeedMultiplier]\r\nkey=-1\r\nenabled=true"), "[SpeedMultiplier] is on"));
    CHECK(one(Swap(locked, "[InfiniteStamina]\r\nkey = -1\r\nenabled = false", "[InfiniteStamina]\r\nkey = -1\r\nenabled = yes"), "[InfiniteStamina] is on"));
    CHECK(one(Swap(locked, "[Jump]\r\nkey = -1\r\n", ""), "a default hotkey is left"));
    // Two bound keys: one problem. A key on 143, which no key sends, is as good as none. Comments don't count.
    CHECK(one(Swap(Swap(locked, "[InfiniteStamina]\r\nkey = -1", "[InfiniteStamina]\r\nkey = 73"), "key=-1", "key=80"), "a hotkey is bound"));
    CHECK(HseLockProblems(Swap(locked, "[InfiniteStamina]\r\nkey = -1", "[InfiniteStamina]\r\nkey = 143")).empty());
    CHECK(HseLockProblems(locked + "; enabled = true\r\n# key = 5\r\n").empty());
    // Graphics at start is off when the file doesn't say; the console must say off.
    CHECK(HseLockProblems(Swap(locked, "apply_on_startup = false\r\n", "")).empty());
    CHECK(one(Swap(locked, "console_enabled = false\r\n", ""), "the console may be on"));

    // HseIniFromLock: the menu key alone tells a file the lock wrote, changed since or not
    CHECK(HseIniFromLock(locked) && HseIniFromLock(fresh) && HseIniFromLock(Swap(locked, "[SpeedMultiplier]\r\nkey=-1", "[SpeedMultiplier]\r\nkey=80")));
    CHECK(!HseIniFromLock(real) && !HseIniFromLock("") && !HseIniFromLock(Swap(locked, "toggle_gui_key = 143", "toggle_gui_key = 45")));
    CHECK(!HseIniFromLock("[Other]\r\ntoggle_gui_key = 143\r\n"));
}

// HSE off at the start of every game: HseStartOffIni writes it, HseStartOffProblems checks it.
static void TestHseStartOff() {
    const std::string real =
        "[Keybinds]\r\ntoggle_gui_key = 45\r\nunbind_key = 8\r\n\r\n[InfiniteStamina]\r\nkey = 73\r\nenabled = true\r\n"
        "[SpeedMultiplier]\r\nkey=80\r\nenabled=true  \r\nrun_speed_multiplier = 3.000000\r\n; enabled = true\r\n"
        "[UE]\r\nconsole_enabled = true\r\n[Graphics]\r\napply_on_startup = true\r\nfog = 0\r\n[Discord]\r\nenabled = true\r\n"
        "[Notifications]\r\nenabled = true\r\n[Jump]\r\nenabled = false\r\n";
    CHECK(HseStartOffProblems(real).size() == 4);
    const std::string off = HseStartOffIni(real);
    CHECK(HseStartOffProblems(off).empty());
    // Only the features, the console and graphics at start: keys, the menu key, params, comments, CRLF all as they were
    CHECK(off == Swap(Swap(Swap(Swap(real, "key = 73\r\nenabled = true", "key = 73\r\nenabled = false"), "enabled=true  ", "enabled=false  "),
                           "console_enabled = true", "console_enabled = false"), "apply_on_startup = true", "apply_on_startup = false"));
    CHECK(RuleIniValue(off, "Keybinds", "toggle_gui_key") == "45" && RuleIniValue(off, "InfiniteStamina", "key") == "73");
    CHECK(RuleIniValue(off, "Discord", "enabled") == "true" && RuleIniValue(off, "Notifications", "enabled") == "true");
    CHECK(off.find("; enabled = true\r\n") != std::string::npos);
    CHECK(HseStartOffIni(off) == off && HseStartOffIni("") == "");   // twice: the same; no file: nothing added
    CHECK(HseStartOffIni("\xEF\xBB\xBF" + real) == off);   // a UTF-8 mark goes
    CHECK(HseStartOffIni("[A]\nenabled =\n") == "[A]\nenabled =false\n");   // no value: one
    CHECK(HseStartOffIni("enabled = true\n") == "enabled = false\n");   // before any section: off too
    CHECK(HseStartOffIni("[UE]\nconsole_enabled = true\n[Other]\nconsole_enabled = true\napply_on_startup = true\n") ==
          "[UE]\nconsole_enabled = false\n[Other]\nconsole_enabled = true\napply_on_startup = true\n");   // those two only where HSE reads them

    // Each thing still on, named; anything but "false" is on (HSE reads 1, yes and on as true)
    const auto one = [](const std::string& text, const char* problem) {
        const std::vector<std::string> v = HseStartOffProblems(text);
        return v.size() == 1 && v[0] == problem;
    };
    CHECK(one(Swap(off, "key = 73\r\nenabled = false", "key = 73\r\nenabled = 1"), "[InfiniteStamina] is on"));
    CHECK(one(Swap(off, "enabled=false  ", "enabled=yes"), "[SpeedMultiplier] is on"));
    CHECK(one(Swap(off, "console_enabled = false", "console_enabled = on"), "the console is on"));
    CHECK(one(Swap(off, "apply_on_startup = false", "apply_on_startup = true"), "graphics commands run at start"));
    CHECK(HseStartOffProblems("\xEF\xBB\xBF[Discord]\r\nenabled = true\r\n").empty());   // Discord first, after a mark: still Discord
    CHECK(HseStartOffProblems("").empty() && HseStartOffProblems("[A]\r\nkey = 73\r\n").empty());   // keys are held, not checked

    CHECK(HseStartOffIni("[A]\nenabled =  \r\n") == "[A]\nenabled =false  \r\n");   // only spaces: the value goes after the =
}

int main() {
    TestHseMenuKey();
    TestHseLock();
    TestHseStartOff();
    CHECK(HseRuleProblems(kGood).empty());
    const std::string good = kGood;
    CHECK(!HseRuleProblems(Swap(good, "key = -1\r\nenabled = false", "key = 73\r\nenabled = false")).empty());     // hotkey bound: not locked
    CHECK(!HseRuleProblems(Swap(good, "enabled = false", "enabled = true")).empty());                              // infinite stamina
    CHECK(!HseRuleProblems(Swap(good, "strength_multiplier = 1.250000", "strength_multiplier = 10.000000")).empty());
    CHECK(!HseRuleProblems(Swap(good, "strength_multiplier = 1.250000", "strength_multiplier = nan")).empty());    // NaN is out of range
    CHECK(!HseRuleProblems(Swap(good, "force = 5000.000000", "force = 12500.000000")).empty());
    CHECK(!HseRuleProblems(Swap(good, "toggle_gui_key = 143", "toggle_gui_key = 45")).empty());                    // launcher skipped
    CHECK(!HseRuleProblems("").empty());                                                                           // no settings at all
    // A value out of range in a section that is off doesn't count.
    CHECK(HseRuleProblems(good + "[KickMultiplier]\r\nenabled = false\r\nkick_power_multiplier = 35.000000\r\n").empty());

    const std::string wolv = "[Healing]\r\nMode = Logan\r\n[Body]\r\nSkeleton = Bone\r\n[Strength]\r\nEnabled = Off\r\n[Muscle mass]\r\nEnabled = Off\r\n[Claws]\r\nEnabled = Off\r\n";
    CHECK(WolverineRuleProblems(wolv).empty());
    CHECK(!WolverineRuleProblems(Swap(wolv, "Mode = Logan", "Mode = WeaponX")).empty());
    CHECK(!WolverineRuleProblems(Swap(wolv, "[Strength]\r\nEnabled = Off", "[Strength]\r\nEnabled = On")).empty());
    // Claws and a metal skeleton are allowed, but they have to be paid for.
    const std::string clawed = Swap(wolv, "[Claws]\r\nEnabled = Off", "[Claws]\r\nEnabled = On");
    CHECK(WolverineRuleProblems(clawed).empty() && ClawsForbidWeapons(clawed) && !ClawsForbidWeapons(wolv));
    CHECK(ClassPoints("", Swap(clawed, "Skeleton = Bone", "Skeleton = Adamantium")) > ClassPoints("", clawed) + 0.9);
    CHECK(!CheckRuleSheet(BuildRuleSheet("", clawed), "HalfWolverine").empty());

    // The sheet: a good class passes, and each kind of cheat fails on the other player's machine.
    const std::string fullHse = good + "[Dash]\r\nkey = -1\r\n";
    CHECK(CheckRuleSheet(BuildRuleSheet(HseSheetFrom(fullHse + LockedDefaults()), WolverineSheetFrom(Swap(wolv, "Mode = Logan", "Mode = Off"))), "HSEnhancer, HalfWolverine").empty());
    CHECK(CheckRuleSheet(BuildRuleSheet("", ""), "WillieFace").empty());                                  // Fair: nothing to report
    CHECK(!CheckRuleSheet("", "").empty());                                                                // sent nothing
    CHECK(!CheckRuleSheet(BuildRuleSheet("", ""), "HSEnhancer").empty());                                  // hides HSE's settings
    CHECK(!CheckRuleSheet(BuildRuleSheet(HseSheetFrom(Swap(fullHse, "strength_multiplier = 1.250000", "strength_multiplier = 10.000000")), ""), "HSEnhancer").empty());
    CHECK(!CheckRuleSheet(BuildRuleSheet(HseSheetFrom(fullHse + "[NPCSpawnBinding_1]\r\nkey = 78\r\n"), ""), "HSEnhancer").empty());   // a hotkey left bound
    CHECK(!CheckRuleSheet(BuildRuleSheet("", WolverineSheetFrom(Swap(wolv, "Mode = Logan", "Mode = WeaponX"))), "HalfWolverine").empty());
    CHECK(HseSheetFrom(fullHse).size() < kMaxRuleSheet);
    // The budget: an edited file with every stat at the maximum is caught even though each value is in range.
    const std::string on = "[SpeedMultiplier]\nenabled = true\nrun_speed_multiplier = 1.250000\nwalk_speed_multiplier = 1.250000\n"
                           "[StrengthMultiplier]\nenabled = true\nstrength_multiplier = 1.250000\ngrab_force_multiplier = 1.250000\nhands_rigidity_multiplier = 1.250000\n"
                           "[Jump]\nforce = 6000.000000\n[UE]\nconsole_enabled = false\n[Keybinds]\ntoggle_gui_key = 143\nkey = -1\n";
    CHECK(ClassPoints(on, "") > kRuleMaxPoints);   // over the limit, whatever the limit is set to
    CHECK(!CheckRuleSheet(BuildRuleSheet(HseSheetFrom(on), ""), "HSEnhancer").empty());
    // The launcher's classes, value for value: run, walk, str, grab, hands, knock, kick, bones, mass, jump, power x100.
    struct Class { const char* name; const char* heal; bool pain, kickCd, bite, disloc; double run, walk, str, grab, hands, knock, kick, brk, mass, jump, power; };
    static constexpr Class kClasses[] = {
        {"Revenant", "Logan", false, false, false, false, 1.15, 1.15, 0.80, 1.00, 1.00, 0.80, 0.80, 0.70, 0.70, 5000, 12.75},
        {"Barber-Surgeon", "Weathered", false, false, false, false, 0.80, 0.80, 0.70, 1.00, 1.00, 0.70, 0.70, 1.05, 1.00, 5000, 5.5},
        {"Crusader", "Logan", false, false, false, false, 0.75, 0.80, 1.25, 1.00, 0.85, 0.80, 0.80, 0.85, 1.00, 4000, -9.75},
        {"Pugilist", "Off", false, true, true, false, 0.90, 0.90, 0.75, 1.60, 1.60, 1.25, 1.50, 0.80, 0.80, 5000, -10.5},
        {"Footpad", "Off", false, true, false, false, 1.50, 1.15, 0.80, 1.00, 1.00, 0.70, 0.85, 0.70, 0.70, 5000, 12.25},
        {"Knight", "Off", false, false, false, true, 0.70, 0.70, 0.85, 1.00, 1.00, 0.85, 0.70, 1.50, 1.50, 4000, 11.5},
        {"Flagellant", "Off", true, false, false, false, 0.70, 0.70, 1.10, 0.70, 0.70, 1.30, 1.30, 0.70, 0.70, 5000, 7.0},
    };
    auto classIni = [](const Class& c) {
        char b[900];
        std::snprintf(b, sizeof b, "[NoPain]\nenabled = %s\n[NoKickCooldown]\nenabled = %s\n[BiteAttack]\nkey = %s\nenabled = %s\n"
            "[SpeedMultiplier]\nenabled = true\nrun_speed_multiplier = %f\nwalk_speed_multiplier = %f\n"
            "[StrengthMultiplier]\nenabled = true\nstrength_multiplier = %f\ngrab_force_multiplier = %f\nhands_rigidity_multiplier = %f\n"
            "[KnockbackMultiplier]\nenabled = true\nknockback_multiplier = %f\n[KickMultiplier]\nenabled = true\nkick_power_multiplier = %f\n"
            "[BoneControl]\nenabled = true\nbreak_strength_multiplier = %f\nmass_multiplier = %f\nblock_dislocation = %s\n"
            "[Jump]\nforce = %f\n[UE]\nconsole_enabled = false\n[Keybinds]\ntoggle_gui_key = 143\n",
            c.pain ? "true" : "false", c.kickCd ? "true" : "false", c.bite ? "72" : "-1", c.bite ? "true" : "false", c.run, c.walk, c.str, c.grab, c.hands, c.knock, c.kick, c.brk, c.mass, c.disloc ? "true" : "false", c.jump);
        return std::string(b);
    };
    for (const Class& c : kClasses) {
        const std::string hseIni = classIni(c), wolvIni = Swap(wolv, "Mode = Logan", std::string("Mode = ") + c.heal);
        CHECK(std::abs(ClassPoints(hseIni, wolvIni) * 100 - c.power) < 0.01);
        CHECK(CheckRuleSheet(BuildRuleSheet(HseSheetFrom(hseIni + LockedDefaults()), WolverineSheetFrom(wolvIni)), "HSEnhancer, HalfWolverine").empty());
    }
    // Run and walk pay as a pair.
    auto speedOnly = [](const char* run, const char* walk) { return std::string("[SpeedMultiplier]\nenabled = true\nrun_speed_multiplier = ") + run + "\nwalk_speed_multiplier = " + walk + "\n"; };
    CHECK(std::abs(ClassPoints(speedOnly("0.100000", "1.250000"), "") - 0.375) < 0.001);      // run 10%, walk 125%: run earns nothing
    CHECK(std::abs(ClassPoints(speedOnly("0.700000", "1.000000"), "") + 0.075) < 0.001);      // run alone: only the gap
    CHECK(std::abs(ClassPoints(speedOnly("1.000000", "0.700000"), "") + 0.15) < 0.001);       // walk alone: only the gap, at walk's rate
    CHECK(std::abs(ClassPoints(speedOnly("0.700000", "0.700000"), "") + 0.675) < 0.001);      // both down: full
    CHECK(ClassPoints(speedOnly("0.100000", "0.100000"), "") == ClassPoints(speedOnly("0.700000", "0.700000"), ""));   // past 70% earns no more
    // Jump is flavour: dropping it buys nothing. The cap is 200%.
    const std::string paladin = classIni(kClasses[2]);
    CHECK(ClassPoints(Swap(paladin, "force = 4000.000000", "force = 0.000000"), wolv) == ClassPoints(paladin, wolv));
    CHECK(HseRuleProblems(Swap(paladin, "strength_multiplier = 1.250000", "strength_multiplier = 2.000000")).empty());
    CHECK(!HseRuleProblems(Swap(paladin, "strength_multiplier = 1.250000", "strength_multiplier = 2.010000")).empty());
    // Only a class with Bite on keeps Bite's key, and no other key stays bound.
    const std::string brawler = classIni(kClasses[3]);
    CHECK(!AnyHotkeyBound(brawler) && HseRuleProblems(brawler).empty());
    CHECK(AnyHotkeyBound(Swap(brawler, "key = 72\nenabled = true", "key = 72\nenabled = false")));
    CHECK(AnyHotkeyBound(brawler + "[NoPain]\nkey = 73\n"));
    CHECK(!AnyHotkeyBound("[NoPain]\nkey = 143\n[Dash]\nkey = -1\n"));   // the lock's key code is no key
    // Claws cost by what they're made of, whatever the skeleton is, and the sheet carries that to the host.
    auto clawCost = [](const char* body) { return ClassPoints("", std::string("[Body]\n") + body + "[Claws]\nEnabled = On\n"); };
    CHECK(std::abs(clawCost("Skeleton = Bone\nClaws = Bone\n") - 0.60) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Bone\nClaws = Adamantium\n") - 1.80) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Bone\nClaws = Same\n") - 0.60) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Adamantium\nClaws = Bone\n") - 1.40) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Adamantium\nClaws = Same\n") - 2.60) < 0.001);
    CHECK(WolverineSheetFrom("[Body]\nSkeleton = Bone\nClaws = Adamantium\n").find("Claws = Adamantium") != std::string::npos);
    // A section with no key line keeps HSE's default key, as J for Jump did, and the sheet says so.
    std::string allKeys;
    for (const char* s : kHseDefaultKeySections) allKeys += std::string("[") + s + "]\nkey = 143\n";
    CHECK(!DefaultKeyLeft(allKeys));
    CHECK(DefaultKeyLeft("[Jump]\nforce = 5000.000000\n"));
    CHECK(HseSheetFrom("[Jump]\nforce = 5000.000000\n[Keybinds]\ntoggle_gui_key = 143\n").find("key = 1\n") != std::string::npos);
    CHECK(HseSheetFrom(allKeys).find("key = -1\n") != std::string::npos);
    // Any other mod on the list is the player's business: only the two class mods are read from it.
    const RuleMods listed = ReadRuleMods("Keybinds, HSEnhancer, HulkMod, HalfWolverine");
    CHECK(listed.hse && listed.wolverine);
    CHECK(!ReadRuleMods("Keybinds, HulkMod").hse && !ReadRuleMods("Keybinds, HulkMod").wolverine && !ReadRuleMods("").hse);
    CHECK(!ReadRuleMods("HSEnhancerX, XHalfWolverine").hse && !ReadRuleMods("HSEnhancerX, XHalfWolverine").wolverine);   // whole names only
    CHECK(LooksLikeHseLoader("xx HSE_Initialize yy") && !LooksLikeHseLoader("a chainloader"));
    // Sheathed claws leave the hands free; unseen claws don't.
    CHECK(ClawsDropWeapons(true, true, true) && !ClawsDropWeapons(true, true, false) && ClawsDropWeapons(true, false, false));
    CHECK(!ClawsDropWeapons(false, true, true) && !ClawsDropWeapons(false, false, false));
    if (g_fail == 0) std::printf("ok\n");
    return g_fail != 0;
}

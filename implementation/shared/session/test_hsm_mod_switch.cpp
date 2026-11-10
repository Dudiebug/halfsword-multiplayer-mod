// Unit tests for hsm_mod_switch.h. Run via implementation/shared/run_tests.sh test_hsm_mod_switch.
#include "session/hsm_mod_switch.h"
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static Sha256Digest Sha(uint8_t seed) { Sha256Digest d{}; for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(seed + i * 7); return d; }
static ModItem Item(ModKind k, const char* name, uint8_t seed = 1) {
    ModItem it; it.kind = k; it.name = name;
    if (seed) { it.hashed = true; it.sha = Sha(seed); }
    return it;
}
static ModReport Report(std::vector<ModItem> items, bool complete = true) {
    ModReport r; r.items = std::move(items); r.ue4ss = true; r.complete = complete; NormalizeReport(r); return r;
}
static ModRule Rule(ModPreset p, bool blockHse = true) { ModRule r; r.preset = p; r.blockHse = blockHse; return r; }
static bool Has(const std::vector<std::string>& v, const char* s) { for (const std::string& x : v) if (x == s) return true; return false; }

static void TestPlanByKind() {
    const ModReport mine = Report({Item(ModKind::Ue4ssMod, "HalfWolverine"), Item(ModKind::MapPak, "Arena_P.pak", 2), Item(ModKind::ContentPak, "Skins.pak", 3),
                                   Item(ModKind::Proxy, "version.dll", 4), Item(ModKind::Ue4ssStock, "Keybinds", 0)});
    SwitchPlan v = PlanSwitch(Rule(ModPreset::Vanilla), mine);
    CHECK(v.Switchable() && v.Count() == 4);
    CHECK(v.modsTxt.size() == 1 && v.modsTxt[0] == "HalfWolverine");
    CHECK(v.paks.size() == 2 && Has(v.paks, "Arena_P.pak") && Has(v.paks, "Skins.pak"));
    CHECK(v.exeDlls.size() == 1 && v.exeDlls[0] == "version.dll");
    CHECK(v.blockers.empty());

    SwitchPlan m = PlanSwitch(Rule(ModPreset::MapsOnly), mine);   // map paks and proxies pass; the content pak and the UE4SS mod go
    CHECK(m.Switchable() && m.Count() == 2 && m.paks.size() == 1 && m.paks[0] == "Skins.pak" && m.modsTxt.size() == 1 && m.exeDlls.empty());

    for (ModPreset p : {ModPreset::Anything, ModPreset::Classes, ModPreset::Unrestricted}) {
        SwitchPlan a = PlanSwitch(Rule(p), mine);
        CHECK(a.Empty() && !a.Switchable());
    }
}

static void TestPlanHseLock() {
    // HS Enhancer is never turned off and never a blocker: where the rule refuses it, the plan locks it (once, however it shows up)
    const ModReport hse = Report({Item(ModKind::Module, "HSEnhancer", 0), Item(ModKind::Ue4ssMod, "HSEnhancer"), Item(ModKind::Ue4ssMod, "Other")});
    SwitchPlan b = PlanSwitch(Rule(ModPreset::Vanilla), hse);
    CHECK(b.Switchable() && b.lockHse && b.blockers.empty());
    CHECK(b.modsTxt.size() == 1 && b.modsTxt[0] == "Other" && b.Count() == 1);
    const ModReport hseMod = Report({Item(ModKind::Ue4ssMod, "HSEnhancer"), Item(ModKind::Ue4ssMod, "Other")});
    SwitchPlan no = PlanSwitch(Rule(ModPreset::MapsOnly, true), hseMod);
    CHECK(no.Switchable() && no.lockHse && no.modsTxt.size() == 1 && no.modsTxt[0] == "Other");
    // Anything with HS Enhancer blocked: the lock alone, still a restart
    SwitchPlan any = PlanSwitch(Rule(ModPreset::Anything), hse);
    CHECK(any.Switchable() && any.lockHse && any.Count() == 0 && !any.Empty());
    CHECK(PlanSwitch(Rule(ModPreset::Anything, false), hse).Empty());
    // Allowed by the switch but not by the preset (Maps only takes no UE4SS mod, Vanilla no module): locked, not turned off
    SwitchPlan maps = PlanSwitch(Rule(ModPreset::MapsOnly, false), hseMod);
    CHECK(maps.Switchable() && maps.lockHse && maps.modsTxt.size() == 1 && maps.modsTxt[0] == "Other");
    SwitchPlan van = PlanSwitch(Rule(ModPreset::Vanilla, false), hse);
    CHECK(van.Switchable() && van.lockHse && van.blockers.empty() && van.modsTxt.size() == 1);
    // A pak that only carries the name is not HS Enhancer: turned off like any other
    SwitchPlan named = PlanSwitch(Rule(ModPreset::Vanilla), Report({Item(ModKind::ContentPak, "HSEnhancer", 5)}));
    CHECK(named.Switchable() && !named.lockHse && named.paks.size() == 1);
    // A module that isn't HS Enhancer (a peer's report): nothing to rename or lock, so a blocker
    SwitchPlan other = PlanSwitch(Rule(ModPreset::Vanilla), Report({Item(ModKind::Module, "SomeModule", 0)}));
    CHECK(!other.Switchable() && !other.lockHse && other.modsTxt.empty() && other.blockers.size() == 1 && other.blockers[0] == "SomeModule");
    // This game locked it already: it passes every rule, nothing more to plan for it
    ModReport locked = hse;
    LockReportHse(locked);
    CHECK(!HasHse(locked) && HasLockedHse(locked));
    for (ModPreset p : {ModPreset::Vanilla, ModPreset::MapsOnly, ModPreset::Bundle, ModPreset::Custom, ModPreset::Anything, ModPreset::Classes})
        CHECK(!PlanSwitch(Rule(p), locked).lockHse);
    CHECK(PlanSwitch(Rule(ModPreset::Anything), locked).Empty());
}

static void TestPlanBlockers() {
    // A required mod this machine lacks can't be fixed by turning things off
    ModRule custom = Rule(ModPreset::Custom);
    custom.list.push_back({"NeededMod", "", true});
    const ModReport mine = Report({Item(ModKind::Ue4ssMod, "Extra")});
    SwitchPlan c = PlanSwitch(custom, mine);
    CHECK(!c.Switchable() && c.modsTxt.size() == 1 && c.blockers.size() == 1 && c.blockers[0] == "NeededMod (this lobby needs it)");
    // The same list with the mod present and nothing extra: nothing to do
    ModAllow need{"NeededMod", "", true};
    ModRule custom2 = Rule(ModPreset::Custom); custom2.list.push_back(need);
    CHECK(PlanSwitch(custom2, Report({Item(ModKind::Ue4ssMod, "NeededMod")})).Empty());
}

static void TestPlanPendingAndUnchecked() {
    const ModReport pending = Report({Item(ModKind::Ue4ssMod, "X")}, false);
    CHECK(PlanSwitch(Rule(ModPreset::Vanilla), pending).Empty());
    ModRule hashOnly = Rule(ModPreset::Custom); hashOnly.listHashOnly = true; hashOnly.list.push_back({"a", "", false});
    CHECK(PlanSwitch(hashOnly, Report({Item(ModKind::Ue4ssMod, "X")})).Empty());   // the host checks: no plan
    ModReport cut = Report({Item(ModKind::Ue4ssMod, "X")}); cut.truncated = true;
    SwitchPlan t = PlanSwitch(Rule(ModPreset::Vanilla), cut);
    CHECK(!t.Switchable() && t.blockers.size() == 1 && t.Count() == 0);
}

static void TestSummaryAndTag() {
    SwitchPlan p;
    p.modsTxt = {"HalfWolverine", "WillieFace"}; p.paks = {"Skins.pak"}; p.exeDlls = {"version.dll"};
    std::vector<std::string> all = SwitchSummary(p, 10);
    CHECK(all.size() == 4 && all[0] == "HalfWolverine (UE4SS mod)" && all[1] == "WillieFace (UE4SS mod)" && all[2] == "Skins.pak" && all[3] == "version.dll");
    std::vector<std::string> cut = SwitchSummary(p, 3);
    CHECK(cut.size() == 3 && cut[0] == "HalfWolverine (UE4SS mod)" && cut[1] == "WillieFace (UE4SS mod)" && cut[2] == "+2 more");
    CHECK(SwitchSummary(p, 4).size() == 4 && SwitchSummary(p, 4)[3] == "version.dll");   // exactly fits: no "+N more"
    CHECK(SwitchSummary(p, 0).size() == 4);          // 0: no limit
    std::vector<std::string> one = SwitchSummary(p, 1);
    CHECK(one.size() == 1 && one[0] == "+4 more");
    CHECK(SwitchTag(p) == "Restart: 4 mods off");
    SwitchPlan single; single.paks = {"a.pak"};
    CHECK(SwitchTag(single) == "Restart: 1 mod off");
    // The HS Enhancer lock: its own line first, and in the tag
    SwitchPlan lockOnly; lockOnly.lockHse = true;
    CHECK(lockOnly.Switchable() && !lockOnly.Empty() && lockOnly.Count() == 0);
    CHECK(SwitchTag(lockOnly) == "Restart: HS Enhancer locked");
    CHECK(SwitchSummary(lockOnly, 6).size() == 1 && SwitchSummary(lockOnly, 6)[0] == kHseLockLine);
    SwitchPlan both = p; both.lockHse = true;
    CHECK(SwitchTag(both) == "Restart: 4 mods off, HSE locked");
    std::vector<std::string> bl = SwitchSummary(both, 3);
    CHECK(bl.size() == 3 && bl[0] == kHseLockLine && bl[1] == "HalfWolverine (UE4SS mod)" && bl[2] == "+3 more");
    SwitchPlan oneLock = single; oneLock.lockHse = true;
    CHECK(SwitchTag(oneLock) == "Restart: 1 mod off, HSE locked");
    CHECK(SwitchWords(single) == "1 mod off" && SwitchWords(p) == "4 mods off" && SwitchWords(SwitchPlan{}) == "0 mods off");
    CHECK(SwitchWords(lockOnly) == "HS Enhancer locked" && SwitchWords(oneLock) == "1 mod off and HS Enhancer locked");
    CHECK(SwitchWords(both) == "4 mods off and HS Enhancer locked");
}

static void TestPakRel() {
    CHECK(PakRelOk("~mods/Skins.pak") && PakRelOk("Arena_P.pak") && PakRelOk("~mods/Pack/deep/x.pak") && PakRelOk("~mods\\win\\x.pak"));
    CHECK(!PakRelOk("") && !PakRelOk("/abs.pak") && !PakRelOk("\\abs.pak") && !PakRelOk("C:/x.pak") && !PakRelOk("../up.pak") && !PakRelOk("~mods/../x.pak"));
    CHECK(!PakRelOk("~mods//x.pak") && !PakRelOk("./x.pak") && !PakRelOk("x.txt") && !PakRelOk(".pak") && !PakRelOk("a\x01.pak") && !PakRelOk("x.pak\r"));
    CHECK(!PakRelOk(std::string(600, 'a') + ".pak"));
}

static void TestSwitchFile() {
    SwitchFile f; f.pid = 4242; f.paks = {"~mods/Skins.pak", "../evil.pak", "Arena_P.pak"};
    const std::string text = EncodeSwitchFile(f);
    CHECK(text == "pid 4242\npak ~mods/Skins.pak\npak Arena_P.pak\n");
    SwitchFile back;
    CHECK(ParseSwitchFile(text, back) && back.pid == 4242 && back.paks.size() == 2 && back.paks[0] == "~mods/Skins.pak" && back.paks[1] == "Arena_P.pak");
    CHECK(ParseSwitchFile("pid 7\r\npak a.pak\r\n", back) && back.pid == 7 && back.paks.size() == 1 && back.paks[0] == "a.pak");
    CHECK(ParseSwitchFile("pid 7\npak /abs.pak\npak C:\\x.pak\nnoise\n", back) && back.pid == 7 && back.paks.empty());
    CHECK(!ParseSwitchFile("", back) && !ParseSwitchFile("pak a.pak\n", back) && !ParseSwitchFile("pid \n", back) && !ParseSwitchFile("pid 12x\n", back));
    CHECK(!ParseSwitchFile("pid 99999999999\n", back));
    CHECK(ParseSwitchFile("pid 1\npid 2\n", back) && back.pid == 1);   // the first pid counts
    SwitchFile none; none.pid = 3;
    CHECK(EncodeSwitchFile(none) == "pid 3\n" && ParseSwitchFile(EncodeSwitchFile(none), back) && back.pid == 3 && back.paks.empty() && !back.lockHse);
    // The HS Enhancer lock: one exact line
    SwitchFile lock; lock.pid = 9; lock.lockHse = true; lock.paks = {"a.pak"};
    CHECK(EncodeSwitchFile(lock) == "pid 9\npak a.pak\nhse lock\n");
    CHECK(ParseSwitchFile(EncodeSwitchFile(lock), back) && back.lockHse && back.paks.size() == 1);
    CHECK(ParseSwitchFile("pid 9\r\nhse lock\r\n", back) && back.lockHse && back.paks.empty());
    CHECK(ParseSwitchFile("pid 9\nhse locked\nhse\nHSE LOCK\n", back) && !back.lockHse);
    CHECK(ParseSwitchFile(text, back) && !back.lockHse);
}

static void TestAsideLines() {
    AsideEntry e;
    CHECK(ParseAsideLine("version.dll", e) && !e.pak && e.name == "version.dll");
    CHECK(ParseAsideLine("pak ~mods/Skins.pak", e) && e.pak && e.name == "~mods/Skins.pak");
    CHECK(ParseAsideLine("pak Arena_P.pak\r", e) && e.pak && e.name == "Arena_P.pak");
    CHECK(!ParseAsideLine("", e) && !ParseAsideLine("..\\x.dll", e) && !ParseAsideLine("sub/x.dll", e) && !ParseAsideLine("C:x.dll", e));
    CHECK(!ParseAsideLine("pak ../x.pak", e) && !ParseAsideLine("pak /x.pak", e) && !ParseAsideLine("pak x.dll", e));
    CHECK(AsideLine({false, "version.dll"}) == "version.dll\n" && AsideLine({true, "~mods/a.pak"}) == "pak ~mods/a.pak\n");
    AsideEntry rt;
    CHECK(ParseAsideLine("pak ~mods/a.pak", rt) && AsideLine(rt) == "pak ~mods/a.pak\n");
}

static void TestRejoinFile() {
    RejoinFile f; f.lobby = 109775243123456789ull; f.until = 1700000600; f.password = "sword fight";
    const std::string text = EncodeRejoinFile(f);
    CHECK(text == "lobby 109775243123456789\nuntil 1700000600\npass sword fight\n");
    RejoinFile back;
    CHECK(ParseRejoinFile(text, 1700000000, back) && back.lobby == f.lobby && back.until == f.until && back.password == "sword fight");
    CHECK(ParseRejoinFile(text, 1700000600, back));      // the deadline itself still counts
    CHECK(!ParseRejoinFile(text, 1700000601, back));     // expired
    RejoinFile open; open.lobby = 5; open.until = 10;
    CHECK(EncodeRejoinFile(open) == "lobby 5\nuntil 10\n");
    CHECK(ParseRejoinFile("lobby 5\r\nuntil 10\r\n", 9, back) && back.lobby == 5 && back.password.empty());
    CHECK(!ParseRejoinFile("", 0, back) && !ParseRejoinFile("lobby 5\n", 0, back) && !ParseRejoinFile("until 10\n", 0, back));
    CHECK(!ParseRejoinFile("lobby 0\nuntil 10\n", 0, back) && !ParseRejoinFile("lobby x\nuntil 10\n", 0, back) && !ParseRejoinFile("lobby 5\nuntil 1e3\n", 0, back));
    CHECK(!ParseRejoinFile("lobby 123456789012345678901\nuntil 10\n", 0, back));   // 21 digits: refused, not wrapped
}

int main() {
    TestPlanByKind();
    TestPlanHseLock();
    TestPlanBlockers();
    TestPlanPendingAndUnchecked();
    TestSummaryAndTag();
    TestPakRel();
    TestSwitchFile();
    TestAsideLines();
    TestRejoinFile();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_mod_switch: all passed\n");
    return 0;
}

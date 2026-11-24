// The lobby's MODS tab: who runs each mod ("everyone", names, "+N", unknown slots), UE4SS's stock mods left out, the
// refused rows first with their reason under each preset, the tones, the host's rows enabled and a member's not, the
// hints when the rule in force differs from the host's choice, the status lines, the truncated footer, the host's
// pending lines and the steppers.
#include "match/hsm_lobby_mods.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static ModItem Item(ModKind k, const char* name) { ModItem it; it.kind = k; it.name = name; return it; }
static ModRule Rule(ModPreset p, bool blockHse = true, ClassesMode c = ClassesMode::Off) { ModRule r; r.preset = p; r.classes = c; r.blockHse = blockHse; r.officialOnly = false; return r; }

static LobbyModsLocal Lobby(bool host, ModRule rule = Rule(ModPreset::MapsOnly)) {
    LobbyModsLocal l;
    l.host = host; l.rulesOn = true; l.hostHasRule = true; l.haveList = true; l.rule = rule;
    l.chosen = ChoiceOf(rule);
    l.names[0] = "Ann"; l.names[1] = "Bob"; l.names[2] = "Cy";
    l.present = 0b111;
    l.mods.entries = {
        {Item(ModKind::MapPak, "Arena.pak"), 0b111},
        {Item(ModKind::Ue4ssStock, "BPModLoaderMod"), 0b111},
        {Item(ModKind::Ue4ssMod, "HSEnhancer"), 0b010},
        {Item(ModKind::Ue4ssMod, "WillieFace"), 0b101},
    };
    return l;
}
static const ModRowView* Row(const LobbyModsScene& s, const char* name) {
    for (const ModRowView& r : s.items) if (r.name == name) return &r;
    return nullptr;
}

static void Holders() {
    LobbyModsLocal l = Lobby(false);
    LobbyModsScene s = BuildLobbyMods(l);
    CHECK(Row(s, "Arena.pak") && Row(s, "Arena.pak")->holders == "everyone");
    CHECK(Row(s, "WillieFace") && Row(s, "WillieFace")->holders == "Ann, Cy");
    CHECK(Row(s, "HSEnhancer") && Row(s, "HSEnhancer")->holders == "Bob");
    // Alone in the session: a name, not "everyone".
    l.present = 0b001;
    CHECK(ModHolders(l, 0b001) == "Ann");
    // Five holders: three names and "+2"; unknown slots by number, slot 0 as the host.
    l.present = 0b111111;
    CHECK(ModHolders(l, 0b011111) == "Ann, Bob, Cy +2");
    LobbyModsLocal anon; anon.present = 0b1001;
    CHECK(ModHolders(anon, 0b1000) == "player 4");
    CHECK(ModHolders(anon, 0b0001) == "host");
    CHECK(ModHolders(anon, 0b1001) == "everyone");
    CHECK(ModSlotCount(0b1011) == 3);
}

static void StockHiddenAndOrder() {
    LobbyModsScene s = BuildLobbyMods(Lobby(false));
    CHECK(Row(s, "BPModLoaderMod") == nullptr);
    CHECK(s.items.size() == 3);
    // Maps only refuses HSEnhancer (blocked) and WillieFace (not a map): first, in order; the map keeps its place after them.
    CHECK(s.items.size() == 3 && s.items[0].name == "HSEnhancer" && s.items[1].name == "WillieFace" && s.items[2].name == "Arena.pak");
    CHECK(s.items[0].refused && s.items[0].note == "refused: HS Enhancer is blocked here");
    CHECK(s.items[1].refused && s.items[1].note == "refused: not allowed (Maps only)");
    CHECK(!s.items[2].refused && s.items[2].note.empty());
    CHECK(s.empty.empty());
}

static void Tones() {
    // HS Enhancer where it is allowed: amber and named, so the lobby sees who runs it. A map: green. Any other mod: dim, "mod".
    LobbyModsScene s = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, false)));
    CHECK(Row(s, "HSEnhancer")->tone == ModTone::Warn && Row(s, "HSEnhancer")->verdict == "HS ENHANCER" && !Row(s, "HSEnhancer")->refused);
    CHECK(Row(s, "Arena.pak")->tone == ModTone::Ok && Row(s, "Arena.pak")->verdict == "map");
    CHECK(Row(s, "WillieFace")->tone == ModTone::Dim && Row(s, "WillieFace")->verdict == "mod");
    // Blocked: red.
    LobbyModsScene b = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, true)));
    CHECK(Row(b, "HSEnhancer")->tone == ModTone::Bad && Row(b, "HSEnhancer")->verdict == "HS ENHANCER" && Row(b, "HSEnhancer")->refused);
    CHECK(ModToneOf(false, ModKind::Ue4ssMod, false) == ModTone::Dim && ModToneOf(false, ModKind::ContentPak, false) == ModTone::Dim);
    CHECK(ModToneOf(false, ModKind::Proxy, false) == ModTone::Dim && ModToneOf(false, ModKind::MapPak, false) == ModTone::Ok);
    CHECK(ModToneOf(true, ModKind::Module, false) == ModTone::Warn && ModToneOf(true, ModKind::Ue4ssMod, true) == ModTone::Bad);
    CHECK(ModToneOf(false, ModKind::MapPak, true) == ModTone::Bad);
    CHECK(std::string(ModVerdictText(false, ModKind::Proxy)) == "overlay or loader");
    CHECK(std::string(ModVerdictText(false, ModKind::Ue4ssMod)) == "mod" && std::string(ModVerdictText(false, ModKind::ContentPak)) == "mod");
    CHECK(std::string(ModVerdictText(true, ModKind::Module)) == "HS ENHANCER" && std::string(ModVerdictText(false, ModKind::MapPak)) == "map");
    CHECK(std::string(ModVerdictText(false, ModKind::HseLocked)) == "HS Enhancer, locked" && ModToneOf(false, ModKind::HseLocked, false) == ModTone::Dim);
}

static void RefusedByPreset() {
    // Anything with HS Enhancer allowed: nothing refused. The Classes row judges mods no differently.
    LobbyModsScene any = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, false)));
    for (const ModRowView& r : any.items) CHECK(!r.refused);
    for (const ClassesMode m : {ClassesMode::On, ClassesMode::Unrestricted}) {
        LobbyModsScene cls = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, false, m)));
        CHECK(cls.items.size() == 3);
        for (const ModRowView& r : cls.items) CHECK(!r.refused);
    }
    // HS Enhancer blocked: only it, whatever the preset.
    LobbyModsScene anyBlock = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, true)));
    CHECK(Row(anyBlock, "HSEnhancer")->refused && !Row(anyBlock, "Arena.pak")->refused && !Row(anyBlock, "WillieFace")->refused);
    LobbyModsScene clsBlock = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, true, ClassesMode::On)));
    CHECK(Row(clsBlock, "HSEnhancer")->refused && Row(clsBlock, "HSEnhancer")->note == "refused: HS Enhancer is blocked here" && !Row(clsBlock, "WillieFace")->refused);
    // Vanilla: every mod.
    LobbyModsScene van = BuildLobbyMods(Lobby(false, Rule(ModPreset::Vanilla, false)));
    CHECK(Row(van, "Arena.pak")->refused && Row(van, "Arena.pak")->note == "refused: Vanilla takes no mods");
    CHECK(Row(van, "WillieFace")->refused && Row(van, "HSEnhancer")->refused && Row(van, "HSEnhancer")->note == "refused: Vanilla takes no mods");
    // ...and Vanilla stays Vanilla with Classes on: the row lets no mod in (it works with any Mods allowed value).
    for (const ClassesMode m : {ClassesMode::On, ClassesMode::Unrestricted}) {
        LobbyModsScene vc = BuildLobbyMods(Lobby(false, Rule(ModPreset::Vanilla, false, m)));
        CHECK(Row(vc, "Arena.pak")->refused && Row(vc, "WillieFace")->refused && Row(vc, "Arena.pak")->note == "refused: Vanilla takes no mods");
    }
    // Maps only, HS Enhancer allowed: every UE4SS mod is not allowed (HSE included: not a map), the map is.
    LobbyModsLocal l = Lobby(false, Rule(ModPreset::MapsOnly, false));
    l.mods.entries.push_back({Item(ModKind::Ue4ssMod, "SomethingNew"), 0b100});
    l.mods.entries.push_back({Item(ModKind::Proxy, "dxgi.dll"), 0b100});
    LobbyModsScene maps = BuildLobbyMods(l);
    CHECK(Row(maps, "SomethingNew")->refused && Row(maps, "SomethingNew")->note == "refused: not allowed (Maps only)");
    CHECK(Row(maps, "HSEnhancer")->refused && Row(maps, "HSEnhancer")->note == "refused: not allowed (Maps only)");
    CHECK(Row(maps, "WillieFace")->refused);
    CHECK(!Row(maps, "Arena.pak")->refused && !Row(maps, "dxgi.dll")->refused);
    // Custom: only the list; a required mod someone lacks says who.
    ModRule custom = Rule(ModPreset::Custom, false);
    ModAllow a; a.name = "Arena.pak"; a.required = true; custom.list.push_back(a);
    ModAllow b; b.name = "HSEnhancer"; b.required = true; custom.list.push_back(b);
    LobbyModsScene cs = BuildLobbyMods(Lobby(false, custom));
    CHECK(Row(cs, "Arena.pak")->note.empty());
    CHECK(Row(cs, "HSEnhancer")->note == "required: missing for Ann, Cy" && Row(cs, "HSEnhancer")->tone == ModTone::Warn && !Row(cs, "HSEnhancer")->refused);
    CHECK(Row(cs, "WillieFace")->refused && Row(cs, "WillieFace")->note == "refused: not allowed (Custom)");   // off the list
    // A required mod nobody runs gets a row of its own.
    ModRule need = Rule(ModPreset::Custom, false);
    need.list = custom.list;
    ModAllow c; c.name = "Pack.pak"; c.required = true; need.list.push_back(c);
    LobbyModsScene ns0 = BuildLobbyMods(Lobby(false, need));
    CHECK(Row(ns0, "Pack.pak") && Row(ns0, "Pack.pak")->note == "required: missing for everyone" && Row(ns0, "Pack.pak")->verdict == "missing");
    CHECK(!ns0.items.empty() && ns0.items.back().name == "Pack.pak");
    size_t arenas = 0;
    for (const ModRowView& r : ns0.items) arenas += r.name == "Arena.pak";
    CHECK(arenas == 1);   // a required mod someone runs: its own row only
    ModAllow opt; opt.name = "Optional.pak"; need.list.push_back(opt);
    CHECK(Row(BuildLobbyMods(Lobby(false, need)), "Optional.pak") == nullptr);   // not required, nobody runs it: no row
    // A hash-only list the browser cannot read (decoded with no names): nothing marked refused.
    custom.list.clear(); custom.listHashOnly = true;
    LobbyModsScene hidden = BuildLobbyMods(Lobby(false, custom));
    for (const ModRowView& r : hidden.items) CHECK(!r.refused);
    // No rule in force: no notes at all.
    LobbyModsLocal none = Lobby(false, Rule(ModPreset::Vanilla, true));
    none.hostHasRule = false;
    LobbyModsScene ns = BuildLobbyMods(none);
    for (const ModRowView& r : ns.items) CHECK(!r.refused && r.note.empty());
    CHECK(ns.status == "The host does not check mods.");
}

static void RuleRows() {
    LobbyModsScene host = BuildLobbyMods(Lobby(true));
    for (const ModRuleView& r : host.rules) CHECK(r.enabled);
    CHECK(kModRuleRows == 4);
    CHECK(host.rules[0].id == ModRuleRow::Preset && host.rules[0].value == "Maps only");
    CHECK(host.rules[1].id == ModRuleRow::Classes && host.rules[1].label == "Classes" && host.rules[1].value == "Off");   // its own row, under Mods allowed
    CHECK(host.rules[2].id == ModRuleRow::Hse && host.rules[2].label == "Block HS Enhancer" && host.rules[2].value == "On");
    CHECK(host.rules[1].hint.empty() && host.rules[2].hint.empty() && host.rules[3].hint.empty());   // the label and the value say it
    CHECK(host.rules[3].id == ModRuleRow::Official && host.rules[3].label == "Official builds only" && host.rules[3].value == "Off");
    CHECK(host.rules[0].hint.empty());                                                  // Maps only: the value says it
    CHECK(host.status.empty());
    LobbyModsScene member = BuildLobbyMods(Lobby(false));
    for (const ModRuleView& r : member.rules) CHECK(!r.enabled);
    CHECK(member.rules[0].value == "Maps only" && member.rules[1].value == "Off");
    CHECK(std::string(ModPresetHint(ModPreset::Custom, false)) == "The host's mods, or the list it set.");
    CHECK(std::string(ModPresetHint(ModPreset::Custom, true)) == "The mods you run, or the list you set.");
    CHECK(std::string(ModPresetHint(ModPreset::Bundle, true)) == "Everyone needs exactly your mods.");
    CHECK(std::string(ModPresetHint(ModPreset::Bundle, false)) == "You need exactly the host's mods.");
    for (bool h : {true, false})                                                         // no hint where the value says it
        CHECK(*ModPresetHint(ModPreset::MapsOnly, h) == 0 && *ModPresetHint(ModPreset::Vanilla, h) == 0 && *ModPresetHint(ModPreset::Anything, h) == 0);
    // The Classes row: each value shows for the host's choice and for a member's reading of the rule; only Unrestricted has a hint.
    for (const ModPreset p : {ModPreset::Vanilla, ModPreset::MapsOnly, ModPreset::Bundle, ModPreset::Custom, ModPreset::Anything})
        for (const ClassesMode m : {ClassesMode::Off, ClassesMode::On, ClassesMode::Unrestricted}) {
            const LobbyModsScene h = BuildLobbyMods(Lobby(true, Rule(p, true, m)));
            const LobbyModsScene g = BuildLobbyMods(Lobby(false, Rule(p, true, m)));
            const std::string want = m == ClassesMode::Off ? "Off" : m == ClassesMode::On ? "On" : "Unrestricted";
            CHECK(h.rules[1].value == want && g.rules[1].value == want);
            CHECK(h.rules[0].value == ModPresetName(p) && g.rules[0].value == ModPresetName(p));   // the preset row is not touched by the Classes row
            CHECK(h.rules[1].enabled && !g.rules[1].enabled);
            CHECK(h.rules[1].hint == (m == ClassesMode::Unrestricted ? "No class balance. Expect glitches, and possibly crashes, for everyone." : ""));
        }
    {   // The host sees the setting it just made (chosen); a member sees the rule in force, whatever the member's own choice says.
        LobbyModsLocal d = Lobby(true, Rule(ModPreset::Vanilla, true, ClassesMode::Off));
        d.chosen.classes = ClassesMode::On;
        CHECK(BuildLobbyMods(d).rules[1].value == "On" && BuildLobbyMods(d).rules[1].choice == size_t(ClassesMode::On));
        LobbyModsLocal g = Lobby(false, Rule(ModPreset::Vanilla, true, ClassesMode::Unrestricted));
        g.chosen.classes = ClassesMode::Off;
        CHECK(BuildLobbyMods(g).rules[1].value == "Unrestricted" && BuildLobbyMods(g).rules[1].choice == size_t(ClassesMode::Unrestricted));
    }
    CHECK(*ClassesModeHint(ClassesMode::Off) == 0 && *ClassesModeHint(ClassesMode::On) == 0);
    CHECK(std::string(ClassesModeHint(ClassesMode::Unrestricted)) == "No class balance. Expect glitches, and possibly crashes, for everyone.");
    // Rules off: rows disabled, the status says so, nothing refused.
    LobbyModsLocal off = Lobby(true);
    off.rulesOn = false;
    LobbyModsScene so = BuildLobbyMods(off);
    for (const ModRuleView& r : so.rules) CHECK(!r.enabled);
    CHECK(so.status == "Mod rules are off: anyone may join with any mods.");              // no INI key in player text
    for (const ModRowView& r : so.items) CHECK(!r.refused);
    // #3396: a host with a class picked hears on the Classes row that classes are off, whatever Mods allowed says.
    LobbyModsLocal picked = Lobby(true, Rule(ModPreset::Vanilla));
    picked.classPicked = true;
    const std::string kOff = "Classes are off: your class does not apply. Turn them on to use it.";
    CHECK(BuildLobbyMods(picked).rules[1].hint == kOff && BuildLobbyMods(picked).rules[0].hint.empty());
    picked.chosen.classes = ClassesMode::On;
    CHECK(BuildLobbyMods(picked).rules[1].hint.empty());                                // on: nothing to say
    picked.chosen.classes = ClassesMode::Unrestricted;
    CHECK(BuildLobbyMods(picked).rules[1].hint == "No class balance. Expect glitches, and possibly crashes, for everyone.");
    picked.chosen.classes = ClassesMode::Off;
    for (const ModPreset p : {ModPreset::MapsOnly, ModPreset::Anything}) { picked.chosen.preset = p; CHECK(BuildLobbyMods(picked).rules[1].hint == kOff); }
    picked.chosen.preset = ModPreset::Bundle;                                           // Mods allowed keeps its own hint, unmixed
    CHECK(BuildLobbyMods(picked).rules[0].hint == "Everyone needs exactly your mods." && BuildLobbyMods(picked).rules[1].hint == kOff);
    picked.classPicked = false;
    CHECK(BuildLobbyMods(picked).rules[1].hint.empty());
    LobbyModsLocal member2 = Lobby(false, Rule(ModPreset::Vanilla));
    member2.classPicked = true;   // never set for a member; the hint stays the host's alone anyway
    CHECK(BuildLobbyMods(member2).rules[1].hint.empty());
}

static void DiffersHints() {
    // The host chose Bundle with no pack announced: its own mods are the bundle.
    LobbyModsLocal l = Lobby(true);
    l.chosen = ModChoice{ModPreset::Bundle, ClassesMode::Off, true, true};
    l.rule = Rule(ModPreset::Bundle, false);   // it runs HS Enhancer itself; not an official build
    LobbyModsScene s = BuildLobbyMods(l);
    CHECK(s.rules[0].value == "Bundle" && s.rules[0].hint == "Everyone needs exactly your mods.");
    CHECK(s.rules[2].value == "On" && s.rules[2].hint == "You run HS Enhancer yourself, so this lobby allows it.");
    CHECK(s.rules[3].value == "On" && s.rules[3].hint.find("not an official release") != std::string::npos);
    l.rule = Rule(ModPreset::MapsOnly, false);
    s = BuildLobbyMods(l);
    CHECK(s.rules[0].value == "Bundle" && s.rules[0].hint == "Everyone needs exactly your mods.");   // the host row shows its choice
    // A member sees the rule in force, no host hints.
    LobbyModsLocal m = l; m.host = false;
    LobbyModsScene ms = BuildLobbyMods(m);
    CHECK(ms.rules[0].value == "Maps only" && ms.rules[1].value == "Off" && ms.rules[2].value == "Off" && ms.rules[3].value == "Off");
    CHECK(ms.rules[2].hint.find("yourself") == std::string::npos);
}

static void StatusAndFooters() {
    LobbyModsLocal l = Lobby(false);
    l.haveList = false;
    LobbyModsScene s = BuildLobbyMods(l);
    CHECK(s.items.empty() && s.empty.empty() && s.status.find("Waiting") == 0);
    l.haveList = true; l.mods.entries = {{Item(ModKind::Ue4ssStock, "Keybinds"), 0b111}};
    LobbyModsScene e = BuildLobbyMods(l);
    CHECK(e.items.empty() && e.empty == "Nobody here runs mods." && e.status.empty());
    l.mods.truncated = true;
    CHECK(BuildLobbyMods(l).truncated == "The list stops at 96 mods.");
    CHECK(!BuildLobbyMods(l).privacy.empty());
}

static void Pending() {
    ModPending wait; wait.name = "Bob"; wait.held = true;
    CHECK(ModPendingLine(wait) == "Bob: waiting for their mod list");
    wait.haveList = true; wait.result.fail = RuleFail::Pending;
    CHECK(ModPendingLine(wait) == "Bob: still scanning their mods");
    wait.result.fail = RuleFail::None;
    CHECK(ModPendingLine(wait) == "Bob: checking mods");
    ModPending leave; leave.name = "Ann"; leave.held = false; leave.graceSeconds = 42;
    leave.result.fail = RuleFail::Hse; leave.result.mod = "HSEnhancer";
    CHECK(ModPendingLine(leave) == "Ann: runs HSEnhancer, leaving in 42 s unless fixed");
    leave.result.fail = RuleFail::Missing; leave.result.mod = "Arena.pak";
    CHECK(ModPendingLine(leave) == "Ann: lacks Arena.pak, leaving in 42 s unless fixed");
    leave.result.fail = RuleFail::NoMods; leave.result.mod = "ConsoleEnablerMod";
    CHECK(ModPendingLine(leave) == "Ann: runs ConsoleEnablerMod, leaving in 42 s unless fixed");
    leave.graceSeconds = 0;
    CHECK(ModPendingLine(leave).empty());
    // Only the host gets the lines; empty ones are dropped.
    LobbyModsLocal h = Lobby(true);
    h.pending = {wait, leave};
    CHECK(BuildLobbyMods(h).pending.size() == 1 && BuildLobbyMods(h).pending[0] == "Bob: checking mods");
    h.host = false;
    CHECK(BuildLobbyMods(h).pending.empty());
}

static void Steppers() {
    ModChoice c{ModPreset::Vanilla, ClassesMode::Off, true, false};
    CHECK(StepModRule(c, ModRuleRow::Preset, -1).preset == ModPreset::Anything);          // five stops, and Classes is not one of them
    CHECK(StepModRule(c, ModRuleRow::Preset, 1).preset == ModPreset::MapsOnly);
    c.preset = ModPreset::Anything;
    CHECK(StepModRule(c, ModRuleRow::Preset, 1).preset == ModPreset::Vanilla);
    CHECK(StepModRule(c, ModRuleRow::Preset, -1).preset == ModPreset::Custom);
    ModChoice t = StepModRule(c, ModRuleRow::Hse, 1);
    CHECK(!t.blockHse && t.preset == c.preset && !t.officialOnly && t.classes == c.classes);
    t = StepModRule(c, ModRuleRow::Official, -1);
    CHECK(t.officialOnly && t.blockHse);
    CHECK(StepModRule(StepModRule(c, ModRuleRow::Hse, 1), ModRuleRow::Hse, -1) == c);
    // Every Mods allowed value comes round after five steps either way, and the Classes row is left as it was.
    for (const ClassesMode m : {ClassesMode::Off, ClassesMode::On, ClassesMode::Unrestricted}) {
        ModChoice w = c; w.classes = m; w.preset = ModPreset::Vanilla;
        for (int i = 1; i <= 5; ++i) { w = StepModRule(w, ModRuleRow::Preset, 1); CHECK(w.classes == m && int(w.preset) == i % 5); }
        for (int i = 1; i <= 5; ++i) { w = StepModRule(w, ModRuleRow::Preset, -1); CHECK(w.classes == m && int(w.preset) == (5 - i) % 5); }
    }
    // The Classes row: Off, On, Unrestricted, round; with every Mods allowed value (the preset is left as it was).
    for (uint8_t p = 0; p <= kMaxModPreset; ++p) {
        ModChoice k = c; k.preset = ModPreset(p); k.classes = ClassesMode::Off;
        k = StepModRule(k, ModRuleRow::Classes, 1);  CHECK(k.classes == ClassesMode::On && k.preset == ModPreset(p));
        k = StepModRule(k, ModRuleRow::Classes, 1);  CHECK(k.classes == ClassesMode::Unrestricted && k.preset == ModPreset(p));
        k = StepModRule(k, ModRuleRow::Classes, 1);  CHECK(k.classes == ClassesMode::Off && k.preset == ModPreset(p));
        k = StepModRule(k, ModRuleRow::Classes, -1); CHECK(k.classes == ClassesMode::Unrestricted);
        k = StepModRule(k, ModRuleRow::Classes, -1); CHECK(k.classes == ClassesMode::On);
        k = StepModRule(k, ModRuleRow::Classes, -1); CHECK(k.classes == ClassesMode::Off);
        CHECK(k.blockHse == c.blockHse && k.officialOnly == c.officialOnly);
    }
}

int main() {
    Holders();
    StockHiddenAndOrder();
    Tones();
    RefusedByPreset();
    RuleRows();
    DiffersHints();
    StatusAndFooters();
    Pending();
    Steppers();
    if (g_fail) { std::printf("test_hsm_lobby_mods: %d FAILED\n", g_fail); return 1; }
    std::printf("test_hsm_lobby_mods: PASS\n");
    return 0;
}

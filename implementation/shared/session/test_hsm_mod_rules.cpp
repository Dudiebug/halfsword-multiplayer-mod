// Unit tests for hsm_mod_rules.h. Run via implementation/shared/run_tests.sh test_hsm_mod_rules.
#include "session/hsm_mod_rules.h"
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static Sha256Digest Sha(uint8_t seed) { Sha256Digest d{}; for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(seed + i * 7); return d; }
static ModItem Item(ModKind k, const char* name, uint8_t seed = 0) {
    ModItem it; it.kind = k; it.name = name;
    if (seed) { it.hashed = true; it.sha = Sha(seed); }
    return it;
}
static ModReport Report(std::vector<ModItem> items, bool ue4ss = false) {
    ModReport r; r.items = std::move(items); r.ue4ss = ue4ss; r.complete = true; NormalizeReport(r); return r;
}
static ModRule Rule(ModPreset p, bool blockHse = true) { ModRule r; r.preset = p; r.blockHse = blockHse; return r; }
static std::vector<ModItem> Ue4ssStock() {
    std::vector<ModItem> v;
    for (const char* n : {"BPModLoaderMod", "BPML_GenericFunctions", "Keybinds", "CheatManagerEnablerMod", "ConsoleEnablerMod", "ConsoleCommandsMod"})
        v.push_back(Item(ModKind::Ue4ssStock, n));
    return v;
}

static void TestNames() {
    CHECK(ModItemNameOk("jmblocks.pak") && ModItemNameOk("chrome334's pistol mod.pak") && ModItemNameOk("Screams+_P.pak"));
    CHECK(!ModItemNameOk("") && !ModItemNameOk("a|b") && !ModItemNameOk("a*b") && !ModItemNameOk("+Req") && !ModItemNameOk("#abc"));
    CHECK(!ModItemNameOk(std::string(kMaxModItemName + 1, 'a')) && ModItemNameOk(std::string(kMaxModItemName, 'a')));
    CHECK(!ModItemNameOk("bad\x01") && !ModItemNameOk("caf\xc3\xa9"));
    CHECK(SanitizeModItemName("+a|b*c\x02") == "_a_b_c_" && SanitizeModItemName("#x") == "_x" && SanitizeModItemName("").empty());
    CHECK(SanitizeModItemName(std::string(200, 'z')).size() == kMaxModItemName);
}

static void TestNormalize() {
    ModReport r;
    r.items = {Item(ModKind::Ue4ssMod, "zeta"), Item(ModKind::MapPak, "b.pak"), Item(ModKind::Ue4ssMod, "Alpha"), Item(ModKind::MapPak, "A.pak"), Item(ModKind::Proxy, "bad|name")};
    NormalizeReport(r);
    CHECK(r.items.size() == 4 && r.items[0].name == "A.pak" && r.items[1].name == "b.pak" && r.items[2].name == "Alpha" && r.items[3].name == "zeta");
    CHECK(!r.truncated);
    ModReport big;
    for (size_t i = 0; i < kMaxModItems + 3; ++i) big.items.push_back(Item(ModKind::ContentPak, ("p" + std::to_string(1000 + i) + ".pak").c_str()));
    NormalizeReport(big);
    CHECK(big.items.size() == kMaxModItems && big.truncated);
}

static void TestHse() {
    // The one named mod: its UE4SS folder or the loaded-DLL Module item, any case. A pak or proxy DLL of that name is not it.
    CHECK(IsHseItem(Item(ModKind::Ue4ssMod, "HSEnhancer")) && IsHseItem(Item(ModKind::Ue4ssMod, "hsenhancer")));
    CHECK(IsHseItem(Item(ModKind::Module, "HSEnhancer")));
    CHECK(!IsHseItem(Item(ModKind::ContentPak, "HSEnhancer")) && !IsHseItem(Item(ModKind::Proxy, "HSEnhancer")) && !IsHseItem(Item(ModKind::Ue4ssStock, "HSEnhancer")));
    CHECK(!IsHseItem(Item(ModKind::Ue4ssMod, "HSEnhancerX")) && !IsHseItem(Item(ModKind::Ue4ssMod, "HSHelper")) && !IsHseItem(Item(ModKind::Ue4ssMod, "BiteMod")));
    CHECK(HasHse(Report({Item(ModKind::MapPak, "A.pak"), Item(ModKind::Module, "HSEnhancer")})) && !HasHse(Report({Item(ModKind::Ue4ssMod, "HSHelper")})) && !HasHse(Report({})));
    // Locked for this game: no longer an HSE item, its own kind
    const ModItem locked = Item(ModKind::HseLocked, "HSEnhancer");
    CHECK(!IsHseItem(locked) && std::string(ModKindName(ModKind::HseLocked)) == "HS Enhancer, locked");
    CHECK(HasLockedHse(Report({locked})) && !HasHse(Report({locked})) && !HasLockedHse(Report({Item(ModKind::Module, "HSEnhancer")})) && !HasLockedHse(Report({})));
    // LockReportHse: each HSE item, and only those, becomes HseLocked, hash kept, sorted again by kind
    ModReport lr = Report({Item(ModKind::Module, "HSEnhancer"), Item(ModKind::Ue4ssMod, "HSEnhancer", 2), Item(ModKind::Ue4ssMod, "Other", 3), Item(ModKind::ContentPak, "HSEnhancer", 4)});
    LockReportHse(lr);
    CHECK(lr.items.size() == 4 && lr.items[0].kind == ModKind::ContentPak && lr.items[1].kind == ModKind::Ue4ssMod && lr.items[1].name == "Other");
    CHECK(lr.items[2].kind == ModKind::HseLocked && lr.items[2].hashed && lr.items[3].kind == ModKind::HseLocked && !lr.items[3].hashed);
    CHECK(!HasHse(lr) && HasLockedHse(lr));
    ModReport none = Report({Item(ModKind::Ue4ssMod, "Other", 3)});
    LockReportHse(none);
    CHECK(none.items.size() == 1 && none.items[0].kind == ModKind::Ue4ssMod && !HasLockedHse(none));
    // UE4SS's stock mods, by name: the six it ships enabled, and nothing else.
    for (const ModItem& it : Ue4ssStock()) CHECK(ue4ss::IsStockMod(it.name));
    CHECK(ue4ss::IsStockMod("keybinds") && !ue4ss::IsStockMod("HSEnhancer") && !ue4ss::IsStockMod("LineTraceMod") && !ue4ss::IsStockMod(""));
    for (const char* c : ue4ss::kConsoleMods) CHECK(ue4ss::IsStockMod(c));
    // RuleRefusesHse: a loaded HS Enhancer refused by the switch or by the preset (its guests hold HSE's keys)
    const auto rule = [](ModPreset p, bool block) { ModRule r; r.preset = p; r.blockHse = block; return r; };
    CHECK(RuleRefusesHse(rule(ModPreset::Anything, true)) && !RuleRefusesHse(rule(ModPreset::Anything, false)));
    CHECK(!RuleRefusesHse(rule(ModPreset::Classes, false)) && !RuleRefusesHse(rule(ModPreset::Unrestricted, false)));
    CHECK(RuleRefusesHse(rule(ModPreset::Vanilla, false)) && RuleRefusesHse(rule(ModPreset::MapsOnly, false)));
    ModRule custom = rule(ModPreset::Custom, false);
    CHECK(RuleRefusesHse(custom));                       // Same as host, and the host has no HS Enhancer
    custom.list.push_back(ModAllow{"hsenhancer", "", false});
    CHECK(!RuleRefusesHse(custom));                      // on the list, any case
    custom.listHashOnly = true;
    CHECK(RuleRefusesHse(custom));                       // only the host can read the list: held
    custom.listHashOnly = false;
    custom.blockHse = true;
    CHECK(RuleRefusesHse(custom));                       // the switch wins over the list
}

static void TestClassify() {
    // Real map paks: the map and its assets under the mod's own folder.
    CHECK(ClassifyPak({"/Game/Tavern/Maps/Tavern"}, {"/Game/Tavern/Maps", "/Game/Tavern/Maps/Textures"}, false) == ModKind::MapPak);
    CHECK(ClassifyPak({"/Game/jmblocks/Blocks", "/Game/jmblocks/Maps/Arena"}, {"/Game/jmblocks/Meshes", "/Game/jmblocks/Maps/Arena"}, false) == ModKind::MapPak);
    CHECK(ClassifyPak({"/Game/Maps/MyArena/Arena"}, {"/Game/Maps/MyArena/Props"}, false) == ModKind::MapPak);   // own subfolder of a stock folder
    CHECK(ClassifyPak({"/HalfCraft/Maps/Craft"}, {"/HalfCraft/Maps/Blocks"}, false) == ModKind::MapPak);         // a plugin root
    CHECK(ClassifyPak({"/Game/Maps/Tower"}, {}, false) == ModKind::MapPak);
    // 2026-11-07, a log with 17 mod paks: not one counted as a map, as real maps keep props, sounds and starter content all over.
    CHECK(ClassifyPak({"/Game/CustomMaps/AeroFlat", "/Game/CustomMaps/MMA"}, {"/Game/CustomMapContent", "/Game/CustomSound", "/Game/Stadium"}, false) == ModKind::MapPak);
    CHECK(ClassifyPak({"/Game/gm_flatgrass"}, {"/Game/Materials", "/Game/Meshes", "/Game/Textures"}, false) == ModKind::MapPak);
    CHECK(ClassifyPak({"/Game/CustomMaps/BackRooms/level"}, {"/Game"}, false) == ModKind::MapPak);
    CHECK(ClassifyPak({"/Game/CustomMaps/TOMB"}, {"/Game/Fab/Props_Pack_-_Cobwebs", "/Game"}, false) == ModKind::MapPak);
    // A new file in a stock folder only adds. What it replaces is told by name, not by folder.
    CHECK(ClassifyPak({"/Game/Maps/Tower"}, {"/Game/Maps"}, false) == ModKind::MapPak);
    PakClass c = ClassifyPakWhy({"/Game/Hole"}, {"/Game/Character/Blueprints"}, false, "", 0, {"/Game/Character/Blueprints/AI_BP"});
    CHECK(c.kind == ModKind::ContentPak && c.why == "replaces stock files: /Game/Character/Blueprints/AI_BP");
    CHECK(ClassifyPakWhy({"/Game/Hole"}, {}, false, "", 0, {"/Game/A", "/Game/B"}).why == "replaces stock files: /Game/A /Game/B");
    // BPModLoaderMod runs what sits in /Game/Mods, and every lobby allows it. Weapon paks live there, test map or not.
    CHECK(ClassifyPak({"/Game/Mods/Cheat/T"}, {"/Game/Mods/Cheat"}, false) == ModKind::ContentPak);
    CHECK(ClassifyPak({"/Game/Mods/Smitor/Lightsaber/Lightsabertest"}, {"/Game/Mods/Smitor/Lightsaber", "/Game"}, false) == ModKind::ContentPak);
    CHECK(ClassifyPak({"/Game/Mine/Arena"}, {"/Game/Mine", "/Game/mods/x"}, false) == ModKind::ContentPak);   // a stub map doesn't vouch for it
    CHECK(ClassifyPak({"/Engine/Maps/X/Y"}, {"/Engine/Maps/X"}, false) == ModKind::ContentPak);
    CHECK(MapPakKeepsOut("/Game/Mods") && MapPakKeepsOut("/Game/mods/x") && MapPakKeepsOut("/Engine/X") && MapPakKeepsOut("/engine"));
    CHECK(!MapPakKeepsOut("/Game/ModsOfMine") && !MapPakKeepsOut("/Game") && !MapPakKeepsOut("/Enginex") && !MapPakKeepsOut("/Mods/X"));
    // A pak outside those folders that adds a weapon and a test map (GunsEtc) passes: the weapon only exists on its own map.
    CHECK(ClassifyPak({"/Game/GunsEtc/Maps/TestMap"}, {"/Game/GunsEtc/Maps", "/Game/Weapons/Guns"}, false) == ModKind::MapPak);
    // A map at a stock package's path replaces it, with no asset to show for it.
    CHECK(ClassifyPak({"/Game/Maps/Arenas/Map_Arena_EastTower"}, {}, false) == ModKind::ContentPak);
    CHECK(ClassifyPak({"/Game/maps/arenas/map_arena_easttower"}, {}, false) == ModKind::ContentPak);
    CHECK(ClassifyPak({"/Game/RT_Blood"}, {}, false) == ModKind::ContentPak);
    CHECK(ClassifyPak({"/Game/RT_Mine"}, {}, false) == ModKind::MapPak);                          // NEGATIVE CONTROL
    // The stock list holds every stock package, in any case, and nothing else.
    CHECK(std::size(kStockPackageHashes) > 10000 && std::is_sorted(std::begin(kStockPackageHashes), std::end(kStockPackageHashes)));
    CHECK(IsStockPackage("/Game/Character/Blueprints/AI_BP") && IsStockPackage("/game/CHARACTER/blueprints/ai_bp"));
    CHECK(!IsStockPackage("/Game/Character/Blueprints/AI_BP2") && !IsStockPackage("/Game/Character/Blueprints") && !IsStockPackage(""));
    for (size_t i = 0; i < kStockMapPathCount; ++i) CHECK(IsStockPackage(kStockMapPaths[i]));
    // What the log says. The class comes from the same call.
    CHECK(ClassifyPakWhy({"/Game/Hole"}, {"/Game/Hole/T", "/Game/UI"}, false).why.empty());
    CHECK(ClassifyPakWhy({"/Game/Hole"}, {"/Game/Hole/T", "/Game/Mods/A", "/Engine/B"}, false).why ==
          "2 asset folder(s) in /Game/Mods or /Engine: /Game/Mods/A, /Engine/B");
    CHECK(ClassifyPakWhy({"/Game/Hole"}, {"/Game/Mods/A", "/Game/Mods/B", "/Game/Mods/C", "/Game/Mods/D"}, false).why ==
          "4 asset folder(s) in /Game/Mods or /Engine: /Game/Mods/A, /Game/Mods/B, /Game/Mods/C, ...");
    CHECK(ClassifyPakWhy({"/Game/Hole"}, {}, true, "a config file").why == "a config file");
    CHECK(ClassifyPakWhy({"/Game/Hole"}, {}, true).kind == ModKind::ContentPak);
    CHECK(ClassifyPakWhy({"/Game/Maps/Arenas/Map_Arena_EastTower"}, {}, false).why == "replaces stock /Game/Maps/Arenas/Map_Arena_EastTower");
    // A map that can't be a package path is still a map; the rest of the pak is judged the same.
    c = ClassifyPakWhy({}, {"/Game"}, false, "", 1);
    CHECK(c.kind == ModKind::MapPak && c.why == "a map's file name cannot be a package path");
    CHECK(ClassifyPakWhy({}, {"/Game/Mods/Gun"}, false, "", 1).kind == ModKind::ContentPak);
    CHECK(ClassifyPakWhy({}, {}, false, "", 1, {"/Game/RT_Blood"}).kind == ModKind::ContentPak);
    CHECK(ClassifyPakWhy({}, {}, true, "a config file", 1).kind == ModKind::ContentPak);
    CHECK(ClassifyPakWhy({}, {"/Game/Weapons"}, false).why.empty());    // a pak with no maps needs no reason
    CHECK(ClassifyPak({}, {"/Game/Weapons"}, false) == ModKind::ContentPak);
    CHECK(ClassifyPak({"/Game/A/B/C"}, {"/Game/A/B"}, true) == ModKind::ContentPak);                // overflow
}

static void TestPresets() {
    const ModItem map = Item(ModKind::MapPak, "Tavern.pak", 1);
    const ModItem weapons = Item(ModKind::ContentPak, "ModularWeapons.pak", 2);
    const ModItem face = Item(ModKind::ContentPak, "zWillieFace_P.pak", 3);
    const ModItem enhancer = Item(ModKind::Ue4ssMod, "HSEnhancer", 4);
    const ModItem module = Item(ModKind::Module, "HSEnhancer");
    const ModItem helper = Item(ModKind::Ue4ssMod, "HSHelper", 5);
    const ModItem killcam = Item(ModKind::Ue4ssMod, "MeleeKillCam", 6);
    const ModItem winmm = Item(ModKind::Proxy, "winmm.dll", 7);

    // Nothing at all fits everything.
    for (uint8_t p = 0; p <= kMaxModPreset; ++p) CHECK(CheckMods(Rule(ModPreset(p)), Report({})).ok());
    // Unrestricted Classes judges mods exactly as Classes does: it lifts the class budget, and nothing else.
    CHECK(IsClassesPreset(ModPreset::Unrestricted) && IsClassesPreset(ModPreset::Classes) && !IsClassesPreset(ModPreset::Anything));
    { ModPreset parsed = ModPreset::Vanilla; CHECK(ParseModPreset("unrestricted", parsed) && parsed == ModPreset::Unrestricted); CHECK(DecodeLobbyRule(EncodeLobbyRule(Rule(ModPreset::Unrestricted))).preset == ModPreset::Unrestricted); }
    CHECK(std::string(ModPresetName(ModPreset::Unrestricted)) == "Unrestricted Classes");
    // A scan still running: ask again.
    ModReport running; CHECK(CheckMods(Rule(ModPreset::Anything), running).fail == RuleFail::Pending);

    // Vanilla: no paks and no UE4SS mods. UE4SS alone is no mod, but its console's `set` is a cheat tool.
    CHECK(CheckMods(Rule(ModPreset::Vanilla), Report({map})).fail == RuleFail::NoMods);
    CHECK(CheckMods(Rule(ModPreset::Vanilla), Report({face})).fail == RuleFail::NoMods);
    CHECK(CheckMods(Rule(ModPreset::Vanilla), Report({}, true)).ok());
    CHECK(CheckMods(Rule(ModPreset::Vanilla), Report({Item(ModKind::Ue4ssStock, "BPModLoaderMod"), Item(ModKind::Ue4ssStock, "BPML_GenericFunctions"), Item(ModKind::Ue4ssStock, "Keybinds")}, true)).ok());
    for (const char* console : ue4ss::kConsoleMods) {
        const RuleResult refused = CheckMods(Rule(ModPreset::Vanilla), Report({Item(ModKind::Ue4ssStock, "Keybinds"), Item(ModKind::Ue4ssStock, console)}, true));
        CHECK(refused.fail == RuleFail::NoMods && refused.mod == console);
    }
    { const RuleResult all = CheckMods(Rule(ModPreset::Vanilla), Report(Ue4ssStock(), true)); CHECK(all.fail == RuleFail::NoMods && all.more == 2); }
    CHECK(CheckMods(Rule(ModPreset::Vanilla), Report({Item(ModKind::Ue4ssMod, "HulkMod")}, true)).fail == RuleFail::NoMods);
    // A proxy DLL beside the exe is a mod to Vanilla: its name cannot tell an overlay from a mod loader.
    CHECK(CheckMods(Rule(ModPreset::Vanilla), Report({winmm})).fail == RuleFail::NoMods);

    // Maps only: map paks and stock UE4SS. A content pak, a UE4SS mod of any kind: not a map.
    std::vector<ModItem> ok = Ue4ssStock(); ok.push_back(map);
    CHECK(CheckMods(Rule(ModPreset::MapsOnly), Report(ok, true)).ok());
    RuleResult r = CheckMods(Rule(ModPreset::MapsOnly), Report({map, weapons}));
    CHECK(r.fail == RuleFail::NotAllowed && r.mod == "ModularWeapons.pak");
    CHECK(CheckMods(Rule(ModPreset::MapsOnly), Report({map, face})).fail == RuleFail::NotAllowed);     // a cosmetic pak is a content pak
    CHECK(CheckMods(Rule(ModPreset::MapsOnly), Report({killcam})).fail == RuleFail::NotAllowed);       // no good/bad table: a UE4SS mod is a mod
    CHECK(CheckMods(Rule(ModPreset::MapsOnly), Report({helper})).fail == RuleFail::NotAllowed);
    // Proxy DLLs (ReShade, ASI loaders) are reported and pass every preset but Vanilla.
    const ModItem dxgi = Item(ModKind::Proxy, "dxgi.dll", 8);
    for (const ModPreset p : {ModPreset::MapsOnly, ModPreset::Bundle, ModPreset::Custom, ModPreset::Anything, ModPreset::Classes})
        CHECK(CheckMods(Rule(p), Report({winmm, dxgi})).ok());
    r = CheckMods(Rule(ModPreset::Vanilla), Report({dxgi, map}));
    CHECK(r.fail == RuleFail::NoMods && r.mod == "Tavern.pak" && r.more == 1);                     // map paks sort first
    CHECK(CheckMods(Rule(ModPreset::MapsOnly), Report({dxgi, module})).fail == RuleFail::Hse);

    // HS Enhancer comes first, whatever the preset, Classes included.
    r = CheckMods(Rule(ModPreset::Anything), Report({weapons, enhancer}));
    CHECK(r.fail == RuleFail::Hse && r.mod == "HSEnhancer");
    CHECK(CheckMods(Rule(ModPreset::Anything), Report({module})).fail == RuleFail::Hse);
    CHECK(CheckMods(Rule(ModPreset::Classes), Report({module})).fail == RuleFail::Hse);
    CHECK(CheckMods(Rule(ModPreset::Anything, false), Report({weapons, enhancer, module, winmm})).ok());
    CHECK(CheckMods(Rule(ModPreset::Classes, false), Report({weapons, enhancer, module, winmm})).ok());
    CHECK(CheckMods(Rule(ModPreset::MapsOnly, false), Report({enhancer})).fail == RuleFail::NotAllowed);   // HSE allowed, still not a map
    // Locked HS Enhancer passes every preset, blocked or not, as the item alone and beside a map
    const ModItem locked = Item(ModKind::HseLocked, "HSEnhancer", 4);
    for (uint8_t p = 0; p <= kMaxModPreset; ++p)
        for (bool block : {true, false}) {
            CHECK(CheckMods(Rule(ModPreset(p), block), Report({locked})).ok());
            CHECK(ItemPresetFail(Rule(ModPreset(p), block), locked) == RuleFail::None);
            CHECK(ItemRefused(Rule(ModPreset(p), block), locked) == RuleFail::None);
        }
    CHECK(CheckMods(Rule(ModPreset::MapsOnly), Report({map, locked})).ok());
    CHECK(CheckMods(Rule(ModPreset::Vanilla), Report({locked, helper})).fail == RuleFail::NoMods);   // the lock covers HS Enhancer only
    // A renamed HSE mod folder is still caught by the loaded-DLL signature (the Module item).
    CHECK(CheckMods(Rule(ModPreset::Anything), Report({Item(ModKind::Ue4ssMod, "TotallyLegit", 4), module})).fail == RuleFail::Hse);
    // Other mods are nobody's business but the preset's: no cheat list.
    CHECK(CheckMods(Rule(ModPreset::Anything), Report({Item(ModKind::Ue4ssMod, "BiteMod"), Item(ModKind::Ue4ssMod, "InfiniteStamina")})).ok());

    // Anything and Classes take a cut list; the others refuse it.
    ModReport big;
    for (size_t i = 0; i < kMaxModItems + 1; ++i) big.items.push_back(Item(ModKind::MapPak, ("m" + std::to_string(100 + i) + ".pak").c_str()));
    big.complete = true; NormalizeReport(big);
    CHECK(CheckMods(Rule(ModPreset::Anything), big).ok() && CheckMods(Rule(ModPreset::Classes), big).ok());
    CHECK(CheckMods(Rule(ModPreset::MapsOnly), big).fail == RuleFail::TooMany && CheckMods(Rule(ModPreset::Vanilla), big).fail == RuleFail::TooMany);
}

static void TestLists() {
    const ModItem weapons = Item(ModKind::ContentPak, "ModularWeapons.pak", 2);
    const ModItem other = Item(ModKind::ContentPak, "ModularWeapons.pak", 9);   // same name, another build
    ModRule custom = Rule(ModPreset::Custom);
    custom.list = ParseAllowList(" ModularWeapons.pak*" + Sha256Hex(Sha(2)).substr(0, 8) + " , +Tavern.pak, ImmersiveCombat ");
    CHECK(custom.list.size() == 3 && custom.list[0].pin.size() == 8 && !custom.list[0].required && custom.list[1].required && custom.list[2].pin.empty());
    CHECK(CheckMods(custom, Report({weapons, Item(ModKind::MapPak, "tavern.pak", 1)})).ok());
    RuleResult r = CheckMods(custom, Report({weapons}));
    CHECK(r.fail == RuleFail::Missing && r.mod == "Tavern.pak");
    r = CheckMods(custom, Report({other, Item(ModKind::MapPak, "Tavern.pak", 1)}));
    CHECK(r.fail == RuleFail::NotAllowed && r.mod == "ModularWeapons.pak");                      // the pin decides
    CHECK(CheckMods(custom, Report({Item(ModKind::MapPak, "Tavern.pak", 1), Item(ModKind::MapPak, "Other.pak", 3)})).fail == RuleFail::NotAllowed);
    CHECK(CheckMods(custom, Report({Item(ModKind::MapPak, "Tavern.pak", 1), Item(ModKind::ContentPak, "zWillieFace_P.pak", 5)})).fail == RuleFail::NotAllowed);   // an extra off the list, cosmetic or not
    CHECK(CheckMods(custom, Report({Item(ModKind::MapPak, "Tavern.pak", 1), Item(ModKind::Proxy, "dxgi.dll", 5)})).ok());   // a proxy DLL is no list item

    // Bad entries are skipped: a short pin, a marker inside, an empty one.
    CHECK(ParseAllowList("a*12, b*zzzzzzzz, , c|+d*ABCDEF01").size() == 2);
    CHECK(ParseAllowList("c|+d*ABCDEF01")[1].pin == "abcdef01");

    // Bundle: the pack's paks, all required and pinned.
    PackManifest m; m.name = "Arena"; m.version = "1";
    PackPak p1; p1.file = "ArenaMap_P.pak"; p1.size = 10; p1.sha = Sha(1); m.paks.push_back(p1);
    PackPak p2; p2.file = "ArenaGear_P.pak"; p2.size = 20; p2.sha = Sha(2); m.paks.push_back(p2);
    ModRule bundle = Rule(ModPreset::Bundle); bundle.list = BundleList(m);
    CHECK(bundle.list.size() == 2 && bundle.list[0].required && bundle.list[1].pin == Sha256Hex(Sha(2)).substr(0, 8));
    CHECK(CheckMods(bundle, Report({Item(ModKind::MapPak, "ArenaMap_P.pak", 1), Item(ModKind::ContentPak, "ArenaGear_P.pak", 2)})).ok());
    CHECK(CheckMods(bundle, Report({Item(ModKind::MapPak, "ArenaMap_P.pak", 1)})).fail == RuleFail::Missing);
    r = CheckMods(bundle, Report({Item(ModKind::MapPak, "ArenaMap_P.pak", 1), Item(ModKind::ContentPak, "ArenaGear_P.pak", 2), weapons}));
    CHECK(r.fail == RuleFail::NotAllowed && r.mod == "ModularWeapons.pak");                       // an extra pak no longer passes

    // Custom with no list: what the host runs.
    ModReport host = Report({weapons, Item(ModKind::Ue4ssStock, "Keybinds"), Item(ModKind::Module, "HSEnhancer"), Item(ModKind::HseLocked, "HSEnhancer", 9)}, true);
    std::vector<ModAllow> mine = ListFromReport(host);
    CHECK(mine.size() == 1 && mine[0].name == "ModularWeapons.pak" && !mine[0].required);
}

static void TestEffective() {
    ModRule chosen = Rule(ModPreset::MapsOnly);
    ModReport clean = Report({});
    ModReport cheater = Report({Item(ModKind::Module, "HSEnhancer")});
    CHECK(EffectiveRule(chosen, clean, Official::Yes) == chosen);
    {   // Classes takes any mod, like Anything; HS Enhancer follows the one switch there too. A host that runs HSE cannot block it.
        const ModRule classes = Rule(ModPreset::Classes, false);
        CHECK(CheckMods(classes, Report({Item(ModKind::Module, "HSEnhancer"), Item(ModKind::Ue4ssMod, "HSEnhancer"), Item(ModKind::Ue4ssMod, "HalfWolverine"), Item(ModKind::Proxy, "winmm.dll")})).ok());
        CHECK(CheckMods(classes, Report({Item(ModKind::MapPak, "Arena.pak"), Item(ModKind::ContentPak, "Armour.pak")})).ok());
        CHECK(CheckMods(classes, Report({Item(ModKind::Ue4ssMod, "BiteMod"), Item(ModKind::Ue4ssMod, "HulkMod")})).ok());
        CHECK(CheckMods(Rule(ModPreset::Classes), Report({Item(ModKind::Module, "HSEnhancer")})).fail == RuleFail::Hse);
        CHECK(!EffectiveRule(Rule(ModPreset::Classes), cheater, Official::Yes).blockHse);
        CHECK(CheckMods(Rule(ModPreset::MapsOnly), Report({Item(ModKind::Module, "HSEnhancer")})).fail == RuleFail::Hse);   // the other rules are unchanged
    }
    CHECK(!EffectiveRule(chosen, cheater, Official::Yes).blockHse);
    CHECK(EffectiveRule(chosen, Report({Item(ModKind::Ue4ssMod, "BiteMod")}), Official::Yes).blockHse);   // only HSE itself lifts the switch
    CHECK(!EffectiveRule(chosen, clean, Official::No).officialOnly && EffectiveRule(chosen, clean, Official::Unknown).officialOnly);
}

static void TestLobbyData() {
    ModRule r = Rule(ModPreset::Custom); r.officialOnly = false;
    r.list = {{"Tavern.pak", "1a2b3c4d", true}, {"ImmersiveCombat", "", false}};
    LobbyModData d = EncodeLobbyRule(r);
    CHECK(d.rule == "3" && d.hse == "1" && d.official == "0" && d.list == "+Tavern.pak*1a2b3c4d|ImmersiveCombat");
    CHECK(DecodeLobbyRule(d) == r);
    CHECK(EncodeLobbyRule(Rule(ModPreset::MapsOnly)).list.empty());
    CHECK(EncodeLobbyRule(Rule(ModPreset::Anything, false)).hse == "0" && !DecodeLobbyRule(EncodeLobbyRule(Rule(ModPreset::Anything, false))).blockHse);
    for (uint8_t p = 0; p <= kMaxModPreset; ++p) { ModRule x = Rule(ModPreset(p)); if (p != 2 && p != 3) CHECK(DecodeLobbyRule(EncodeLobbyRule(x)) == x); }
    // No rule published: Anything with HS Enhancer blocked.
    CHECK(DecodeLobbyRule({}) == NoRule() && NoRule().preset == ModPreset::Anything && NoRule().blockHse && !NoRule().officialOnly);
    // Garbage reads strict.
    LobbyModData bad{"9", "x", "", ""};
    ModRule b = DecodeLobbyRule(bad);
    CHECK(b.preset == ModPreset::Vanilla && b.blockHse && !b.officialOnly);
    CHECK(DecodeLobbyRule({"12", "0", "1", ""}).preset == ModPreset::Vanilla);
    // A list too long for lobby data publishes its hash only; the browser leaves the check to the host.
    ModRule longList = Rule(ModPreset::Custom);
    for (int i = 0; i < 64; ++i) longList.list.push_back({std::string(70, char('a' + i % 26)) + std::to_string(i), "", false});
    const std::string enc = EncodeLobbyModList(longList.list);
    CHECK(enc.size() == 17 && enc[0] == '#');
    ModRule seen = DecodeLobbyRule(EncodeLobbyRule(longList));
    CHECK(seen.listHashOnly && seen.list.empty());
    RuleResult res = CheckMods(seen, Report({Item(ModKind::ContentPak, "x.pak", 1)}));
    CHECK(res.fail == RuleFail::Unchecked && res.ok());
    CHECK(CheckMods(seen, Report({Item(ModKind::Module, "HSEnhancer")})).fail == RuleFail::Hse);   // HS Enhancer still caught
}

static void TestBuildGate() {
    const std::string host = ModBuildString("0.8.0", "abc1234", "1a2b3c4d5e6f");
    const std::string same = ModBuildString("0.8.0", "abc1234", "1a2b3c4d5e6f");
    const std::string other = ModBuildString("0.8.0", "abc1234", "ffffffffffff");
    const std::string none = ModBuildString("0.8.0", "abc1234");
    CHECK(BuildHashOf(host) == "1a2b3c4d5e6f" && BuildHashOf(none).empty() && BuildHashOf("x #1A2B3C4D5E6F").empty() && BuildHashOf("x #123").empty());
    CHECK(CheckJoinBuild(host, same, false) == BuildGate::Ok);
    CHECK(CheckJoinBuild(host, other, false) == BuildGate::Different);
    CHECK(CheckJoinBuild(host, none, false) == BuildGate::NoHash);
    CHECK(CheckJoinBuild(host, other, true) == BuildGate::Ok);
    CHECK(CheckJoinBuild(none, other, false) == BuildGate::Ok);                                  // the host cannot check
    CHECK(BuildGateText(BuildGate::Different, host, other).find("#ffffffffffff") != std::string::npos);
    CHECK(BuildGateText(BuildGate::Ok, host, same).empty());
}

static void TestOfficial() {
    CHECK(OfficialOf(true, Sha(1), Sha(1)) == Official::Yes);
    CHECK(OfficialOf(true, Sha(1), Sha(2)) == Official::No);
    CHECK(OfficialOf(false, Sha(1), Sha(1)) == Official::Unknown);
    CHECK(OfficialOf(true, Sha256Digest{}, Sha256Digest{}) == Official::Unknown);
    for (Official o : {Official::Yes, Official::No, Official::Unknown}) CHECK(ParseOfficial(OfficialName(o)) == o);
    CHECK(ParseOfficial("garbage") == Official::Unknown);
    CHECK(OfficialMayJoin(true, Official::Yes, false));
    CHECK(OfficialMayJoin(true, Official::Unknown, true) && !OfficialMayJoin(true, Official::Unknown, false));
    CHECK(!OfficialMayJoin(true, Official::No, true));
    CHECK(OfficialMayJoin(false, Official::No, false));
}

static void TestGrace() {
    ModGrace g;
    CHECK(StepGrace(g, true, 1000) == GraceStep::None && !g.active);
    CHECK(StepGrace(g, false, 1000) == GraceStep::Started && g.active && g.deadlineMs == 1000 + kModRuleGraceMs);
    CHECK(GraceSecondsLeft(g, 1000) == 60 && GraceSecondsLeft(g, 1000 + kModRuleGraceMs - 1) == 1);
    CHECK(StepGrace(g, false, 1000 + kModRuleGraceMs - 1) == GraceStep::None);
    CHECK(StepGrace(g, false, 1000 + kModRuleGraceMs) == GraceStep::Remove);
    ModGrace h;
    StepGrace(h, false, 0);
    CHECK(StepGrace(h, true, 5000) == GraceStep::Cleared && !h.active);
    CHECK(StepGrace(h, false, 6000) == GraceStep::Started && h.deadlineMs == 6000 + kModRuleGraceMs);   // a fresh 60 s
    CHECK(GraceSecondsLeft(ModGrace{}, 0) == 0);
}

// A host who changed nothing allows no mods; a bad mod_rule= falls back to the same, never to something looser.
static void TestDefaultIsNoMods() {
    const ModChoice c;
    CHECK(c.preset == ModPreset::Vanilla && c.blockHse && !c.officialOnly);
    CHECK(ModRule{}.preset == ModPreset::Vanilla && ModRule{}.blockHse);
    ModPreset bad = c.preset;
    CHECK(!ParseModPreset("nonsense", bad) && bad == ModPreset::Vanilla);
    const ModRule rule = Rule(c.preset);
    CHECK(CheckMods(rule, Report({})).fail == RuleFail::None);
    CHECK(CheckMods(rule, Report({Item(ModKind::MapPak, "Hole.pak")})).fail == RuleFail::NoMods);
    CHECK(CheckMods(rule, Report({Item(ModKind::ContentPak, "Sword.pak")})).fail == RuleFail::NoMods);
}

static void TestLobbyView() {
    const auto Rule = [](ModPreset p, bool blockHse = true) { ModRule r = ::Rule(p, blockHse); r.officialOnly = false; return r; };   // the INI default
    CHECK(LobbyRuleTag(Rule(ModPreset::Vanilla)).empty());
    CHECK(LobbyRuleTag(Rule(ModPreset::MapsOnly)) == "mods: Maps only");
    CHECK(LobbyRuleTag(Rule(ModPreset::Vanilla, false)) == "mods: Vanilla, HS Enhancer ok");
    { ModRule r = Rule(ModPreset::Vanilla); r.officialOnly = true; CHECK(LobbyRuleTag(r) == "mods: Vanilla, official only"); }
    CHECK(LobbyRuleTag(Rule(ModPreset::Anything, false)) == "mods: Anything, HS Enhancer ok");
    CHECK(LobbyRuleTag(Rule(ModPreset::MapsOnly, false)) == "mods: Maps only, HS Enhancer ok");
    { ModRule r = Rule(ModPreset::MapsOnly); r.officialOnly = true; CHECK(LobbyRuleTag(r) == "mods: Maps only, official only"); }
    { ModRule r = Rule(ModPreset::Classes, false); r.officialOnly = true; CHECK(LobbyRuleTag(r) == "mods: Classes, HS Enhancer ok, official only"); }
    CHECK(LobbyRuleTag(NoRule()) == "mods: Anything");
    CHECK(ViewLobbyRule(Rule(ModPreset::Anything, false), Report({}), Official::Yes).tag == "mods: Anything, HS Enhancer ok");
    const ModReport clean = Report({Item(ModKind::MapPak, "Arena_P.pak", 3)});
    const ModReport cheat = Report({Item(ModKind::Module, "HSEnhancer")});
    // HS Enhancer in a lobby that blocks it: the note and the sentence say so.
    {
        const LobbyModView v = ViewLobbyRule(Rule(ModPreset::MapsOnly), cheat, Official::Yes);
        CHECK(v.reason == "Blocks HS Enhancer" && v.text == "This lobby blocks HS Enhancer unless HSMMod can lock it: remove it, or join a lobby that allows it." && v.tag == "mods: Maps only");
    }
    CHECK(ViewLobbyRule(Rule(ModPreset::Anything, false), cheat, Official::Yes).reason.empty());
    CHECK(ViewLobbyRule(Rule(ModPreset::Classes, false), cheat, Official::Yes).reason.empty());
    // Still scanning: no verdict (the host decides).
    { ModReport scanning = cheat; scanning.complete = false; CHECK(ViewLobbyRule(Rule(ModPreset::Vanilla), scanning, Official::No).reason.empty()); }
    // Official only: an unofficial build is turned away, an unknown one is not.
    {
        ModRule r = Rule(ModPreset::MapsOnly); r.officialOnly = true;
        CHECK(ViewLobbyRule(r, clean, Official::No).reason == "Official builds only");
        CHECK(ViewLobbyRule(r, clean, Official::No).text == kUnofficialText);
        CHECK(ViewLobbyRule(r, clean, Official::Unknown).reason.empty());
        CHECK(ViewLobbyRule(r, clean, Official::Yes).reason.empty());
    }
    // A hash-only list: joinable here, the sentence says the host checks.
    {
        ModRule r = Rule(ModPreset::Custom); r.listHashOnly = true;
        const LobbyModView v = ViewLobbyRule(r, clean, Official::Yes);
        CHECK(v.reason.empty() && v.text == "This lobby has a long mod list: the host checks it when you join.");
    }
}

static void TestHeldIgnores() {
    for (MsgType t : {MsgType::Input, MsgType::HitClaim, MsgType::PropLease, MsgType::HandOff, MsgType::LoadoutChange, MsgType::ProgressionVote, MsgType::GiveUp, MsgType::Pose})
        CHECK(HeldPlayerIgnores(t));
    // Joining, chat and the mod messages go through.
    for (MsgType t : {MsgType::Ready, MsgType::BlobChunk, MsgType::Chat, MsgType::ModList, MsgType::ModAnswer, MsgType::PackStatus, MsgType::PingTable, MsgType::MeshHeard, MsgType::Voice})
        CHECK(!HeldPlayerIgnores(t));
}

static void TestPlayerMods() {
    const ModRule maps = Rule(ModPreset::MapsOnly);
    const ModReport clean = Report({Item(ModKind::MapPak, "Arena_P.pak", 3)});
    const ModReport cheat = Report({Item(ModKind::Module, "HSEnhancer")});
    // Held: no list yet waits, then is removed at kModListWaitMs.
    {
        PlayerMods p; p.heldSinceMs = 1000;
        CHECK(StepPlayerMods(p, maps, true, 1000 + kModListWaitMs - 1).step == PlayerModStep::Wait);
        const PlayerModVerdict v = StepPlayerMods(p, maps, true, 1000 + kModListWaitMs);
        CHECK(v.step == PlayerModStep::Remove && v.reason == RejectReason::ModsNotAllowed && v.detail == kNoModListText);
        CHECK(p.held);
    }
    // Held: an incomplete list waits up to kModCompleteWaitMs, not kModListWaitMs.
    {
        PlayerMods p; p.heldSinceMs = 0; p.haveList = true; p.report = clean; p.report.complete = false;
        CHECK(StepPlayerMods(p, maps, true, kModListWaitMs).step == PlayerModStep::Wait);
        CHECK(StepPlayerMods(p, maps, true, kModCompleteWaitMs - 1).step == PlayerModStep::Wait);
        const PlayerModVerdict v = StepPlayerMods(p, maps, true, kModCompleteWaitMs);
        CHECK(v.step == PlayerModStep::Remove && v.detail == kModScanSlowText);
    }
    // Held: a passing list admits once; a cheat is removed with the text naming it; unofficial with ModifiedClient.
    {
        PlayerMods p; p.haveList = true; p.report = clean;
        CHECK(StepPlayerMods(p, maps, true, 10).step == PlayerModStep::Admit);
        CHECK(!p.held);
        CHECK(StepPlayerMods(p, maps, true, 20).step == PlayerModStep::Wait);
    }
    {
        PlayerMods p; p.haveList = true; p.report = cheat;
        const PlayerModVerdict v = StepPlayerMods(p, maps, true, 10);
        CHECK(v.step == PlayerModStep::Remove && v.reason == RejectReason::ModsNotAllowed);
        CHECK(v.detail == "This lobby blocks HS Enhancer unless HSMMod can lock it: remove it, or join a lobby that allows it.");
        CHECK(p.held);
    }
    {
        ModRule off = maps; off.officialOnly = true;
        PlayerMods p; p.haveList = true; p.report = clean; p.official = Official::No;
        const PlayerModVerdict v = StepPlayerMods(p, off, false, 10);
        CHECK(v.step == PlayerModStep::Remove && v.reason == RejectReason::ModifiedClient && v.detail == kUnofficialText);
        p.official = Official::Yes;
        CHECK(StepPlayerMods(p, off, false, 10).step == PlayerModStep::Admit);
    }
    // Admitted: the rule tightens -> warned, still in at the deadline - 1, removed at the deadline; fixed in time -> cleared.
    {
        PlayerMods p; p.held = false; p.haveList = true; p.report = clean;
        const ModRule vanilla = Rule(ModPreset::Vanilla);
        const PlayerModVerdict w = StepPlayerMods(p, vanilla, true, 100);
        CHECK(w.step == PlayerModStep::Warn && w.reason == RejectReason::ModsNotAllowed && w.detail == "This lobby is Vanilla: remove Arena_P.pak.");
        CHECK(StepPlayerMods(p, vanilla, true, 100 + kModRuleGraceMs - 1).step == PlayerModStep::Wait);
        // A rescan in progress neither clears nor removes.
        p.report.complete = false;
        CHECK(StepPlayerMods(p, vanilla, true, 100 + kModRuleGraceMs).step == PlayerModStep::Wait);
        p.report.complete = true;
        const PlayerModVerdict r = StepPlayerMods(p, vanilla, true, 100 + kModRuleGraceMs);
        CHECK(r.step == PlayerModStep::Remove && r.reason == RejectReason::ModsNotAllowed);
    }
    {
        PlayerMods p; p.held = false; p.haveList = true; p.report = cheat;
        CHECK(StepPlayerMods(p, maps, true, 0).step == PlayerModStep::Warn);
        p.report = clean;
        CHECK(StepPlayerMods(p, maps, true, 5).step == PlayerModStep::Clear);
        CHECK(!p.grace.active);
        CHECK(StepPlayerMods(p, maps, true, 6).step == PlayerModStep::Wait);
    }
    // Every detail fits Reject.detail.
    {
        PlayerMods p; p.haveList = true; p.report = Report({Item(ModKind::ContentPak, std::string(kMaxModItemName, 'x').c_str())});
        const PlayerModVerdict v = StepPlayerMods(p, maps, true, 0);
        CHECK(v.step == PlayerModStep::Remove && v.detail.size() == kMaxRejectDetail);
    }
}

static void TestTexts() {
    ModRule maps = Rule(ModPreset::MapsOnly);
    CHECK(RuleSummary(maps) == "Maps only, HS Enhancer blocked, official builds only");
    CHECK(RuleSummary(Rule(ModPreset::Anything, false)) == "Anything, HS Enhancer allowed, official builds only");
    RuleResult r; r.fail = RuleFail::Hse; r.mod = "HSEnhancer";
    CHECK(RuleFailText(maps, r) == "This lobby blocks HS Enhancer unless HSMMod can lock it: remove it, or join a lobby that allows it.");
    CHECK(RuleFailShort(r) == "Blocks HS Enhancer");
    {   // The browser row names what to take out; "No Abyss.pak" read as a file the player was missing.
        RuleResult x;
        x.fail = RuleFail::NotAllowed;
        x.mod = "Abyss.pak";
        CHECK(RuleFailShort(x) == "Remove Abyss.pak");
        CHECK(RuleFailText(maps, x) == "This lobby (Maps only) does not allow Abyss.pak: remove it.");
        // Every refused mod shows, so a player does not find them one restart at a time.
        ModRule rule;
        rule.preset = ModPreset::MapsOnly;
        ModReport rep = Report({Item(ModKind::ContentPak, "Abyss.pak", 1), Item(ModKind::MapPak, "Hole.pak", 2),
                                Item(ModKind::ContentPak, "Highway.pak", 3), Item(ModKind::ContentPak, "Other.pak", 4)});
        const RuleResult many = CheckMods(rule, rep);
        CHECK(many.fail == RuleFail::NotAllowed && many.mod == "Abyss.pak" && many.more == 2);
        CHECK(RuleFailShort(many) == "Remove Abyss.pak +2");
        CHECK(RuleFailText(maps, many) == "This lobby (Maps only) does not allow Abyss.pak and 2 more: remove them.");
        rule.preset = ModPreset::Vanilla;
        CHECK(RuleFailText(rule, CheckMods(rule, rep)) == "This lobby is Vanilla: remove Hole.pak and 3 more.");   // map paks sort first
    }
    r.fail = RuleFail::NotAllowed; r.mod = "ModularWeapons.pak";
    CHECK(RuleFailText(maps, r).find("(Maps only) does not allow ModularWeapons.pak") != std::string::npos);
    for (uint8_t f = 1; f <= uint8_t(RuleFail::Unchecked); ++f) { RuleResult x; x.fail = RuleFail(f); x.mod = "M"; CHECK(!RuleFailText(maps, x).empty() && !RuleFailShort(x).empty()); }
    CHECK(RuleFailText(maps, RuleResult{}).empty());
    for (uint8_t p = 0; p <= kMaxModPreset; ++p) { ModPreset q; CHECK(ParseModPreset(ModPresetKey(ModPreset(p)), q) && q == ModPreset(p)); }
    ModPreset q; CHECK(!ParseModPreset("lol", q) && ParseModPreset("MAPS", q) && q == ModPreset::MapsOnly);

    std::vector<ModItem> items = Ue4ssStock();
    items.push_back(Item(ModKind::MapPak, "Tavern.pak", 1));
    items.push_back(Item(ModKind::Ue4ssMod, "HSEnhancer", 2));
    items.push_back(Item(ModKind::Ue4ssMod, "HSHelper", 3));
    items.push_back(Item(ModKind::Proxy, "winmm.dll", 4));
    const std::string line = ReportLogLine(Report(items, true));
    CHECK(line == "1 map paks, 0 content paks, ue4ss 8 (6 stock), proxies [winmm.dll], HS Enhancer");
    CHECK(ReportLogLine(Report({Item(ModKind::ContentPak, "A.pak", 1)})) == "0 map paks, 1 content paks, ue4ss off, proxies []");
    { ModReport cut = Report({Item(ModKind::Module, "HSEnhancer")}); cut.truncated = true; cut.complete = false;
      CHECK(ReportLogLine(cut) == "0 map paks, 0 content paks, ue4ss off, proxies [], HS Enhancer (list cut) (scan running)"); }
    CHECK(ReportLogLine(ModReport{}).find("ue4ss off") != std::string::npos);
    CHECK(ReportLogLine(Report({Item(ModKind::HseLocked, "HSEnhancer")})) == "0 map paks, 0 content paks, ue4ss off, proxies [], HS Enhancer (locked)");
    CHECK(ReportLogLine(Report({Item(ModKind::HseLocked, "HSEnhancer"), Item(ModKind::Module, "HSEnhancer")})) == "0 map paks, 0 content paks, ue4ss off, proxies [], HS Enhancer");
}

static void TestWire() {
    ModListMsg m;
    m.official = Official::Yes;
    std::vector<ModItem> items = Ue4ssStock();
    items.push_back(Item(ModKind::MapPak, "Tavern.pak", 1));
    items.push_back(Item(ModKind::Module, "HSEnhancer"));
    items.push_back(Item(ModKind::HseLocked, "HSEnhancer", 5));
    m.report = Report(items, true);
    std::vector<uint8_t> b = Encode(m);
    ModListMsg back;
    CHECK(Decode(b.data(), b.size(), back) && back.official == Official::Yes && back.report == m.report);
    // Every truncation fails; a trailing byte fails.
    for (size_t n = 0; n < b.size(); ++n) { ModListMsg x; CHECK(!Decode(b.data(), n, x)); }
    b.push_back(0); { ModListMsg x; CHECK(!Decode(b.data(), b.size(), x)); } b.pop_back();
    // Bad fields fail: official 3, an unknown flag, a kind 0 or 8, a hashed byte 2.
    { auto c = b; c[4] = 3; ModListMsg x; CHECK(!Decode(c.data(), c.size(), x)); }
    { auto c = b; c[5] = 8; ModListMsg x; CHECK(!Decode(c.data(), c.size(), x)); }
    { auto c = b; c[7] = 0; ModListMsg x; CHECK(!Decode(c.data(), c.size(), x)); }
    { auto c = b; c[7] = 8; ModListMsg x; CHECK(!Decode(c.data(), c.size(), x)); }
    { auto c = b; c[7] = 7; ModListMsg x; CHECK(Decode(c.data(), c.size(), x) && x.report.items[0].kind == ModKind::HseLocked); }   // 7, the last kind, decodes
    { auto c = b; c[8] = 2; ModListMsg x; CHECK(!Decode(c.data(), c.size(), x)); }
    // A full list fits a packet.
    ModListMsg full;
    for (size_t i = 0; i < kMaxModItems; ++i) full.report.items.push_back(Item(ModKind::ContentPak, (std::string(kMaxModItemName - 4, char('a' + i % 26)) + std::to_string(i % 10) + ".pa").c_str(), uint8_t(i + 1)));
    full.report.complete = true;
    b = Encode(full);
    CHECK(b.size() <= kMaxPacket);
    CHECK(Decode(b.data(), b.size(), back) && back.report.items.size() == kMaxModItems);

    // SessionMods: the union with slot masks.
    ModReport a = Report({Item(ModKind::MapPak, "Tavern.pak", 1), Item(ModKind::Ue4ssMod, "HSHelper", 2)});
    ModReport c = Report({Item(ModKind::MapPak, "Tavern.pak", 1), Item(ModKind::MapPak, "Tavern.pak", 9)});
    SessionModsMsg s = BuildSessionMods({{0, &a}, {3, &c}, {20, &a}, {1, nullptr}});
    CHECK(s.entries.size() == 3 && !s.truncated);
    CHECK(s.entries[0].item.name == "Tavern.pak" && (s.entries[0].slots == 0x9 || s.entries[1].slots == 0x9));
    b = Encode(s);
    SessionModsMsg sb;
    CHECK(Decode(b.data(), b.size(), sb) && sb.entries == s.entries && !sb.truncated);
    for (size_t n = 0; n < b.size(); ++n) { SessionModsMsg x; CHECK(!Decode(b.data(), n, x)); }
    { auto e = b; e[e.size() - 1] = 0; e[e.size() - 2] = 0; SessionModsMsg x; CHECK(!Decode(e.data(), e.size(), x)); }   // an empty mask
    // Wrong type.
    b = Encode(m);
    { SessionModsMsg x; CHECK(!Decode(b.data(), b.size(), x)); }
}

int main() {
    TestNames();
    TestNormalize();
    TestHse();
    TestClassify();
    TestPresets();
    TestLists();
    TestEffective();
    TestLobbyData();
    TestBuildGate();
    TestOfficial();
    TestGrace();
    TestPlayerMods();
    TestHeldIgnores();
    TestDefaultIsNoMods();
    TestLobbyView();
    TestTexts();
    TestWire();
    // The update notice: public builds hear of public releases only; a dev-only release or a local build follows the dev ones.
    CHECK(UpdateChannelOf(Official::Yes, false) == UpdateChannel::Public && UpdateChannelOf(Official::Yes, true) == UpdateChannel::Public);
    CHECK(UpdateChannelOf(Official::No, false) == UpdateChannel::Dev);                                   // a local build of a public version
    CHECK(UpdateChannelOf(Official::Unknown, true) == UpdateChannel::Dev);                               // a test build ahead of the public ones
    CHECK(UpdateChannelOf(Official::Unknown, false) == UpdateChannel::Public);                           // an old public build past the list
    if (g_fail) { std::printf("test_hsm_mod_rules: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_mod_rules: PASS\n");
    return 0;
}

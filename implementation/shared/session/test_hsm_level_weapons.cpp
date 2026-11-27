// Unit tests for hsm_level_weapons.h (the host's loose level weapons, protocol rev 53, #4297). Run via implementation/shared/run_tests.sh test_hsm_level_weapons.
#include "session/hsm_level_weapons.h"
#include "session/hsm_budget.h"
#include <cstdio>
#include <map>
#include <tuple>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static const char* kModular = "/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C";
static const char* kMace = "/Game/Assets/Weapons/Blueprints/Built_Weapons/BP_Weapon_Mace_Flanged.BP_Weapon_Mace_Flanged_C";
static const char* kBread = "/Game/Assets/Food/Blueprints/BP_Food_Bread.BP_Food_Bread_C";
static const char* kDoublet = "/Game/Assets/Armor/BP_Armor_Modular_Core_Body_Doublet_Arming.BP_Armor_Modular_Core_Body_Doublet_Arming_C";

// A weapon passport save file of the shape CheckPassportSave takes (hsm_gvas.h): the weapon array with `fill` value bytes.
static std::vector<uint8_t> WeaponSave(size_t fill = 40, uint8_t salt = 0) {
    std::vector<uint8_t> b;
    auto u8 = [&](uint8_t v) { b.push_back(v); };
    auto u16 = [&](uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); };
    auto i32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) u8(uint8_t(uint32_t(v) >> (8 * i))); };
    auto str = [&](const std::string& t) { i32(int32_t(t.size() + 1)); for (char c : t) u8(uint8_t(c)); u8(0); };
    for (char c : std::string("GVAS")) u8(uint8_t(c));
    i32(3); i32(522); i32(1012);
    u16(5); u16(4); u16(4); i32(int32_t(2147486353u));
    str("++Halfsword+Main");
    i32(3); i32(0);
    str(kPassportSaveClass);
    u8(0);
    str("Available Weapons 1H");
    str("ArrayProperty"); i32(1);
    str("StructProperty"); i32(2);
    str("Str_Passport_Weapon1"); i32(1);
    str("/Game/Blueprints/Structure/Passports/Str_Passport_Weapon1"); i32(0);
    str("4ffe2bb5-464d-c6a5-8138-3cadd2c7eb0d"); i32(0);
    i32(int32_t(fill)); u8(0);
    for (size_t i = 0; i < fill; ++i) u8(uint8_t(i + salt));
    str("None"); i32(0);
    return b;
}

static LevelWeaponMsg Weapon(const char* cls, float x, float y, float z, std::vector<uint8_t> item = {}) {
    LevelWeaponMsg m; m.cls = cls; m.x = x; m.y = y; m.z = z; m.yaw = 45; m.item = std::move(item);
    return m;
}

static void TestWire() {
    CHECK(CheckPassportSave(WeaponSave().data(), WeaponSave().size(), nullptr, crypt::kMaxItemBytes));
    LevelWeaponMsg m = Weapon(kModular, 120.5f, -40, 15, WeaponSave());
    m.travel = 7; m.seq = 3; m.index = 2; m.count = 5; m.pitch = -10; m.roll = 359;
    auto b = Encode(m);
    LevelWeaponMsg y;
    CHECK(Decode(b.data(), b.size(), y) && y.travel == 7 && y.seq == 3 && y.index == 2 && y.count == 5 && y.cls == kModular
          && y.x == 120.5f && y.y == -40 && y.z == 15 && y.pitch == -10 && y.yaw == 45 && y.roll == 359 && y.item == m.item);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { auto x = b; x.push_back(0); CHECK(!Decode(x.data(), x.size(), y)); }
    // A premade weapon without a passport: its class as it is.
    { LevelWeaponMsg p = Weapon(kMace, 1, 2, 3); p.travel = 7; p.seq = 3; p.count = 1; auto e = Encode(p); CHECK(Decode(e.data(), e.size(), y) && y.item.empty() && y.cls == kMace); }
    // An empty list: count 0, index 0, no weapon.
    { LevelWeaponMsg e0; e0.travel = 7; e0.seq = 4; auto e = Encode(e0); CHECK(Decode(e.data(), e.size(), y) && y.count == 0 && y.cls.empty()); }
    // Refused: seq 0; index out of the list; a count over the cap; a weapon in an empty list; a class that is no weapon (food,
    // armour, outside /Game); a save file that is not one; a place or angle out of range.
    auto refused = [&](LevelWeaponMsg z) { auto e = Encode(z); LevelWeaponMsg o; return !Decode(e.data(), e.size(), o); };
    { LevelWeaponMsg z = m; z.seq = 0; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.index = 5; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.count = kMaxLevelWeaponList + 1; z.index = 0; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.count = kMaxLevelWeaponList; z.index = kMaxLevelWeaponList - 1; CHECK(!refused(z)); }
    { LevelWeaponMsg z = m; z.count = 0; z.index = 0; CHECK(refused(z)); }
    { LevelWeaponMsg z; z.seq = 4; z.index = 1; CHECK(refused(z)); }
    { LevelWeaponMsg z; z.seq = 4; z.item = WeaponSave(); CHECK(refused(z)); }
    for (const char* c : {kBread, kDoublet, "/Script/Engine.Actor", "/Game/Assets/Weapons/../BP_Weapon_X.BP_Weapon_X_C"}) { LevelWeaponMsg z = m; z.cls = c; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.item.resize(80, 0); z.item[0] = 'X'; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.x = std::nanf(""); CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.z = 1e7f; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.yaw = 361; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.roll = std::nanf(""); CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.roll = -400; CHECK(refused(z)); }
    { LevelWeaponMsg z = m; z.pitch = 361; CHECK(refused(z)); }
    CHECK(BudgetClassOf(MsgType::LevelWeapons) == BudgetClass::Control);
}

static void TestSplit() {
    std::vector<LevelWeaponMsg> w = {Weapon(kModular, 1, 0, 0, WeaponSave()), Weapon(kMace, 2, 0, 0), Weapon(kModular, 3, 0, 0, WeaponSave(40, 9))};
    auto parts = SplitLevelWeapons(9, 5, w);
    CHECK(parts.size() == 3);
    for (size_t i = 0; i < parts.size(); ++i)
        CHECK(parts[i].travel == 9 && parts[i].seq == 5 && parts[i].index == i && parts[i].count == 3 && parts[i].cls == w[i].cls && parts[i].x == w[i].x && parts[i].item == w[i].item);
    auto empty = SplitLevelWeapons(9, 6, {});
    CHECK(empty.size() == 1 && empty[0].count == 0 && empty[0].index == 0 && empty[0].seq == 6 && empty[0].travel == 9);
    CHECK(SplitLevelWeapons(9, 0, w).empty());
    CHECK(SplitLevelWeapons(9, 7, std::vector<LevelWeaponMsg>(kMaxLevelWeaponList + 1, Weapon(kMace, 0, 0, 0))).empty());
    CHECK(SplitLevelWeapons(9, 7, std::vector<LevelWeaponMsg>(kMaxLevelWeaponList, Weapon(kMace, 0, 0, 0))).size() == kMaxLevelWeaponList);
    // Every part goes through the wire.
    for (const LevelWeaponMsg& p : parts) { auto e = Encode(p); LevelWeaponMsg o; CHECK(Decode(e.data(), e.size(), o)); }
}

// A machine's loose weapons, as the test's world: class and place.
using World = std::vector<std::tuple<std::string, float, float, float, std::vector<uint8_t>>>;

// Carries out a follow on a world the way gameplay/level_weapons_sync.cpp does: purge, then spawn each, `fails` the spawns that fail.
static void Run(LevelWeaponFollow& f, World& world, std::map<size_t, int> fails = {}) {
    for (int guard = 0; guard < 1000; ++guard) {
        const LevelWeaponFollow::Step s = f.Next();
        if (s == LevelWeaponFollow::Step::Wait || s == LevelWeaponFollow::Step::Done) return;
        if (s == LevelWeaponFollow::Step::Purge) { world.clear(); f.Purged(); continue; }
        const LevelWeaponMsg& m = f.Current();
        const size_t i = f.CurrentIndex();
        if (fails[i] > 0) { --fails[i]; f.Spawned(false); continue; }
        world.emplace_back(m.cls, m.x, m.y, m.z, m.item);
        f.Spawned(true);
    }
}

// The report (#4297): the host's level and a client's had different loose weapons (11 against 10 in the same round; a weapon
// the host saw was not on the client's floor). After the list, the client has exactly the host's.
static void TestClientEndsWithTheHostsWeapons() {
    const World host = {{kModular, 100, 0, 0, WeaponSave(40, 1)}, {kMace, 200, 50, 0, {}}, {kModular, 300, 0, 10, WeaponSave(40, 2)},
                        {kModular, 300, 0, 10, WeaponSave(40, 3)}};   // two in one chest: both are sent
    World client = {{kModular, 100, 0, 0, WeaponSave(40, 7)}, {kMace, 900, 0, 0, {}}, {kModular, 50, 50, 0, WeaponSave(40, 8)}};
    std::vector<LevelWeaponMsg> w;
    for (const auto& [cls, x, y, z, item] : host) w.push_back(Weapon(cls.c_str(), x, y, z, item));
    LevelWeaponFollow f;
    const auto parts = SplitLevelWeapons(12, 1, w);
    // The client's match has not begun when the list comes (the host's begins first): it waits, its weapons stay.
    for (size_t i = 0; i + 1 < parts.size(); ++i) CHECK(f.Add(parts[i], 12) == LevelWeaponFollow::Added::Pending);
    CHECK(f.Next() == LevelWeaponFollow::Step::Wait);
    CHECK(f.Add(parts.back(), 12) == LevelWeaponFollow::Added::Complete && f.HaveList() && f.List().size() == host.size());
    Run(f, client);
    CHECK(f.Next() == LevelWeaponFollow::Step::Wait && client.size() == 3);
    f.Baseline();
    CHECK(f.Next() == LevelWeaponFollow::Step::Purge);
    Run(f, client);
    CHECK(f.Next() == LevelWeaponFollow::Step::Done);
    CHECK(client == host);
    CHECK(f.SpawnedCount() == host.size() && f.FailedCount() == 0);
    // The joiner's resend of the same list changes nothing.
    for (const LevelWeaponMsg& p : parts) CHECK(f.Add(p, 12) == LevelWeaponFollow::Added::Stale);
    Run(f, client);
    CHECK(client == host);
}

static void TestBaselineFirst() {
    // The client's match began first (it heard the MatchInfo before the list): it waits for the list, then carries it out.
    World client = {{kMace, 1, 1, 1, {}}};
    LevelWeaponFollow f;
    f.Baseline();
    CHECK(f.Next() == LevelWeaponFollow::Step::Wait);
    Run(f, client);
    CHECK(client.size() == 1);
    const auto parts = SplitLevelWeapons(3, 2, {Weapon(kModular, 5, 5, 5, WeaponSave())});
    CHECK(f.Add(parts[0], 3) == LevelWeaponFollow::Added::Complete);
    Run(f, client);
    CHECK(client.size() == 1 && std::get<0>(client[0]) == kModular && std::get<1>(client[0]) == 5);
    // An empty list: the client's own go and none come.
    LevelWeaponFollow g; g.Baseline();
    World c2 = {{kMace, 1, 1, 1, {}}, {kModular, 2, 2, 2, WeaponSave()}};
    CHECK(g.Add(SplitLevelWeapons(3, 9, {})[0], 3) == LevelWeaponFollow::Added::Complete);
    Run(g, c2);
    CHECK(c2.empty() && g.Next() == LevelWeaponFollow::Step::Done);
}

static void TestOtherTravelAndOrder() {
    LevelWeaponFollow f;
    std::vector<LevelWeaponMsg> w = {Weapon(kMace, 1, 0, 0), Weapon(kMace, 2, 0, 0), Weapon(kMace, 3, 0, 0)};
    // A list for the last level (it was on its way when this machine travelled): not this level's.
    for (const LevelWeaponMsg& p : SplitLevelWeapons(4, 1, w)) CHECK(f.Add(p, 5) == LevelWeaponFollow::Added::OtherTravel);
    CHECK(!f.HaveList());
    // Parts in any order; an index heard twice is Stale and counts once.
    auto parts = SplitLevelWeapons(5, 2, w);
    CHECK(f.Add(parts[2], 5) == LevelWeaponFollow::Added::Pending);
    CHECK(f.Add(parts[2], 5) == LevelWeaponFollow::Added::Stale);
    CHECK(f.Add(parts[0], 5) == LevelWeaponFollow::Added::Pending);
    // Another count for the same list is refused, and the list being built stays.
    { LevelWeaponMsg z = parts[1]; z.count = 4; CHECK(f.Add(z, 5) == LevelWeaponFollow::Added::Rejected); }
    CHECK(!f.HaveList());
    CHECK(f.Add(parts[1], 5) == LevelWeaponFollow::Added::Complete);
    CHECK(f.List().size() == 3 && f.List()[0].x == 1 && f.List()[1].x == 2 && f.List()[2].x == 3);
    // A half-built list goes when another seq starts.
    LevelWeaponFollow g;
    auto a = SplitLevelWeapons(5, 3, w), b = SplitLevelWeapons(5, 4, {Weapon(kMace, 9, 0, 0)});
    CHECK(g.Add(a[0], 5) == LevelWeaponFollow::Added::Pending);
    CHECK(g.Add(b[0], 5) == LevelWeaponFollow::Added::Complete && g.List().size() == 1 && g.Seq() == 4);
    CHECK(g.Add(a[1], 5) == LevelWeaponFollow::Added::Pending && g.HaveList() && g.Seq() == 4);   // the old seq starts a new build; the list held stays
    // Malformed parts are refused without touching anything.
    LevelWeaponFollow h;
    { LevelWeaponMsg z = a[0]; z.seq = 0; CHECK(h.Add(z, 5) == LevelWeaponFollow::Added::Rejected); }
    { LevelWeaponMsg z = a[0]; z.index = 3; CHECK(h.Add(z, 5) == LevelWeaponFollow::Added::Rejected); }
    { LevelWeaponMsg z = a[0]; z.count = 0; z.index = 1; CHECK(h.Add(z, 5) == LevelWeaponFollow::Added::Rejected); }
    { LevelWeaponMsg z = a[0]; z.count = kMaxLevelWeaponList + 1; CHECK(h.Add(z, 5) == LevelWeaponFollow::Added::Rejected); }
    CHECK(!h.HaveList() && h.Next() == LevelWeaponFollow::Step::Wait);
    // A level change forgets the list and the baseline.
    f.Baseline(); f.Clear();
    CHECK(!f.HaveList() && f.Next() == LevelWeaponFollow::Step::Wait);
}

static void TestSpawnFailures() {
    LevelWeaponFollow f;
    f.Baseline();
    std::vector<LevelWeaponMsg> w = {Weapon(kMace, 1, 0, 0), Weapon(kModular, 2, 0, 0, WeaponSave()), Weapon(kMace, 3, 0, 0)};
    CHECK(f.Add(SplitLevelWeapons(1, 1, w)[0], 1) == LevelWeaponFollow::Added::Pending);
    CHECK(f.Add(SplitLevelWeapons(1, 1, w)[1], 1) == LevelWeaponFollow::Added::Pending);
    CHECK(f.Add(SplitLevelWeapons(1, 1, w)[2], 1) == LevelWeaponFollow::Added::Complete);
    World world;
    // Weapon 0 fails twice then spawns; weapon 1 never spawns (left out after its tries); weapon 2 spawns.
    Run(f, world, {{0, 2}, {1, 99}});
    CHECK(f.Next() == LevelWeaponFollow::Step::Done && world.size() == 2 && std::get<1>(world[0]) == 1 && std::get<1>(world[1]) == 3);
    CHECK(f.SpawnedCount() == 2 && f.FailedCount() == 1);
    // The last try says so; the ones before do not.
    LevelWeaponFollow g; g.Baseline();
    CHECK(g.Add(SplitLevelWeapons(1, 1, {Weapon(kMace, 1, 0, 0)})[0], 1) == LevelWeaponFollow::Added::Complete);
    g.Purged();
    for (uint8_t i = 1; i < kLevelWeaponSpawnTries; ++i) CHECK(!g.Spawned(false) && g.Next() == LevelWeaponFollow::Step::Spawn);
    CHECK(g.Spawned(false) && g.Next() == LevelWeaponFollow::Step::Done && g.FailedCount() == 1);
    CHECK(!g.Spawned(true) && g.SpawnedCount() == 0);   // nothing left to spawn
    // A new list for the same level after one was carried out: carried out again from the purge.
    CHECK(g.Add(SplitLevelWeapons(1, 2, {Weapon(kMace, 4, 0, 0)})[0], 1) == LevelWeaponFollow::Added::Complete);
    CHECK(g.Next() == LevelWeaponFollow::Step::Purge && g.FailedCount() == 0);
}

int main() {
    TestWire();
    TestSplit();
    TestClientEndsWithTheHostsWeapons();
    TestBaselineFirst();
    TestOtherTravelAndOrder();
    TestSpawnFailures();
    if (g_fail) { std::printf("test_hsm_level_weapons: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_level_weapons: ok\n");
    return 0;
}

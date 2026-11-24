// Unit tests for hsm_crypt_loot.h (the shared Crypts run's loot and doors, protocol rev 50). Run via implementation/shared/run_tests.sh test_hsm_crypt_loot.
#include "session/hsm_crypt_loot.h"
#include "session/hsm_budget.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::crypt;

static const char* kBread = "/Game/Assets/Food/Blueprints/BP_Food_Bread.BP_Food_Bread_C";
static const char* kKey = "/Game/Assets/Weapons/Blueprints/Built_Weapons/Tools/BP_Weapon_Tool_Key.BP_Weapon_Tool_Key_C";
static const char* kAltar = "/Game/Assets/Props/Blueprints/BP_Altar_Structure.BP_Altar_Structure_C";
static const char* kDresser = "/Game/Assets/Props/Furniture/Blueprints/BP_Prop_Furniture_Dresser_Large_A_002.BP_Prop_Furniture_Dresser_Large_A_002_C";
static const char* kChest = "/Game/Assets/Props/Blueprints/BP_Container_Chest_A.BP_Container_Chest_A_C";

static const char* kSword = "/Game/Assets/Weapons/Blueprints/ModularWeaponBP.ModularWeaponBP_C";
static const char* kBottle = "/Game/Assets/Weapons/Blueprints/Built_Weapons/Consumable/BP_Weapon_Consumable_Bottle_Water.BP_Weapon_Consumable_Bottle_Water_C";
static const char* kDoublet = "/Game/Assets/Armor/BP_Armor_Modular_Core_Body_Doublet_Arming.BP_Armor_Modular_Core_Body_Doublet_Arming_C";

// A passport save file of the shape CheckPassportSave takes (hsm_gvas.h), holding one property: the weapon or armour array, with `fill`
// as its value bytes. The engine's own serializer writes the real thing in the game; this is the shape only.
static std::vector<uint8_t> ItemSave(bool armor, size_t fill = 40) {
    std::vector<uint8_t> b;
    auto u8 = [&](uint8_t v) { b.push_back(v); };
    auto u16 = [&](uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); };
    auto i32 = [&](int32_t v) { for (int i = 0; i < 4; ++i) u8(uint8_t(uint32_t(v) >> (8 * i))); };
    auto str = [&](const std::string& t) { i32(int32_t(t.size() + 1)); for (char c : t) u8(uint8_t(c)); u8(0); };
    // the array-of-struct type tree: ArrayProperty(StructProperty(Name(/path),guid))
    auto type = [&](const std::string& t) {
        str("ArrayProperty"); i32(1);
        str("StructProperty"); i32(2);
        const size_t open = t.find("StructProperty(") + 15;
        const size_t paren = t.find('(', open);
        str(t.substr(open, paren - open)); i32(1);
        const size_t close = t.find(')', paren);
        str(t.substr(paren + 1, close - paren - 1)); i32(0);
        const size_t guid = close + 2;
        str(t.substr(guid, t.size() - guid - 2)); i32(0);
    };
    for (char c : std::string("GVAS")) u8(uint8_t(c));
    i32(3); i32(522); i32(1012);
    u16(5); u16(4); u16(4); i32(int32_t(2147486353u));
    str("++Halfsword+Main");
    i32(3); i32(2);
    for (int i = 0; i < 2; ++i) { for (int k = 0; k < 16; ++k) u8(uint8_t(i + k)); i32(i); }
    str(hsm::kPassportSaveClass);
    u8(0);
    str(armor ? "Available Custom Armor" : "Available Weapons 1H");
    type(hsm::kGameProgressProperties[armor ? 8 : 5].type);
    i32(int32_t(fill)); u8(0);
    for (size_t i = 0; i < fill; ++i) u8(uint8_t(i));
    str("None"); i32(0);
    return b;
}

static void TestClass() {
    for (const char* c : {kBread, kKey, kAltar, kChest}) CHECK(LootClassOk(c));
    // The pre56 test: the host's dressers (BP_Container_Master_C children) were refused, so the client's dressers never came.
    CHECK(LootClassOk(kDresser) && !LootTakeable(kDresser));
    CHECK(!LootClassOk("/Game/Assets/Props/Furniture/Blueprints/BP_Prop_Furniture_Chair_A.BP_Prop_Furniture_Chair_A_C"));   // other furniture is placed on each machine
    CHECK(!LootClassOk(""));
    CHECK(!LootClassOk("/Game/Assets/Weapons/BP_Sword.BP_Sword_C"));                   // a weapon is placed with its passport (`item`), never as a bare class path
    CHECK(!LootClassOk(kSword) && !LootClassOk(kDoublet));
    CHECK(!LootClassOk("/Script/Engine.Actor"));                                        // not under /Game
    CHECK(!LootClassOk("/Game/Assets/../Food/BP_Food_Bread.BP_Food_Bread_C"));          // no climbing
    CHECK(!LootClassOk("/Game/Assets/Food/BP_Food_Bread.BP_Food_Bread"));               // a class, not an object
    CHECK(!LootClassOk("/Game/Assets/Food/BP_Food_Br ead.BP_Food_Br ead_C"));           // a space
    CHECK(!LootClassOk(std::string("/Game/") + std::string(kMaxLootClass, 'a') + ".BP_Food_X_C"));
    CHECK(LootTakeable(kBread) && LootTakeable(kKey) && !LootTakeable(kChest) && !LootTakeable(kAltar));
    CHECK(IsLootPointClass("BP_Procedural_LootPoint_Chest_C") && IsLootPointClass("BP_Loot_Food_C") && !IsLootPointClass("BP_Container_Chest_A_C"));
    CHECK(IsItemClass("BP_Food_Bread_C") && IsItemClass("BP_Weapon_Tool_Key_C") && !IsItemClass("BP_Container_Chest_A_C"));
    CHECK(IsItemClass("ModularWeaponBP_C") && IsItemClass("ModularWeaponBP_Axe_C") && IsItemClass("BP_Weapon_Consumable_Bottle_Water_C") && IsItemClass("BP_Armor_Modular_Core_Body_Doublet_Arming_C"));
    CHECK(!IsItemClass("Willie_BP_C") && !IsItemClass("Weapon_Fists_C") && !IsItemClass("BP_Altar_Structure_C"));
    CHECK(LootTakeable(kSword) && LootTakeable(kBottle) && LootTakeable(kDoublet));
    CHECK(ItemClassOk(kSword, kItemWeapon) && ItemClassOk(kBottle, kItemWeapon) && ItemClassOk(kKey, kItemWeapon) && ItemClassOk(kDoublet, kItemArmor));
    CHECK(!ItemClassOk(kSword, kItemArmor) && !ItemClassOk(kDoublet, kItemWeapon) && !ItemClassOk(kChest, kItemWeapon) && !ItemClassOk(kBread, kItemWeapon) && !ItemClassOk(kSword, kItemNone));
    CHECK(!ItemClassOk("/Game/A/../ModularWeaponBP.ModularWeaponBP_C", kItemWeapon) && !ItemClassOk("/Script/Engine/ModularWeaponBP.ModularWeaponBP_C", kItemWeapon));
    CHECK(!ItemClassOk("/Game/A/ModularWeaponBP.ModularWeaponBP", kItemWeapon) && !ItemClassOk("/Game/A/Modular WeaponBP.Modular WeaponBP_C", kItemWeapon));
    CHECK(IsRollingContainerClass("BP_Container_Chest_A_C") && IsRollingContainerClass("BP_Altar_Chest_C") && !IsRollingContainerClass("BP_Food_Bread_C"));
}

static CryptLootMsg Loot(uint16_t room, uint16_t index, const char* cls) {
    CryptLootMsg m; m.room = room; m.index = index; m.cls = cls; m.x = 1500.5f; m.y = -20; m.z = 7; m.pitch = 1; m.yaw = 90; m.roll = -2;
    return m;
}

static void TestWire() {
    CryptLootMsg m = Loot(3, 5, kBread), y;
    auto b = Encode(m);
    CHECK(Decode(b.data(), b.size(), y) && y.room == 3 && y.index == 5 && y.cls == kBread && y.x == 1500.5f && y.yaw == 90 && y.roll == -2 && y.state == kLootPresent);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { CryptLootMsg z = m; z.state = kLootTaken; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.state == kLootTaken); }
    { CryptLootMsg z = Loot(4, 2, kDresser); auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.cls == kDresser && y.room == 4 && y.index == 2); }
    { CryptLootMsg z = m; z.state = 2; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptLootMsg z = m; z.room = 0; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptLootMsg z = m; z.room = uint16_t(kMaxRooms + 1); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptLootMsg z = m; z.index = kMaxLootPerRoom; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptLootMsg z = m; z.cls = "/Game/Foo/BP_Sword.BP_Sword_C"; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptLootMsg z = m; z.z = INFINITY; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptLootMsg z = m; z.yaw = 400; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { auto e = b; e.push_back(0); CHECK(!Decode(e.data(), e.size(), y)); }   // trailing byte
    CHECK(BudgetClassOf(MsgType::CryptLoot) == BudgetClass::Control);
}

static void TestItem() {
    const std::vector<uint8_t> sword = ItemSave(false), armor = ItemSave(true);
    CHECK(CheckPassportSave(sword.data(), sword.size(), nullptr, kMaxItemBytes) && CheckPassportSave(armor.data(), armor.size(), nullptr, kMaxItemBytes));
    CryptLootMsg m = Loot(2, 1, kSword), y;
    m.itemKind = kItemWeapon; m.item = sword;
    auto b = Encode(m);
    CHECK(Decode(b.data(), b.size(), y) && y.itemKind == kItemWeapon && y.item == sword && y.cls == kSword && y.state == kLootPresent);
    for (size_t cut = 0; cut < b.size(); cut += 7) CHECK(!Decode(b.data(), cut, y));
    { CryptLootMsg z = Loot(2, 2, kDoublet); z.itemKind = kItemArmor; z.item = armor; auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.itemKind == kItemArmor); }
    { CryptLootMsg z = m; z.itemKind = kItemArmor; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }              // a sword class under the armour kind
    { CryptLootMsg z = m; z.itemKind = 3; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptLootMsg z = m; z.item.clear(); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }                        // present, but no save file
    { CryptLootMsg z = m; z.item[0] = 'X'; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }                       // not a save file
    { CryptLootMsg z = m; z.item.push_back(7); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }                   // trailing data in the save
    { CryptLootMsg z = Loot(2, 1, kBread); z.item = sword; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }        // a save file under no item kind
    { CryptLootMsg z = m; z.state = kLootTaken; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }                   // taken: carries nothing
    { CryptLootMsg z = m; z.state = kLootTaken; z.item.clear(); auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.state == kLootTaken && y.item.empty() && y.itemKind == kItemWeapon); }
    { CryptLootMsg z = m; z.item = ItemSave(false, kMaxItemBytes); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }   // over the cap
    CHECK(b.size() < 2048);                                                                                                   // a message with an item stays small

    // the book keeps the item until it is taken, then drops it
    LootBook host, client;
    CryptLootMsg a, c;
    CHECK(host.Add(3, kSword, 10, 20, 30, 0, 0, 0, a, kItemWeapon, &sword) && a.itemKind == kItemWeapon && a.item == sword);
    CHECK(!host.Add(3, kSword, 12, 20, 30, 0, 0, 0, c, kItemWeapon, &sword));          // the same thing again
    CHECK(!host.Add(3, kSword, 900, 0, 0, 0, 0, 0, c, kItemWeapon, nullptr));          // an item kind with no save file
    CHECK(!host.Add(3, kSword, 900, 0, 0, 0, 0, 0, c));                                // a sword as a bare class path
    CHECK(!host.Add(3, kDoublet, 900, 0, 0, 0, 0, 0, c, kItemWeapon, &sword));         // armour class under the weapon kind
    CHECK(host.Add(3, kDoublet, 900, 0, 0, 0, 0, 0, c, kItemArmor, &armor) && c.index == 1);
    CHECK(client.Receive(a) == LootBook::Got::New && client.Find(3, 0)->item == sword);
    CHECK(host.MarkTaken(3, 0) && host.Find(3, 0)->item.empty() && host.Find(3, 0)->itemKind == kItemWeapon && host.Find(3, 0)->state == kLootTaken);
    CryptLootMsg t = *host.Find(3, 0);
    auto te = Encode(t);
    CHECK(Decode(te.data(), te.size(), y) && client.Receive(y) == LootBook::Got::NowTaken && client.Find(3, 0)->item.empty());
    CHECK(client.Receive(a) == LootBook::Got::Repeat && client.Find(3, 0)->item.empty());   // a late present record does not bring the sword (or its file) back
}

static void TestLootBook() {
    LootBook host, client;
    CryptLootMsg a, b, c;
    CHECK(host.Add(2, kBread, 10, 20, 30, 0, 0, 0, a) && a.room == 2 && a.index == 0);
    CHECK(host.Add(2, kChest, 500, 0, 0, 0, 0, 0, b) && b.index == 1);
    CHECK(!host.Add(2, kBread, 12, 21, 30, 0, 0, 0, c));                 // the same thing again
    CHECK(host.Add(4, kBread, 10, 20, 30, 0, 0, 0, c) && c.room == 4 && c.index == 0);   // another room, its own index
    CHECK(!host.Add(0, kBread, 0, 0, 0, 0, 0, 0, c));
    CHECK(!host.Add(2, "/Game/X/BP_Sword.BP_Sword_C", 0, 0, 0, 0, 0, 0, c));
    CHECK(host.Count() == 3);

    // the client takes the host's records, in any order, twice
    CHECK(client.Receive(b) == LootBook::Got::New && client.Receive(a) == LootBook::Got::New && client.Receive(a) == LootBook::Got::Repeat);
    CHECK(client.Count() == 2 && client.Find(2, 0) && client.Find(2, 0)->cls == kBread);
    { CryptLootMsg w = a; w.cls = kChest; CHECK(client.Receive(w) == LootBook::Got::Bad); }   // same address, another thing

    // a take: only takeable things, once
    CHECK(!host.MarkTaken(2, 1));            // a chest is not taken
    CHECK(host.MarkTaken(2, 0) && !host.MarkTaken(2, 0));
    CHECK(!host.MarkTaken(9, 9));
    CryptLootMsg t = a; t.state = kLootTaken;
    CHECK(client.Receive(t) == LootBook::Got::NowTaken && client.Receive(t) == LootBook::Got::Repeat);
    CHECK(client.Find(2, 0)->state == kLootTaken);
    // a present record after a take does not bring the thing back
    CHECK(client.Receive(a) == LootBook::Got::Repeat && client.Find(2, 0)->state == kLootTaken);
    // a joiner gets all records (taken ones too, so it does not spawn them)
    LootBook joiner; for (const CryptLootMsg& m : host.All()) joiner.Receive(m);
    CHECK(joiner.Count() == 3 && joiner.Find(2, 0)->state == kLootTaken);
    // a record the client never saw, sent as taken, is recorded as taken
    LootBook late; CryptLootMsg tk = Loot(5, 2, kKey); tk.state = kLootTaken;
    CHECK(late.Receive(tk) == LootBook::Got::New && late.Find(5, 2)->state == kLootTaken);
    // the next index after a received record does not collide with it
    CHECK(late.Add(5, kBread, 0, 0, 0, 0, 0, 0, c) && c.index == 3);
}

static void TestDoorBook() {
    DoorBook host, client;
    CryptDoorMsg d, e;
    CHECK(host.Add(kDoorGate, 100, 200, 0, 90, kDoorClosed, d) && d.seq == 1);
    CHECK(host.Add(kDoorTearDown, 100, 200, 0, 0, kDoorClosed, e) && e.seq == 2);   // another kind at the same place is another door
    CHECK(host.Find(kDoorGate, 130, 220, 10) == 1 && host.Find(kDoorTearDown, 99, 200, 0) == 2);
    CHECK(host.Find(kDoorGate, 900, 0, 0) == 0 && host.Find(kDoorUnbreakable, 100, 200, 0) == 0);
    CryptDoorMsg bad;
    CHECK(!host.Add(7, 0, 0, 0, 0, kDoorClosed, bad) && !host.Add(kDoorGate, INFINITY, 0, 0, 0, kDoorClosed, bad));

    CHECK(client.Receive(e) == DoorBook::Got::Bad);                       // seq 2 before seq 1: out of order
    CHECK(client.Receive(d) == DoorBook::Got::New && client.Receive(e) == DoorBook::Got::New && client.Receive(d) == DoorBook::Got::Repeat);

    // forward only: closed -> open -> gone, never back
    CHECK(host.Advance(1, kDoorOpen) && !host.Advance(1, kDoorOpen) && !host.Advance(1, kDoorClosed));
    CHECK(host.Advance(1, kDoorGone) && !host.Advance(1, kDoorOpen));
    CHECK(host.Get(1)->state == kDoorGone);
    CHECK(!host.Advance(0, kDoorOpen) && !host.Advance(9, kDoorOpen) && !host.Advance(2, 3));
    CryptDoorMsg open = *host.Get(1); open.state = kDoorOpen;
    CHECK(client.Receive(open) == DoorBook::Got::Advanced && client.Get(1)->state == kDoorOpen);
    CryptDoorMsg back = open; back.state = kDoorClosed;
    CHECK(client.Receive(back) == DoorBook::Got::Repeat && client.Get(1)->state == kDoorOpen);
    CryptDoorMsg wrong = open; wrong.kind = kDoorUnbreakable;
    CHECK(client.Receive(wrong) == DoorBook::Got::Bad);
    // a joiner gets the book in order
    DoorBook joiner; for (const CryptDoorMsg& m : host.All()) CHECK(joiner.Receive(m) == DoorBook::Got::New);
    CHECK(joiner.Count() == 2 && joiner.Get(1)->state == kDoorGone);
}

int main() {
    TestClass(); TestWire(); TestItem(); TestLootBook(); TestDoorBook();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_crypt_loot: ok\n");
    return 0;
}

// Loot, doors and keys of the shared Crypts run (protocol rev 50, docs/PLAN_CRYPTS_COOP.md, PR D). The host's rooms roll their
// own dice (a loot point picks a container, a container its contents; the dungeon decides which doorways get a barricade). A
// client never rolls them: the host reads what its loot points spawned and sends each one as a class path and a place:
//   CryptLoot 76   both ways, reliable: one placed thing (a container, an altar, a food item, a key) of the host's room, by the
//                  room's seq and its index in that room. host -> client: it is there, or (state taken) it was taken.
//                  client -> host: "I took it" (state taken); the host checks it and sends it on to everyone.
//   CryptDoor 74   both ways (session/hsm_crypt_rooms.h): a door, gate or barricade and its state, forward only.
// This file is the pure part: the message, the class allowlist, and the two books both ends keep. gameplay/crypts_loot.cpp carries
// it out in the game. A weapon, armour, bottle or lantern rolls a passport (GI_Settings Generate Weapon / Generate Armor): that
// struct is one item. It travels as `item`, a SaveGame file of the passport save (hsm_gvas.h) holding the struct as element 0 of
// its weapon or armour array, written and read by the engine's own serializer (docs/PLAN_CRYPTS_COOP.md, PR D2).
#pragma once
#include "session/hsm_crypt_rooms.h"
#include "session/hsm_gvas.h"
#include "protocol/hsm_protocol.h"
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hsm::crypt {

constexpr size_t kMaxLootClass = 240;       // a class path (the wire carries it with a one-byte length)
constexpr uint16_t kMaxLootPerRoom = 256;
constexpr size_t kMaxLoot = 2048;           // a run's loot records
constexpr double kLootTakenCm = 120.0;      // an item this far from where it was placed has been picked up (or kicked off)
constexpr double kLootSameCm = 5.0;         // two records of one class this close are one
constexpr double kDoorSameCm = 150.0;       // a door actor this close to a known door of its kind is that door
constexpr size_t kMaxItemBytes = 8192;      // one item's save file (a weapon's passport is well under 2 KB; the header is most of it)
constexpr double kGearTakenCm = 150.0;      // weapons and armour settle (they are physics bodies): this far from where they were placed is a take

// CryptLootMsg::state
constexpr uint8_t kLootPresent = 0;
constexpr uint8_t kLootTaken = 1;
// CryptLootMsg::itemKind: which passport struct `item` holds
constexpr uint8_t kItemNone = 0;
constexpr uint8_t kItemWeapon = 1;    // Str_Passport_Weapon1 (a weapon, bottle, lantern or key)
constexpr uint8_t kItemArmor = 2;     // Str_Passport_Armor1

struct CryptLootMsg {
    uint16_t room = 0;           // the room's seq (CryptRoom); never 0
    uint16_t index = 0;          // the n-th thing the host saw placed in that room (0..)
    uint8_t state = kLootPresent;
    std::string cls;             // the class path ("/Game/Assets/Food/Blueprints/BP_Food_Bread.BP_Food_Bread_C")
    float x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0;
    uint8_t itemKind = kItemNone;     // PR D2: the passport struct of a weapon or armour piece, if this is one
    std::vector<uint8_t> item;        // its save file (itemKind != 0 and state present); empty once the thing is taken
};

namespace detail {
inline bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
// The name after the last '.' of a class path ("BP_Food_Bread_C"), or the whole text.
inline std::string ClassLeaf(const std::string& path) {
    const size_t dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}
}  // namespace detail

// The things the wire may place: a Blueprint path under /Game of safe characters whose class name is a container, an altar,
// food or the key. Anything else a peer names is refused (a client must not be able to spawn an arbitrary class).
inline bool LootClassOk(const std::string& path) {
    if (path.empty() || path.size() > kMaxLootClass || !detail::StartsWith(path, "/Game/")) return false;
    for (char c : path) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '.';
        if (!ok) return false;
    }
    if (path.find("..") != std::string::npos) return false;
    const std::string leaf = detail::ClassLeaf(path);
    if (leaf.size() < 5 || leaf.compare(leaf.size() - 2, 2, "_C") != 0) return false;
    return detail::StartsWith(leaf, "BP_Container_") || detail::StartsWith(leaf, "BP_Altar_") ||
           detail::StartsWith(leaf, "BP_Food_") || detail::StartsWith(leaf, "BP_Weapon_Tool_Key") ||   // without an `item`; ItemClassOk is the other way in
           detail::StartsWith(leaf, "BP_Prop_Furniture_Dresser_");   // a dresser a loot point rolls: a container (BP_Container_Master_C) by another name
}
// A class name of a weapon ("ModularWeaponBP_C", "BP_Weapon_Consumable_Bottle_Water_C", the key) or an armour piece ("BP_Armor_Modular_Core_Body_Doublet_Arming_C").
inline bool IsWeaponClassName(const std::string& name) {
    return detail::StartsWith(name, "ModularWeaponBP") || detail::StartsWith(name, "BP_Weapon_");
}
inline bool IsArmorClassName(const std::string& name) { return detail::StartsWith(name, "BP_Armor_"); }
// A class name ("BP_Food_Bread_C") of a thing a player can pick up (so its taking is synced): food, the key, weapons and armour.
inline bool IsItemClass(const std::string& name) {
    return detail::StartsWith(name, "BP_Food_") || IsWeaponClassName(name) || IsArmorClassName(name);
}
// The same for a class path. Containers and altars stay where they are.
inline bool LootTakeable(const std::string& path) { return IsItemClass(detail::ClassLeaf(path)); }
// Whether the class path is a safe Blueprint class under /Game (the same characters and shape LootClassOk wants) and its class name is
// a weapon (kind 1) or armour (kind 2) class: the only things a peer's `item` may name.
inline bool ItemClassOk(const std::string& path, uint8_t kind) {
    if (path.empty() || path.size() > kMaxLootClass || !detail::StartsWith(path, "/Game/")) return false;
    for (char c : path) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '.';
        if (!ok) return false;
    }
    if (path.find("..") != std::string::npos) return false;
    const std::string leaf = detail::ClassLeaf(path);
    if (leaf.size() < 5 || leaf.compare(leaf.size() - 2, 2, "_C") != 0) return false;
    return kind == kItemWeapon ? IsWeaponClassName(leaf) : kind == kItemArmor ? IsArmorClassName(leaf) : false;
}
// Whether a class name in the game ("BP_Procedural_LootPoint_Chest_C", "BP_Loot_Food_C") is a loot point: the actors that roll
// the dice and spawn the product. A client blocks their BeginPlay and destroys them.
inline bool IsLootPointClass(const std::string& name) {
    return detail::StartsWith(name, "BP_Procedural_LootPoint_") || detail::StartsWith(name, "BP_Loot_");
}
// The product classes whose own dice (their contents) a client must not roll: containers and altars.
inline bool IsRollingContainerClass(const std::string& name) {
    return detail::StartsWith(name, "BP_Container_") || detail::StartsWith(name, "BP_Altar_");
}

inline bool PoseOk(float a, float b, float c) { return CoordOk(a) && CoordOk(b) && CoordOk(c); }

inline std::vector<uint8_t> Encode(const CryptLootMsg& m) {
    Writer w; w.header(MsgType::CryptLoot);
    w.u16(m.room); w.u16(m.index); w.u8(m.state); w.str(m.cls, kMaxLootClass);
    w.f32(m.x); w.f32(m.y); w.f32(m.z); w.f32(m.pitch); w.f32(m.yaw); w.f32(m.roll);
    w.u8(m.itemKind);
    const size_t n = m.item.size() < kMaxItemBytes ? m.item.size() : kMaxItemBytes;
    w.u16(uint16_t(n));
    w.buf.insert(w.buf.end(), m.item.begin(), m.item.begin() + long(n));
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, CryptLootMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::CryptLoot) return false;
    CryptLootMsg v;
    v.room = r.u16(); v.index = r.u16(); v.state = r.u8(); v.cls = r.str(kMaxLootClass);
    v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); v.pitch = r.f32(); v.yaw = r.f32(); v.roll = r.f32();
    v.itemKind = r.u8();
    const size_t itemLen = r.u16();
    if (itemLen > kMaxItemBytes) return false;
    v.item.reserve(itemLen);
    for (size_t i = 0; i < itemLen && r.ok(); ++i) v.item.push_back(r.u8());
    if (!r.done()) return false;
    if (v.room == 0 || v.room > kMaxRooms || v.index >= kMaxLootPerRoom || v.state > kLootTaken) return false;
    if (v.itemKind > kItemArmor) return false;
    if (v.itemKind == kItemNone ? (!v.item.empty() || !LootClassOk(v.cls)) : !ItemClassOk(v.cls, v.itemKind)) return false;
    // A present item carries its save file, which must be the shape this build writes (the engine will load it); a taken one carries nothing.
    if (v.itemKind != kItemNone && (v.state == kLootPresent ? !CheckPassportSave(v.item.data(), v.item.size(), nullptr, kMaxItemBytes) : !v.item.empty())) return false;
    if (!PoseOk(v.x, v.y, v.z)) return false;
    for (float a : {v.pitch, v.yaw, v.roll}) if (!std::isfinite(a) || std::fabs(a) > 360.0f) return false;
    m = std::move(v);
    return true;
}

// What each end keeps of the loot. The host fills it from its loot points (Add) and marks takes; a client fills it from the host's
// messages (Receive). Both address a record by (room, index).
class LootBook {
public:
    enum class Got { Bad, New, Repeat, NowTaken };

    // Host: a placed thing was seen in `room`. Fills `out` (with its index) and returns true, or false for a bad record, a full book
    // or a thing already recorded (same class, same place, same room).
    // A weapon or armour piece passes its passport's save file as `item` (itemKind kItemWeapon / kItemArmor).
    bool Add(uint16_t room, const std::string& cls, float x, float y, float z, float pitch, float yaw, float roll, CryptLootMsg& out,
             uint8_t itemKind = kItemNone, const std::vector<uint8_t>* item = nullptr) {
        if (room == 0 || room > kMaxRooms || !PoseOk(x, y, z) || all_.size() >= kMaxLoot) return false;
        if (itemKind > kItemArmor || (itemKind == kItemNone ? !LootClassOk(cls) : !ItemClassOk(cls, itemKind))) return false;
        if (itemKind != kItemNone && (!item || item->empty() || item->size() > kMaxItemBytes)) return false;
        for (const CryptLootMsg& e : all_)
            if (e.room == room && e.cls == cls && std::fabs(e.x - x) < kLootSameCm && std::fabs(e.y - y) < kLootSameCm &&
                std::fabs(e.z - z) < kLootSameCm) return false;
        uint16_t& next = next_[room];
        if (next >= kMaxLootPerRoom) return false;
        CryptLootMsg m;
        m.room = room; m.index = next++; m.state = kLootPresent; m.cls = cls;
        m.x = x; m.y = y; m.z = z; m.pitch = pitch; m.yaw = yaw; m.roll = roll;
        m.itemKind = itemKind;
        if (itemKind != kItemNone) m.item = *item;
        at_[Key(m.room, m.index)] = all_.size();
        all_.push_back(m);
        out = m;
        return true;
    }
    // Client (and the host, for a client's take): a record from the wire.
    Got Receive(const CryptLootMsg& m) {
        if (m.room == 0 || m.index >= kMaxLootPerRoom) return Got::Bad;
        auto it = at_.find(Key(m.room, m.index));
        if (it == at_.end()) {
            if (all_.size() >= kMaxLoot) return Got::Bad;
            at_[Key(m.room, m.index)] = all_.size();
            all_.push_back(m);
            uint16_t& next = next_[m.room];
            if (m.index >= next) next = uint16_t(m.index + 1);
            return Got::New;
        }
        CryptLootMsg& e = all_[it->second];
        if (e.cls != m.cls) return Got::Bad;   // the same address with another thing: a peer's mistake
        if (m.state == kLootTaken && e.state != kLootTaken) { e.state = kLootTaken; e.item.clear(); e.item.shrink_to_fit(); return Got::NowTaken; }
        return Got::Repeat;
    }
    // A thing was taken. True when this changed it (it was there and takeable).
    bool MarkTaken(uint16_t room, uint16_t index) {
        auto it = at_.find(Key(room, index));
        if (it == at_.end()) return false;
        CryptLootMsg& e = all_[it->second];
        if (e.state == kLootTaken || !LootTakeable(e.cls)) return false;
        e.state = kLootTaken;
        e.item.clear(); e.item.shrink_to_fit();       // a taken thing is never spawned again: its save file goes
        return true;
    }
    const CryptLootMsg* Find(uint16_t room, uint16_t index) const {
        auto it = at_.find(Key(room, index));
        return it == at_.end() ? nullptr : &all_[it->second];
    }
    const std::vector<CryptLootMsg>& All() const { return all_; }
    size_t Count() const { return all_.size(); }
    void Clear() { all_.clear(); at_.clear(); next_.clear(); }

private:
    static uint32_t Key(uint16_t room, uint16_t index) { return (uint32_t(room) << 16) | index; }
    std::vector<CryptLootMsg> all_;
    std::map<uint32_t, size_t> at_;
    std::map<uint16_t, uint16_t> next_;
};

// The doors, gates and barricades of the run and their states. A state only goes forward (closed, open, gone): a late or repeated
// message never closes a door again. The host numbers a door when it first sees the actor (Find, then Add); a client takes the
// host's numbers (Receive).
class DoorBook {
public:
    enum class Got { Bad, New, Advanced, Repeat };

    // The door of `kind` within kDoorSameCm of the place, or 0.
    uint16_t Find(uint8_t kind, float x, float y, float z) const {
        uint16_t best = 0; double bestD = kDoorSameCm;
        for (const CryptDoorMsg& d : all_) {
            if (d.kind != kind) continue;
            const double dist = std::sqrt(double(d.x - x) * (d.x - x) + double(d.y - y) * (d.y - y) + double(d.z - z) * (d.z - z));
            if (dist < bestD) { bestD = dist; best = d.seq; }
        }
        return best;
    }
    // Host: a new door. Returns its message (seq assigned), or false when full or bad.
    bool Add(uint8_t kind, float x, float y, float z, float yaw, uint8_t state, CryptDoorMsg& out) {
        if (kind > kDoorUnbreakable || state > kDoorGone || !PoseOk(x, y, z) || !std::isfinite(yaw) || all_.size() >= kMaxDoors) return false;
        CryptDoorMsg m;
        m.seq = uint16_t(all_.size() + 1); m.kind = kind; m.x = x; m.y = y; m.z = z; m.yaw = yaw; m.state = state;
        all_.push_back(m);
        out = m;
        return true;
    }
    // Moves a known door forward. True when it changed (a state at or behind the present one changes nothing).
    bool Advance(uint16_t seq, uint8_t state) {
        if (seq == 0 || seq > all_.size() || state > kDoorGone) return false;
        CryptDoorMsg& d = all_[seq - 1];
        if (state <= d.state) return false;
        d.state = state;
        return true;
    }
    // A client takes the host's door (its seq must be the next one or a known one; the host sends them in order, and a joiner gets all).
    Got Receive(const CryptDoorMsg& m) {
        if (m.seq == 0 || m.seq > kMaxDoors) return Got::Bad;
        if (m.seq > all_.size()) {
            if (m.seq != all_.size() + 1) return Got::Bad;
            all_.push_back(m);
            return Got::New;
        }
        CryptDoorMsg& d = all_[m.seq - 1];
        if (d.kind != m.kind) return Got::Bad;
        return Advance(m.seq, m.state) ? Got::Advanced : Got::Repeat;
    }
    const CryptDoorMsg* Get(uint16_t seq) const { return seq == 0 || seq > all_.size() ? nullptr : &all_[seq - 1]; }
    const std::vector<CryptDoorMsg>& All() const { return all_; }
    size_t Count() const { return all_.size(); }
    void Clear() { all_.clear(); }

private:
    std::vector<CryptDoorMsg> all_;
};

}  // namespace hsm::crypt

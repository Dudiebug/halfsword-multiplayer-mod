// The level's loose weapons, from the host (MsgType::LevelWeapons 78, host -> clients, reliable; protocol rev 53, #4297).
// Each machine's level places its own loose weapons, and they differ: a container rolls its contents on GI_Settings' weapon
// spawner, a random premade pick is Array_Random (the C runtime's rand(), not the shared dice), the level manager's roll
// counter runs on past rolls only the host makes ('Random Delete'), and a career's Player Tier is each player's own. A weapon
// nobody has picked up is never announced (object sync names a weapon when a hand lets go of it), so each machine kept its own
// set: a weapon a player saw lying there was not there for anyone else (and a host's pick-up made one up in the stand-in's hand).
// So the host names them: when its match begins in a level (ObjectSync::MarkBaseline) it saves each loose weapon's passport the
// way a Crypts loot item travels (session/hsm_crypt_loot.h `item`: a passport save file) and sends one LevelWeaponMsg per
// weapon: the level's travel, the list's seq, the weapon's index and the list's count, its class path, place and rotation and
// its passport. A client collects the list (LevelWeaponFollow::Add), and once its own match has begun in that level too
// (LevelWeaponFollow::Baseline) it destroys every loose weapon of its own (Step::Purge) and spawns the host's, a few a frame
// (Step::Spawn), so the loose weapons are the host's on every machine. gameplay/level_weapons_sync.cpp carries it out.
#pragma once
#include "protocol/hsm_protocol.h"
#include "session/hsm_crypt_loot.h"
#include "session/hsm_gvas.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

constexpr uint16_t kMaxLevelWeaponList = 128;   // a list's count; a level with more is not sent (every arena has under 30)
constexpr uint8_t kLevelWeaponSpawnTries = 3;   // a client tries one weapon this often, then goes on without it

struct LevelWeaponMsg {
    uint32_t travel = 0;              // the host's Travel seq of the level the list is for
    uint16_t seq = 0;                 // the list (never 0): a resend to a joiner carries the same seq
    uint16_t index = 0, count = 0;    // index < count <= kMaxLevelWeaponList; count 0: the level has none (index 0, no weapon)
    std::string cls;                  // the weapon's class path (crypt::ItemClassOk kItemWeapon); empty for an empty list
    float x = 0, y = 0, z = 0, pitch = 0, yaw = 0, roll = 0;
    std::vector<uint8_t> item;        // its passport as a save file (crypt::kMaxItemBytes); empty: the class as it is
};

inline std::vector<uint8_t> Encode(const LevelWeaponMsg& m) {
    Writer w; w.header(MsgType::LevelWeapons);
    w.u32(m.travel); w.u16(m.seq); w.u16(m.index); w.u16(m.count); w.str(m.cls, crypt::kMaxLootClass);
    w.f32(m.x); w.f32(m.y); w.f32(m.z); w.f32(m.pitch); w.f32(m.yaw); w.f32(m.roll);
    const size_t n = m.item.size() < crypt::kMaxItemBytes ? m.item.size() : crypt::kMaxItemBytes;
    w.u16(uint16_t(n));
    w.buf.insert(w.buf.end(), m.item.begin(), m.item.begin() + long(n));
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, LevelWeaponMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::LevelWeapons) return false;
    LevelWeaponMsg v;
    v.travel = r.u32(); v.seq = r.u16(); v.index = r.u16(); v.count = r.u16(); v.cls = r.str(crypt::kMaxLootClass);
    v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); v.pitch = r.f32(); v.yaw = r.f32(); v.roll = r.f32();
    const size_t itemLen = r.u16();
    if (itemLen > crypt::kMaxItemBytes) return false;
    v.item.reserve(itemLen);
    for (size_t i = 0; i < itemLen && r.ok(); ++i) v.item.push_back(r.u8());
    if (!r.done() || v.seq == 0 || v.count > kMaxLevelWeaponList) return false;
    if (v.count == 0) {
        if (v.index != 0 || !v.cls.empty() || !v.item.empty()) return false;
    } else {
        if (v.index >= v.count || !crypt::ItemClassOk(v.cls, crypt::kItemWeapon)) return false;
        // A weapon with a passport carries the save file this build writes (the engine will load it).
        if (!v.item.empty() && !CheckPassportSave(v.item.data(), v.item.size(), nullptr, crypt::kMaxItemBytes)) return false;
    }
    if (!crypt::PoseOk(v.x, v.y, v.z)) return false;
    for (float a : {v.pitch, v.yaw, v.roll}) if (!std::isfinite(a) || std::fabs(a) > 360.0f) return false;
    m = std::move(v);
    return true;
}

// The host's list as messages, one per weapon (an empty list is one message with count 0: a client must hear that too).
// `weapons` carries each weapon's cls, place and item; their travel, seq, index and count are set here. Nothing when it
// holds more than kMaxLevelWeaponList weapons, or seq is 0.
inline std::vector<LevelWeaponMsg> SplitLevelWeapons(uint32_t travel, uint16_t seq, const std::vector<LevelWeaponMsg>& weapons) {
    if (seq == 0 || weapons.size() > kMaxLevelWeaponList) return {};
    std::vector<LevelWeaponMsg> out;
    if (weapons.empty()) {
        LevelWeaponMsg m; m.travel = travel; m.seq = seq;
        out.push_back(m);
        return out;
    }
    for (size_t i = 0; i < weapons.size(); ++i) {
        LevelWeaponMsg m = weapons[i];
        m.travel = travel; m.seq = seq; m.index = uint16_t(i); m.count = uint16_t(weapons.size());
        out.push_back(std::move(m));
    }
    return out;
}

// A client's side: the host's list for this machine's level, and what to do with it once this machine's match began there.
class LevelWeaponFollow {
public:
    enum class Added : uint8_t { Pending, Complete, Stale, OtherTravel, Rejected };
    enum class Step : uint8_t { Wait, Purge, Spawn, Done };

    // One message of the host's. myTravel: the Travel seq this machine is in; a list for another level is OtherTravel.
    // Complete once every index of the list is in. A message of the list already complete, or an index heard twice (the
    // joiner's resend of the same list), is Stale; another seq drops a half-built list. Rejected: another count for the
    // same list.
    Added Add(const LevelWeaponMsg& m, uint32_t myTravel) {
        if (m.travel != myTravel) return Added::OtherTravel;
        if (m.seq == 0 || m.count > kMaxLevelWeaponList || (m.count == 0 ? m.index != 0 : m.index >= m.count)) return Added::Rejected;
        if (have_ && m.travel == travel_ && m.seq == seq_) return Added::Stale;
        if (pending_ && (m.travel != pendingTravel_ || m.seq != pendingSeq_)) pending_ = false;
        if (!pending_) {
            pending_ = true; pendingTravel_ = m.travel; pendingSeq_ = m.seq; pendingCount_ = m.count;
            got_.assign(m.count, LevelWeaponMsg{}); gotMask_.assign(m.count, false); gotN_ = 0;
        } else if (m.count != pendingCount_) return Added::Rejected;
        if (m.count > 0) {
            if (gotMask_[m.index]) return Added::Stale;
            gotMask_[m.index] = true; got_[m.index] = m; ++gotN_;
            if (gotN_ < pendingCount_) return Added::Pending;
        }
        list_ = std::move(got_); got_.clear(); gotMask_.clear();
        have_ = true; travel_ = pendingTravel_; seq_ = pendingSeq_; pending_ = false;
        next_ = 0; tries_ = 0; spawned_ = failed_ = 0; purged_ = false;   // a new list is carried out from the start
        return Added::Complete;
    }
    void Baseline() { baseline_ = true; }           // this machine's match began in this level (ObjectSync::MarkBaseline)

    // Wait: no list yet, or this machine's match has not begun here. Purge: destroy every loose weapon of this machine's
    // (once, before any of the host's is spawned). Spawn: spawn Current(). Done: the list is carried out.
    Step Next() const {
        if (!have_ || !baseline_) return Step::Wait;
        if (!purged_) return Step::Purge;
        return next_ < list_.size() ? Step::Spawn : Step::Done;
    }
    void Purged() { purged_ = true; }
    const LevelWeaponMsg& Current() const { return list_[next_ < list_.size() ? next_ : 0]; }   // Step::Spawn only
    size_t CurrentIndex() const { return next_; }
    // The spawn of Current() worked or failed: a failed one is tried kLevelWeaponSpawnTries times, then left out.
    // True when that was its last try (it is left out).
    bool Spawned(bool ok) {
        if (next_ >= list_.size()) return false;
        if (ok) { ++spawned_; ++next_; tries_ = 0; return false; }
        if (++tries_ < kLevelWeaponSpawnTries) return false;
        ++failed_; ++next_; tries_ = 0;
        return true;
    }
    bool HaveList() const { return have_; }
    const std::vector<LevelWeaponMsg>& List() const { return list_; }
    uint16_t Seq() const { return seq_; }
    size_t SpawnedCount() const { return spawned_; }
    size_t FailedCount() const { return failed_; }
    void Clear() { *this = LevelWeaponFollow{}; }   // a level change

private:
    bool have_ = false, baseline_ = false, purged_ = false;
    uint32_t travel_ = 0;
    uint16_t seq_ = 0;
    std::vector<LevelWeaponMsg> list_;
    size_t next_ = 0, spawned_ = 0, failed_ = 0;
    uint8_t tries_ = 0;
    bool pending_ = false;
    uint32_t pendingTravel_ = 0;
    uint16_t pendingSeq_ = 0, pendingCount_ = 0;
    std::vector<LevelWeaponMsg> got_;
    std::vector<bool> gotMask_;
    size_t gotN_ = 0;
};

}  // namespace hsm

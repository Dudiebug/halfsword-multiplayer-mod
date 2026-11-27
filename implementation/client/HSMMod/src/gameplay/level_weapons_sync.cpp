#include "gameplay/object_sync.h"
#include "log.h"
#include <string>

// The level's loose weapons, from the host (session/hsm_level_weapons.h, protocol rev 53, #4297). Each machine's level placed
// its own (a chest's contents, a random premade pick and a career's tier are rolled on each machine), and a weapon nobody has
// picked up is never announced, so a weapon one player saw lying there was not there for the others. When the host's match
// begins in a level (MarkBaseline) it saves the passport of each level weapon it recorded, a few a frame (a save to memory,
// as the Crypts loot does), and sends the list. A client keeps the list until its own match has begun in that level, then
// destroys every loose weapon of its own (no holder, never held: Adapter::LevelWeapons) and spawns the host's from their
// passports, a few a frame; those are then its level weapons (levelWeapons_), so a round reset puts the same weapons back on
// every machine. A client that loads the level late gets the list from the host with the census (LevelWeaponsForJoiner).
// Not covered: a weapon the level spawns after the match began, and a shared Crypts run (crypts_loot.cpp sends its loot).

namespace hsm::game {
using namespace hsm::ue;

namespace {
constexpr int kExportsPerTick = 2;        // passports saved a frame (each is a save to memory)
constexpr uint8_t kExportTries = 3;
constexpr int kSpawnsPerTick = 2;
constexpr size_t kLogWeapons = 40;        // per-weapon lines per list, each side

// The class path of an actor's class, without the "Class " its full name starts with.
std::string ClassPath(UObject* o) {
    const std::string full = FullName(reinterpret_cast<UObject*>(ClassOf(o)));
    const size_t sp = full.find(' ');
    return sp == std::string::npos ? full : full.substr(sp + 1);
}
std::string Leaf(const std::string& path) {
    const size_t dot = path.rfind('.');
    return dot == std::string::npos ? path : path.substr(dot + 1);
}
}  // namespace

bool ObjectSync::LevelWeaponsOn() const {
    if (!env_.travel || (env_.cryptsLoot && env_.cryptsLoot())) return false;
    return !env_.host || env_.levelWeaponsSync;
}

void ObjectSync::LevelWeaponsBaseline() {
    if (!LevelWeaponsOn()) return;
    if (!env_.host) { lwFollow_.Baseline(); return; }
    lwExporting_ = true; lwExport_ = lwLeftOut_ = 0; lwTries_ = 0; lwOut_.clear(); lwSent_.clear();
    lwTravel_ = env_.travel();
}

std::vector<std::vector<uint8_t>> ObjectSync::LevelWeaponsForJoiner(uint32_t travel) const {
    if (!env_.host || lwExporting_ || lwSent_.empty() || travel != lwTravel_) return {};
    return lwSent_;
}

void ObjectSync::OnLevelWeapons(const uint8_t* d, size_t n) {
    if (env_.host || !active_) return;
    LevelWeaponMsg m;
    if (!Decode(d, n, m)) return;
    if (!LevelWeaponsOn()) {
        if (++lwRefused_ <= 3) HSM_LOG("level weapons: the host's list #%u is not followed here (a shared Crypts run)", unsigned(m.seq));
        return;
    }
    const uint32_t travel = env_.travel();
    switch (lwFollow_.Add(m, travel)) {
        case LevelWeaponFollow::Added::Complete:
            HSM_LOG("level weapons: the host's list #%u is in (%zu weapon(s), travel #%u)%s", unsigned(m.seq), lwFollow_.List().size(), travel,
                    baselineMs_ >= 0 ? "" : "; held until the match begins here");
            break;
        case LevelWeaponFollow::Added::OtherTravel:
            if (++lwRefused_ <= 3) HSM_LOG("level weapons: the host's list #%u is for travel #%u, this level is travel #%u; dropped", unsigned(m.seq), m.travel, travel);
            break;
        case LevelWeaponFollow::Added::Rejected:
            if (++lwRefused_ <= 3) HSM_LOG("level weapons: a part of the host's list #%u contradicts it (count %u); dropped", unsigned(m.seq), unsigned(m.count));
            break;
        default: break;
    }
}

void ObjectSync::StepLevelWeapons() {
    if (env_.host) {
        if (!lwExporting_) return;
        for (int done = 0; done < kExportsPerTick && lwExport_ < levelWeapons_.size();) {
            const LevelWeapon& w = levelWeapons_[lwExport_];
            const char* skip = nullptr;
            std::string path;
            if (!adapter_->StillValid(w.actor, w.index)) skip = "it is gone";
            else if (ObjectField(w.actor, 0)) skip = "a fighter has it";
            else if (!crypt::ItemClassOk(path = ClassPath(w.actor), crypt::kItemWeapon)) skip = "its class path is not one the wire carries";
            if (skip) {
                ++lwLeftOut_;
                HSM_LOG("level weapons: %s left out of the list (%s)", path.empty() ? "a level weapon" : Leaf(path).c_str(), skip);
                ++lwExport_; lwTries_ = 0;
                continue;
            }
            ++done;
            std::vector<uint8_t> item;
            if (!adapter_->ExportItemState(w.actor, crypt::kItemWeapon, item)) {
                if (++lwTries_ < kExportTries) break;                           // again next frame
                ++lwLeftOut_;
                HSM_LOG("level weapons: %s at (%.0f,%.0f,%.0f) left out of the list (its passport could not be saved)", Leaf(path).c_str(), w.at.x, w.at.y, w.at.z);
                ++lwExport_; lwTries_ = 0;
                continue;
            }
            LevelWeaponMsg m;
            m.cls = path; m.item = std::move(item);
            m.x = float(w.at.x); m.y = float(w.at.y); m.z = float(w.at.z);
            m.pitch = float(w.rot.pitch); m.yaw = float(w.rot.yaw); m.roll = float(w.rot.roll);
            lwOut_.push_back(std::move(m));
            ++lwExport_; lwTries_ = 0;
        }
        if (lwExport_ < levelWeapons_.size()) return;
        lwExporting_ = false;
        if (++lwSeq_ == 0) lwSeq_ = 1;
        const std::vector<LevelWeaponMsg> parts = SplitLevelWeapons(lwTravel_, lwSeq_, lwOut_);
        if (parts.empty()) { HSM_LOG("level weapons: %zu loose weapon(s) is more than a list carries; not sent", lwOut_.size()); return; }
        lwSent_.clear();
        for (const LevelWeaponMsg& p : parts) lwSent_.push_back(Encode(p));
        for (const std::vector<uint8_t>& b : lwSent_) env_.send(b, true);
        HSM_LOG("level weapons: list #%u sent: %zu loose weapon(s) of the host's (%zu left out), travel #%u", unsigned(lwSeq_), lwOut_.size(), lwLeftOut_, lwTravel_);
        for (size_t i = 0; i < lwOut_.size() && i < kLogWeapons; ++i)
            HSM_LOG("level weapons: #%zu %s (%zu bytes of passport) at (%.0f,%.0f,%.0f)", i, Leaf(lwOut_[i].cls).c_str(), lwOut_[i].item.size(), lwOut_[i].x, lwOut_[i].y, lwOut_[i].z);
        return;
    }
    for (int spawned = 0; spawned < kSpawnsPerTick;) {
        const LevelWeaponFollow::Step step = lwFollow_.Next();
        if (step == LevelWeaponFollow::Step::Wait || step == LevelWeaponFollow::Step::Done) return;
        if (step == LevelWeaponFollow::Step::Purge) {
            lwPurged_ = 0;
            for (UObject* w : adapter_->LevelWeapons()) { adapter_->DestroyActor(w); ++lwPurged_; }
            lwSpawned_.clear();
            lwFollow_.Purged();
            HSM_LOG("level weapons: %zu loose weapon(s) of mine destroyed; spawning the host's %zu", lwPurged_, lwFollow_.List().size());
            continue;
        }
        ++spawned;
        const LevelWeaponMsg& m = lwFollow_.Current();
        const size_t i = lwFollow_.CurrentIndex();
        const FVector at{m.x, m.y, m.z};
        const FRotator rot{m.pitch, m.yaw, m.roll};
        UClass* cls = adapter_->LoadClassBlocking(m.cls);
        std::string why = cls ? "the spawn failed" : "its class could not be loaded";
        UObject* a = !cls ? nullptr : !m.item.empty() ? adapter_->SpawnItemFromState(m.item, crypt::kItemWeapon, cls, at, rot, why) : adapter_->SpawnActorAt(cls, at, rot);
        const bool ok = a != nullptr;
        if (ok) {
            LevelWeapon lw;
            lw.actor = a; lw.index = ObjectIndex(a); lw.classHash = ClassHashOf(a); lw.at = at; lw.rot = rot;
            lwSpawned_.push_back(lw);
            if (i < kLogWeapons) HSM_LOG("level weapons: #%zu %s (%zu bytes of passport) at (%.0f,%.0f,%.0f)", i, Leaf(m.cls).c_str(), m.item.size(), m.x, m.y, m.z);
        }
        if (lwFollow_.Spawned(ok)) HSM_LOG("level weapons: #%zu %s NOT spawned (%s)", i, Leaf(m.cls).c_str(), why.c_str());
        if (lwFollow_.Next() == LevelWeaponFollow::Step::Done) {
            levelWeapons_ = lwSpawned_;                 // a round reset puts these back, as the host puts back its own
            HSM_LOG("level weapons: list #%u carried out: %zu of mine destroyed, %zu of the host's spawned (%zu failed)", unsigned(lwFollow_.Seq()), lwPurged_,
                    lwFollow_.SpawnedCount(), lwFollow_.FailedCount());
            return;
        }
    }
}

}  // namespace hsm::game

// The recorder ([mod] record=1, config.h): every fight level to HSMRec-<date>-<time>.hsmrec next to HSMMod.log. At 60 Hz
// it writes every fighter's physics bodies, held weapons and grip, their vitals at 15 Hz, every physics object of the level
// (furniture, loose weapons and armour; every body) while it moves, the local player's input (axes, held buttons, camera),
// and the gameplay events: what the Willie's own fields and vitals show (grab, pick-up, drop, grip change, thrust, kick,
// parry, stuck blade, fall, death, damage, sever; EdgeEvents and VitalsEvents in shared/record/hsm_record.h), the contacts
// the hit hook sees (Hit), button presses (Input) and breakables shattering (Break). Single player and sessions alike;
// it changes nothing in the game.
// A writer thread does the disk work. Off (the default) it costs one branch per tick and starts nothing.
// tools/hsmrec.py reads the files; docs/PLAN_RECORDER.md has the format and what comes next.
#pragma once
#include "hsm_input.h"
#include "record/hsm_record.h"
#include "ue/engine.h"
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hsm::game {
class Adapter;
class ObjectSync;

class Recorder {
public:
    explicit Recorder(bool on);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Per GameMode tick, game thread. gameMode tells levels apart (a reload of the same map is a new recording);
    // fightLevel: record this one; level: its name; localWillie: the player's own fighter (flagged local, the rest bots).
    void Tick(Adapter& adapter, ue::UObject* gameMode, bool fightLevel, const std::string& level, ue::UObject* localWillie, float dt);
    // An event a hook saw (game thread). Ignored while no level is being recorded.
    void Note(const rec::Event& e);
    void Marker(const std::string& text);
    // A contact from the hit hook (game thread): striker hit struck, each with the fighter it belongs to (itself, the one
    // holding or wearing it, or null). strikerWeapon: the striker is a weapon (its prop id goes in the event). Kept at
    // most once per pair per kHitEveryMs (recorder.cpp): a blade resting on a body reports a contact every physics step.
    void Hit(ue::UObject* striker, ue::UObject* struck, ue::UObject* strikerFighter, ue::UObject* struckFighter,
             bool strikerWeapon, const ue::FVector& impulse);
    // The recording's id of a fighter it has seen, 0 for anything else.
    uint16_t FighterId(const ue::UObject* willie) const;
    // A prop's id in the recording (its object index), 0 for none.
    static uint64_t PropId(const ue::UObject* actor);
    bool Recording() const { return file_ != nullptr; }

private:
    struct Fighter;
    struct Prop;
    struct BodyCalls;
    void Open(const std::string& level, double nowMs);
    void Close(double nowMs);
    void Sample(Adapter& adapter, ue::UObject* localWillie, float dt, double nowMs);
    bool Bind(Fighter& f, Adapter& adapter, ue::UObject* willie, ue::UObject* localWillie);
    void ReadState(const Fighter& f, rec::FighterState& s) const;
    void ReadFrame(Fighter& f, float dt, double nowMs);
    void ScanProps(Adapter& adapter, double nowMs);
    void SampleProps(Adapter& adapter, double nowMs);
    BodyCalls* CallsFor(ue::UObject* comp);
    void Dedupe(Prop& p);
    static float Dist(const ue::FVector& a, const ue::FVector& b);
    void HookInput(Adapter& adapter, ue::UObject* localWillie);   // once: a hook on each of the game's input events
    void SampleInput(Adapter& adapter, ue::UObject* localWillie, double nowMs);
    void Push();   // hands batch_ to the writer thread
    void WriterLoop();

    const bool on_;
    ue::UObject* gameMode_ = nullptr;
    std::FILE* file_ = nullptr;
    std::string name_;   // the file's name, for the log
    double openedMs_ = 0, nextSampleMs_ = 0, nextLogMs_ = 0;
    std::vector<std::unique_ptr<Fighter>> fighters_;
    uint16_t nextId_ = 1;
    std::vector<std::unique_ptr<Prop>> props_;
    double nextPropScanMs_ = 0, nextWalkMs_ = 0;
    std::unique_ptr<ObjectSync> walker_;   // its level walk only (never started); new per file
    std::unordered_map<const ue::UClass*, std::unique_ptr<BodyCalls>> calls_;   // per primitive class; cleared per file
    // Input: the hooks stay for the process (installed once); they fill input_ while a file is open.
    bool inputHooked_ = false;
    ue::UObject* inputWillie_ = nullptr;
    InputCapture input_;
    std::map<std::pair<const ue::UObject*, const ue::UObject*>, double> lastHit_;   // per contact pair, lower pointer first
    std::vector<uint8_t> batch_;   // this sample's chunks (game thread)
    // Counters: since the file opened, and the 10 s log window.
    uint64_t samples_ = 0, frames_ = 0, propFrames_ = 0, inputs_ = 0, events_ = 0, queued_ = 0, dropped_ = 0;
    uint64_t winSamples_ = 0;
    double winCostMs_ = 0, winMaxMs_ = 0, winWalkMaxMs_ = 0;
    size_t queueMax_ = 0;
    // Writer thread: pending_ is the bytes it has not written yet.
    std::thread writer_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<uint8_t> pending_;
    bool stop_ = false, writeFailed_ = false;
};

}  // namespace hsm::game

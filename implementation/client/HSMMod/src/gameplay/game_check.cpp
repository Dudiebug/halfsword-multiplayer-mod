// The live game name check. The names and their classes come from generated/hsm_game_manifest.h (the build HSMMod was
// verified on); the verdicts from shared/session/hsm_game_check.h.
// A class's names are looked at when FindClass first finds it (the mod is about to use it) and in a sweep of the object
// array at the self-test and at each level load (classes the mod reaches through objects). Native functions are all
// there from the start, so the first sweep settles them. Lookups here are uncached (FindPropertyNow): a class caught
// while it is still loading must not leave a cached miss behind for the mod's own lookups.
#include "gameplay/game_check.h"
#include "generated/hsm_game_manifest.h"
#include "log.h"
#include "ue/engine.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hsm::game {
using namespace hsm::ue;

namespace {
using Ids = std::vector<size_t>;

std::mutex g_mu;
bool g_started = false;
std::vector<GameCheckEntry> g_entries;
GameCheckTally g_tally{0};
std::unordered_map<std::string, Ids> g_byOwner;    // lower-case class name -> its entries
std::unordered_map<std::string, Ids> g_byNative;   // lower-case native function name -> the function's entries and its parameters'
struct Seen { const Ids* owner = nullptr; const Ids* native = nullptr; };
std::unordered_map<uint64_t, Seen> g_seen;         // FName -> what it names here: each name is read once
uint32_t g_lastSweep = 0;
int g_tickSweeps = 0;                               // second-look sweeps since the last level load
int g_loggedOk = -1, g_loggedChanged = -1;

uint32_t Now() { return uint32_t(GetTickCount64()); }

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

void Note(size_t i, GameCheckLook look, uint32_t now, const std::string& type = {}) {
    if (g_tally.Record(i, look, now)) HSM_LOG("%s", GameCheckLogLine(g_entries[i], g_tally.State(i), type).c_str());
}

void LookAtClass(UClass* cls, const Ids& ids, uint32_t now) {
    for (size_t i : ids) {
        if (!g_tally.Open(i)) continue;
        const GameCheckEntry& e = g_entries[i];
        switch (e.kind) {
            case GameCheckKind::Property: {
                const FProperty* p = FindPropertyNow(cls, e.name);
                const std::string type = p ? PropertyType(p) : std::string();
                Note(i, GameCheckLookProperty(p != nullptr, e.type, type), now, type);
                break;
            }
            case GameCheckKind::Function:
                Note(i, FindFunctionNow(cls, e.name) ? GameCheckLook::Ok : GameCheckLook::Missing, now);
                break;
            case GameCheckKind::Param: {
                const UFunction* f = FindFunctionNow(cls, e.name);
                Note(i, GameCheckLookParam(f != nullptr, f && FindPropertyNow(f, e.member)), now);
                break;
            }
            case GameCheckKind::NativeFunction: break;
        }
    }
}

bool AnyOpen(const Ids& ids) {
    return std::any_of(ids.begin(), ids.end(), [](size_t i) { return g_tally.Open(i); });
}

void LogProgress() {
    if (g_tally.Ok() == g_loggedOk && g_tally.Changed() == g_loggedChanged) return;
    g_loggedOk = g_tally.Ok();
    g_loggedChanged = g_tally.Changed();
    HSM_LOG("game check: %d of %zu names found so far, %d changed since build %d", g_loggedOk, g_entries.size(), g_loggedChanged,
            manifest::kBuild);
}

// One pass over the object array (g_mu held): every class object the manifest names, and every function object with a
// native entry's name. A native entry is judged on the functions of that name the pass saw.
void Sweep(uint32_t now) {
    g_lastSweep = now;
    UObject* functionClass = FindObject("Function", "Class");
    UObject* classClasses[std::size(kClassKinds)] = {};
    for (size_t k = 0; k < std::size(kClassKinds); ++k) classClasses[k] = FindObject(kClassKinds[k], "Class");
    std::unordered_map<const Ids*, std::vector<const UObject*>> natives;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        const UObject* o = ObjectAt(i);
        if (!o) continue;
        const UObject* c = reinterpret_cast<const UObject*>(ClassOf(o));
        const bool isFunction = c && c == functionClass;
        const bool isClass = c && std::find(std::begin(classClasses), std::end(classClasses), c) != std::end(classClasses);
        if (!isFunction && !isClass) continue;
        const FName fn = ObjectFName(o);
        const uint64_t key = uint64_t(uint32_t(fn.index)) << 32 | uint32_t(fn.number);
        auto it = g_seen.find(key);
        if (it == g_seen.end()) {
            const std::string name = Lower(ObjectName(o));
            Seen s;
            if (auto a = g_byOwner.find(name); a != g_byOwner.end()) s.owner = &a->second;
            if (auto a = g_byNative.find(name); a != g_byNative.end()) s.native = &a->second;
            it = g_seen.emplace(key, s).first;
        }
        if (isClass && it->second.owner && AnyOpen(*it->second.owner))
            LookAtClass(reinterpret_cast<UClass*>(const_cast<UObject*>(o)), *it->second.owner, now);
        if (isFunction && it->second.native) natives[it->second.native].push_back(o);
    }
    for (const auto& [name, ids] : g_byNative) {
        if (!AnyOpen(ids)) continue;
        const auto at = natives.find(&ids);
        const std::vector<const UObject*> none;
        const std::vector<const UObject*>& fns = at == natives.end() ? none : at->second;
        for (size_t i : ids) {
            const GameCheckEntry& e = g_entries[i];
            if (e.kind == GameCheckKind::NativeFunction)
                Note(i, fns.empty() ? GameCheckLook::Missing : GameCheckLook::Ok, now);
            else if (e.kind == GameCheckKind::Param)
                Note(i, GameCheckLookParam(!fns.empty(), std::any_of(fns.begin(), fns.end(), [&](const UObject* f) { return FindPropertyNow(f, e.member) != nullptr; })), now);
        }
    }
    LogProgress();
}

void OnClassFound(const std::string& name, UClass* cls) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_started) return;
    if (auto it = g_byOwner.find(Lower(name)); it != g_byOwner.end() && AnyOpen(it->second)) LookAtClass(cls, it->second, Now());
}
}  // namespace

void StartGameCheck(bool testMiss) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_started) return;
    g_entries.assign(std::begin(manifest::kEntries), std::end(manifest::kEntries));
    if (testMiss) {
        g_entries.push_back({GameCheckKind::Property, "Willie_BP_C", "HSM Game Check Test", "", "", "game_check_test=1"});
        g_entries.push_back({GameCheckKind::NativeFunction, "", "HSMGameCheckTest", "", "", "game_check_test=1"});
    }
    g_tally = GameCheckTally(g_entries.size());
    for (size_t i = 0; i < g_entries.size(); ++i) {
        const GameCheckEntry& e = g_entries[i];
        if (*e.owner) g_byOwner[Lower(e.owner)].push_back(i);
        else g_byNative[Lower(e.name)].push_back(i);
    }
    g_started = true;
    HSM_LOG("game check: %zu names on %zu classes and %zu native functions, as of Half Sword build %d%s", g_entries.size(),
            g_byOwner.size(), g_byNative.size(), manifest::kBuild, testMiss ? " (game_check_test=1: two made-up names added)" : "");
    SetClassFoundHook(&OnClassFound);
    Sweep(Now());
}

void GameCheckSweep() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_started) return;
    g_tickSweeps = 0;
    Sweep(Now());
}

void GameCheckTick() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_started || !g_tally.Suspects() || g_tickSweeps >= 3) return;
    const uint32_t now = Now();
    if (uint32_t(now - g_lastSweep) < kGameCheckConfirmMs) return;
    ++g_tickSweeps;
    Sweep(now);
}

std::string GameCheckNotice(int steamBuild) {
    std::lock_guard<std::mutex> lock(g_mu);
    return hsm::GameCheckNotice(steamBuild, g_tally.Changed());
}

}  // namespace hsm::game

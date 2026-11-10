#include "gameplay/mod_rules.h"
#include "util/hsm_ini.h"
#include "match/hsm_online_rules.h"
#include <cstdio>
#include <ctime>
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include "enhancer.h"
#include "hse_hold.h"
#include "file_hash.h"
#include "gameplay/lobby.h"
#include "gameplay/mod_maps.h"
#include "log.h"
#include "net/update_check.h"
#include "paths.h"
#include "session/hsm_ue4ss.h"
#include "ue4ss_load.h"

namespace hsm::game::modrules {
namespace {
bool g_enabled = true, g_challenge = true;
ModRule g_chosen;   // from the INI; the list only for Custom with mod_allow= set
bool g_allowFromIni = false;

// ---- worker: the UE4SS mods and the proxies (files on disk) -------------------------------------------------------

struct Extras {
    std::mutex mu;
    std::vector<ModItem> items;
    bool ue4ss = false, done = false, running = false;
    uint32_t version = 0;
};
Extras g_extras;
std::atomic<bool> g_extrasWanted{false};

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L' ');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
// The first 8 MB of a file ("" when it can't be read): a proxy's bytes, HS Enhancer's settings.
std::string ReadFileHead(const std::wstring& path) {
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) { text.resize(8 * 1024 * 1024); text.resize(std::fread(text.data(), 1, text.size(), f)); std::fclose(f); }
    return text;
}

// The folder (with a trailing backslash) UE4SS.dll was loaded from, when it is loaded in this process.
std::wstring Ue4ssFolder() {
    HMODULE m = GetModuleHandleW(L"UE4SS.dll");
    if (!m) return {};
    wchar_t p[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : s.substr(0, k + 1);
}

// A UE4SS mod's hash: SHA-256 of Scripts/main.lua, or of dlls/main.dll, or (both there) SHA-256 of the two digests.
bool HashUe4ssMod(const std::wstring& dir, Sha256Digest& out) {
    Sha256Digest lua{}, dll{};
    const bool hasLua = FileExists(dir + L"Scripts\\main.lua") && HashFile(dir + L"Scripts\\main.lua", lua);
    const bool hasDll = FileExists(dir + L"dlls\\main.dll") && HashFile(dir + L"dlls\\main.dll", dll);
    if (hasLua && hasDll) {
        uint8_t both[64];
        std::memcpy(both, lua.data(), 32); std::memcpy(both + 32, dll.data(), 32);
        out = Sha256Of(both, sizeof both);
        return true;
    }
    if (hasLua) { out = lua; return true; }
    if (hasDll) { out = dll; return true; }
    return false;
}

void ScanExtras() {
    std::vector<ModItem> items;
    const std::wstring folder = Ue4ssFolder();
    const bool ue4ss = !folder.empty();
    if (ue4ss) {
        const std::wstring mods = folder + L"Mods\\";
        for (const std::string& name : ue4ss::EnabledModNames(mods)) {
            ModItem it;
            it.name = SanitizeModItemName(name);
            if (ue4ss::IsStockMod(name)) { it.kind = ModKind::Ue4ssStock; items.push_back(it); continue; }
            it.kind = ModKind::Ue4ssMod;
            it.hashed = HashUe4ssMod(mods + Wide(name) + L"\\", it.sha);
            items.push_back(it);
        }
    }
    const std::wstring exeDir = ModFilePath(L"");
    for (const char* n : {"version.dll", "dsound.dll", "winmm.dll", "xinput1_3.dll", "d3d11.dll", "dxgi.dll"}) {
        const std::wstring p = exeDir + Wide(n);
        if (!FileExists(p)) continue;
        ModItem it; it.kind = ModKind::Proxy; it.name = n;
        it.hashed = HashFile(p, it.sha);
        // HS Enhancer's own loader: HS Enhancer, which a lobby that refuses it locks rather than sets aside
        if (LooksLikeHseLoader(ReadFileHead(p))) { it.kind = ModKind::Module; it.name = kEnhancerModName; }
        items.push_back(it);
    }
    std::lock_guard<std::mutex> lock(g_extras.mu);
    g_extras.items = std::move(items);
    g_extras.ue4ss = ue4ss;
    g_extras.done = true;
    ++g_extras.version;
}

void StartExtras() {
    {
        std::lock_guard<std::mutex> lock(g_extras.mu);
        if (g_extras.running) { g_extrasWanted = true; return; }
        g_extras.running = true;
        g_extras.done = false;
    }
    std::thread([] {
        do { g_extrasWanted = false; ScanExtras(); } while (g_extrasWanted.load());
        std::lock_guard<std::mutex> lock(g_extras.mu);
        g_extras.running = false;
    }).detach();
}

// ---- game thread ----------------------------------------------------------------------------------------------------

struct Local {
    bool started = false;
    bool ue4ssSeen = false;
    bool scanDone = false;                       // the pak scan was listed and hashed at the last extras start
    uint32_t scanAtExtras = 0;                   // modmaps::ScanVersion() then
    uint32_t mapsVersion = ~0u, extrasVersion = ~0u;
    bool enhancer = false, hseLocked = false;
    ModReport report;
    uint32_t version = 0;
    std::string logged;
};
Local g;

void Rebuild() {
    ModReport r;
    const std::vector<ModPak>& paks = modmaps::Paks();
    const std::vector<ModKind>& kinds = modmaps::PakKinds();
    for (size_t i = 0; i < paks.size(); ++i) {
        ModItem it;
        it.kind = i < kinds.size() ? kinds[i] : ModKind::ContentPak;
        it.name = SanitizeModItemName(paks[i].name);
        it.hashed = paks[i].hashed; it.sha = paks[i].sha;
        r.items.push_back(it);
    }
    bool extrasDone = false;
    {
        std::lock_guard<std::mutex> lock(g_extras.mu);
        for (const ModItem& it : g_extras.items) r.items.push_back(it);
        r.ue4ss = g_extras.ue4ss;
        extrasDone = g_extras.done && !g_extras.running;
    }
    if (g.enhancer && !HasHse(r)) { ModItem it; it.kind = ModKind::Module; it.name = kEnhancerModName; r.items.push_back(it); }
    r.complete = modmaps::Listed() && modmaps::AllHashed() && extrasDone;
    NormalizeReport(r);
    if (g.hseLocked) LockReportHse(r);
    if (r == g.report) return;
    g.report = std::move(r);
    ++g.version;
    if (!g.report.complete) return;
    const std::string line = ReportLogLine(g.report);
    if (line != g.logged) { g.logged = line; HSM_LOG("modrules: my mods: %s", line.c_str()); }
}
}  // namespace

namespace {
Config g_cfg;                 // the rule keys as last read
FILETIME g_iniTime{};
ULONGLONG g_nextIniCheck = 0;

bool IniWriteTime(FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(ModFilePath(L"HSMMod.ini").c_str(), GetFileExInfoStandard, &a)) return false;
    out = a.ftLastWriteTime;
    return true;
}

void ApplyRule(const Config& cfg) {
    ModPreset p = ModChoice{}.preset;
    if (!ParseModPreset(cfg.modRule, p)) HSM_LOG("modrules: mod_rule=%s is not vanilla, maps, bundle, custom, anything or classes; using vanilla", cfg.modRule.c_str());
    g_chosen = ModRule{};
    g_chosen.preset = p;
    g_chosen.blockHse = cfg.blockHse;
    g_chosen.officialOnly = cfg.modOfficialOnly;
    g_allowFromIni = !cfg.modAllow.empty();
    if (g_allowFromIni) g_chosen.list = ParseAllowList(cfg.modAllow);
}

// The host edited the rule keys while the game runs: the next ModRulesTick (gameplay/session.cpp) announces the change.
void CheckIni() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextIniCheck) return;
    g_nextIniCheck = now + 2000;
    FILETIME t;
    if (!IniWriteTime(t) || CompareFileTime(&t, &g_iniTime) == 0) return;
    g_iniTime = t;
    Config c = g_cfg;
    ReadModRuleKeys(c);
    if (c.modRule == g_cfg.modRule && c.blockHse == g_cfg.blockHse && c.modOfficialOnly == g_cfg.modOfficialOnly && c.modAllow == g_cfg.modAllow) return;
    g_cfg = c;
    ApplyRule(c);
    HSM_LOG("modrules: HSMMod.ini changed; my lobbies: %s", RuleSummary(g_chosen).c_str());
}
std::string ReadTextFile(const std::wstring& path) { return ReadFileHead(path); }
}  // namespace

// A class's Half Wolverine parts go into its file now, before UE4SS starts the mod, because it reads the file only once. The player's
// own settings stay in a copy, and come back when they pick a class that doesn't use the mod.
static void EnableUe4ssHotReload(const Config& cfg) {
    // Off in a stock UE4SS. It's read when UE4SS starts, which is after this.
    if (!cfg.wolverineRefresh) return;
    const std::wstring path = ModFilePath(L"ue4ss\\UE4SS-settings.ini");
    const std::string text = ReadTextFile(path);
    if (text.empty()) return;
    const std::string want = util::SetIniValue(text, "General", "EnableHotReloadSystem", "1");
    if (want == text) return;
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        const bool ok = std::fwrite(want.data(), 1, want.size(), f) == want.size();
        std::fclose(f);
        HSM_LOG("classes: UE4SS's hot reload %s, so Half Wolverine can pick up a class without a restart", ok ? "turned on" : "could not be turned on");
    }
}
void ApplyWolverineClass(const Config& cfg) {
    if (!cfg.classWolverine) return;
    const std::wstring path = ModFilePath(L"ue4ss\\Mods\\HalfWolverine\\config.ini"), keep = ModFilePath(L"HSMMod-wolverine-own.ini");
    const std::string text = ReadTextFile(path);
    if (text.empty()) return;
    const std::optional<ClassSheet> c = ResolveClass(cfg.className, cfg.classCustom);
    const auto write = [](const std::wstring& to, const std::string& what) {
        FILE* f = _wfopen(to.c_str(), L"wb");
        if (!f) return false;
        const bool ok = std::fwrite(what.data(), 1, what.size(), f) == what.size();
        std::fclose(f);
        return ok;
    };
    const std::string own = ReadTextFile(keep);
    if (c && ClassNeedsWolverine(*c)) {
        const std::string want = WolverineFileFor(text, *c);
        if (want == text) return;
        if (own.empty() && !write(keep, text)) { HSM_LOG("classes: could not keep your Half Wolverine settings, so they were left alone"); return; }
        HSM_LOG("classes: Half Wolverine set for %s: %s", cfg.className.c_str(), write(path, want) ? "ok" : "could not write its file");
    } else if (!own.empty()) {
        const bool back = write(path, own);
        HSM_LOG("classes: your Half Wolverine settings: %s", back ? "put back" : "could not be put back");
        if (back) DeleteFileW(keep.c_str());
    }
}

void ApplyWolverineFile(const Config& cfg) { ApplyWolverineClass(cfg); }

namespace {
// "next": turned off for the next game. "now": this is that game.
wchar_t g_modsKeep[MAX_PATH * 2] = {}, g_modsTxt[MAX_PATH * 2] = {}, g_modsState[MAX_PATH * 2] = {};
bool g_modsOffThisGame = false;
constexpr int kMaxSetAside = 64;   // DLLs beside the exe and paks under Content/Paks, together
wchar_t g_asideFrom[kMaxSetAside][MAX_PATH * 2] = {}, g_asideTo[kMaxSetAside][MAX_PATH * 2] = {};   // ready for the exit, which can't build strings
int g_asideCount = 0;
wchar_t g_asideList[MAX_PATH * 2] = {};
std::wstring AsideListPath() { return ModFilePath(L"HSMMod-files-off.txt"); }
std::wstring WideName(const std::string& s) { wchar_t w[MAX_PATH]; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, MAX_PATH); return n > 0 ? std::wstring(w) : std::wstring(); }
std::wstring SwitchFilePath() { return ModFilePath(L"HSMMod-switch.txt"); }
std::wstring RejoinFilePath() { return ModFilePath(L"HSMMod-rejoin.txt"); }
std::wstring PaksRoot() { return ModFilePath(L"..\\..\\Content\\Paks\\"); }   // as gameplay/mod_maps.cpp lists them; ".." is fine for a rename
// Where an aside-list entry lives (session/hsm_mod_switch.h AsideEntry): a pak under Content/Paks, or a DLL beside the exe. Empty: unusable.
std::wstring AsidePath(const AsideEntry& e) {
    std::wstring w = WideName(e.name);
    if (w.empty()) return {};
    if (!e.pak) return ModFilePath(w.c_str());
    for (wchar_t& c : w) if (c == L'/') c = L'\\';
    return PaksRoot() + w;
}
std::vector<std::string> AsideList() {
    std::vector<std::string> out;
    const std::string text = ReadTextFile(AsideListPath());
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) out.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}
void PutFilesBack() {
    for (const std::string& line : AsideList()) {
        AsideEntry e;
        if (!ParseAsideLine(line, e)) continue;   // a bare name beside the exe or a pak under Content/Paks, nothing else
        const std::wstring to = AsidePath(e);
        if (to.empty()) continue;
        const bool back = MoveFileExW((to + L".off").c_str(), to.c_str(), 0);
        HSM_LOG("modrules: %s: %s", e.name.c_str(), back ? "put back" : "could not be put back; it's still there as .off");
    }
    DeleteFileW(AsideListPath().c_str());
}
std::wstring ModsTxtPath() { return ModFilePath(L"ue4ss\\Mods\\mods.txt"); }
std::wstring ModsKeepPath() { return ModFilePath(L"HSMMod-mods-own.txt"); }
std::wstring ModsStatePath() { return ModFilePath(L"HSMMod-mods-off.state"); }
bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}
void PutModsTxtBack() {
    const std::string keep = ReadTextFile(ModsKeepPath());
    const bool back = !keep.empty() && WriteFileBytes(ModsTxtPath(), keep);
    HSM_LOG("modrules: your mods.txt: %s", back ? "put back" : "could not be put back; your own copy is HSMMod-mods-own.txt");
    if (back) DeleteFileW(ModsKeepPath().c_str());
    PutFilesBack();
    DeleteFileW(ModsStatePath().c_str());
}
void ModsTxtAtStart() {
    const std::string state = ReadTextFile(ModsStatePath());
    if (state == "next") {
        WriteFileBytes(ModsStatePath(), "now");
        wcsncpy(g_modsKeep, ModsKeepPath().c_str(), MAX_PATH * 2 - 1);
        wcsncpy(g_modsTxt, ModsTxtPath().c_str(), MAX_PATH * 2 - 1);
        wcsncpy(g_modsState, ModsStatePath().c_str(), MAX_PATH * 2 - 1);
        wcsncpy(g_asideList, AsideListPath().c_str(), MAX_PATH * 2 - 1);
        g_modsOffThisGame = true;
        for (const std::string& line : AsideList()) {
            AsideEntry e;
            if (g_asideCount >= kMaxSetAside || !ParseAsideLine(line, e)) continue;
            const std::wstring to = AsidePath(e);
            if (to.empty()) continue;
            wcsncpy(g_asideTo[g_asideCount], to.c_str(), MAX_PATH * 2 - 1);
            wcsncpy(g_asideFrom[g_asideCount], (to + L".off").c_str(), MAX_PATH * 2 - 1);
            ++g_asideCount;
        }
        HSM_LOG("modrules: some mods are off for this game (%d file(s) set aside); everything goes back when it closes", g_asideCount);
    }
    else if (state == "now") PutModsTxtBack();   // the last game didn't close cleanly
}

// ---- HS Enhancer, off at the start of every game, or locked for one ------------------------------------------------
// HSE reads %APPDATA%\Half Sword Enhancer\config.ini once, when its loader starts it after the process has loaded, so
// DllMain settles the file before then. Every game: HSE's features off, its keys as the player set them
// (match/hsm_online_rules.h HseStartOffIni), so a lobby that blocks HSE needs only its keys held (hse_hold.h) and no
// restart. The player turns on what they want again where HSE is allowed. A game the switch file asks to lock it (no
// key hold, or features on at the join) locks it whole instead (LockedHseIni): the player's own file is copied first,
// to HSMMod-hse-own.ini, and goes back when that game closes. The copy stays until the next start, which puts it back
// again when the file still comes from the lock (a crash, or HSE writing its file on the way out). An empty copy: there
// was no file, so putting it back deletes it. A locked game that restarts for another lobby puts the file back at once
// (TurnOffForNextGame), and the next game locks it again.
wchar_t g_hseIni[MAX_PATH * 2] = {}, g_hseOwn[MAX_PATH * 2] = {};   // ready for the exit, which can't build strings
bool g_hseRestoreAtExit = false, g_hseOwnEmpty = false;
bool g_hseLockedThisGame = false, g_hseUnlocked = false;
bool g_hseStartOff = false;                  // this game started with HSE's features off (or with no file: all off by default)
bool g_hseOn = false, g_hseHeldOn = false;   // a feature is on now; one came on while its keys were held (until the game closes)
bool g_hseHoldWanted = false;
unsigned g_hseTurnedOff = 0;                 // features the start turned off, for the log
int g_hseMisses = 0;
ULONGLONG g_hseNextCheck = 0;
enum class HseStart : uint8_t { Nothing, Off, OffFailed, Locked, LockFailed, PutBack, PutBackFailed, Dropped };
HseStart g_hseStart = HseStart::Nothing, g_hseCopy = HseStart::Nothing;   // what DllMain did, for Configure to log
std::wstring HseIniPath() {
    wchar_t appData[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appData) + L"\\Half Sword Enhancer\\config.ini";
}
std::wstring HseOwnPath() { return ModFilePath(L"HSMMod-hse-own.ini"); }
bool FileSizeOf(const std::wstring& path, ULONGLONG& out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    out = (ULONGLONG(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}
bool PutHseBack(const std::wstring& ini, const std::wstring& own) {
    ULONGLONG size = 0;
    if (!FileSizeOf(own, size)) return false;
    if (size == 0) return DeleteFileW(ini.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    return CopyFileW(own.c_str(), ini.c_str(), FALSE) != 0;
}
// DllMain: file calls only, no log. `lock`: the switch file asks for it (the lobby this game restarted for refuses an
// unlocked HS Enhancer). Otherwise a copy left by an earlier locked game is settled first, then the features go off.
void SettleHseEarly(bool lock) {
    const std::wstring ini = HseIniPath(), own = HseOwnPath();
    if (ini.empty()) { g_hseStart = lock ? HseStart::LockFailed : HseStart::OffFailed; return; }
    ULONGLONG iniSize = 0, ownSize = 0;
    bool iniThere = FileSizeOf(ini, iniSize);
    const bool ownThere = FileSizeOf(own, ownSize);
    std::string text = iniThere ? ReadFileHead(ini) : std::string();
    const bool fromLock = iniThere && HseIniFromLock(text);
    if (!lock) {
        if (ownThere) {
            if (fromLock) g_hseCopy = PutHseBack(ini, own) ? HseStart::PutBack : HseStart::PutBackFailed;
            else g_hseCopy = HseStart::Dropped;   // the player's own file is there already
            if (g_hseCopy != HseStart::PutBackFailed) DeleteFileW(own.c_str());
            iniThere = FileSizeOf(ini, iniSize);
            text = iniThere ? ReadFileHead(ini) : std::string();
        }
        // No file: HSE starts with every feature off (its defaults), and nothing is written for a player without HSE.
        // A file too big to read whole is left alone, and so is one that can't be written: HSE starts as it is.
        if (!iniThere) { g_hseStartOff = true; g_hseStart = HseStart::Off; return; }
        g_hseTurnedOff = unsigned(HseStartOffProblems(text).size());
        const std::string off = HseStartOffIni(text);
        if (iniSize > text.size() || !HseStartOffProblems(off).empty() || (off != text && !WriteFileBytes(ini, off))) {
            g_hseStart = HseStart::OffFailed;
            return;
        }
        g_hseStartOff = true;
        g_hseStart = HseStart::Off;
        return;
    }
    // A file too big to read whole would be locked cut short, and one the lock can't make sense of stays unlocked: both
    // are left alone, and HS Enhancer is reported as it is.
    const std::string locked = LockedHseIni(text);
    if ((iniThere && iniSize > text.size()) || !HseLockProblems(locked).empty()) { g_hseStart = HseStart::LockFailed; return; }
    // Still from the lock (the game before this one locked it too, or crashed): its copy is the player's file. Otherwise
    // the player's file is copied now, byte for byte.
    if (!(ownThere && fromLock)) {
        const bool copied = iniThere ? CopyFileW(ini.c_str(), own.c_str(), FALSE) != 0 : WriteFileBytes(own, std::string());
        if (!copied || !FileSizeOf(own, ownSize)) { g_hseStart = HseStart::LockFailed; return; }
    }
    CreateDirectoryW(ini.substr(0, ini.find_last_of(L'\\')).c_str(), nullptr);   // a fresh HSE has none until it first saves
    if (!WriteFileBytes(ini, locked)) { PutHseBack(ini, own); g_hseStart = HseStart::LockFailed; return; }
    wcsncpy(g_hseIni, ini.c_str(), MAX_PATH * 2 - 1);
    wcsncpy(g_hseOwn, own.c_str(), MAX_PATH * 2 - 1);
    g_hseOwnEmpty = ownSize == 0;
    g_hseRestoreAtExit = g_hseLockedThisGame = true;
    g_hseStart = HseStart::Locked;
    enhancer::hold::CloseAtStart(int(kRuleLockedKeyCode));   // its menu opens at the start, and no key closes it now
}
void LogHseStart() {
    switch (g_hseCopy) {
        case HseStart::PutBack: HSM_LOG("modrules: your HS Enhancer settings: put back after the last locked game"); break;
        case HseStart::PutBackFailed: HSM_LOG("modrules: your HS Enhancer settings could not be put back; your own copy is HSMMod-hse-own.ini"); break;
        case HseStart::Dropped: HSM_LOG("modrules: HS Enhancer's settings are your own already; the copy from the last locked game is gone"); break;
        default: break;
    }
    switch (g_hseStart) {
        case HseStart::Off:
            if (g_hseTurnedOff) HSM_LOG("modrules: HS Enhancer starts with its features off (%u turned off), its keys as you set them", g_hseTurnedOff);
            break;
        case HseStart::OffFailed: HSM_LOG("modrules: could not turn HS Enhancer's features off for this game (its settings file could not be read or written): lobbies that block it need a restart"); break;
        case HseStart::Locked: HSM_LOG("modrules: HS Enhancer is locked for this game: its menu, hotkeys and features are off; your own settings come back when the game closes"); break;
        case HseStart::LockFailed: HSM_LOG("modrules: could not lock HS Enhancer for this game (its settings file could not be read, copied or written): it is reported as it is"); break;
        default: break;
    }
}
bool LockedStill() { return g_hseLockedThisGame && !g_hseUnlocked; }
// Every 5 s, and once at a join: HSE's file read again. Locked: two reads in a row that aren't (one could catch HSE
// mid-write) and it counts as unlocked until the game closes. Off at the start: a feature turned on counts until it is
// off again; one turned on while HSE's keys were held (only its menu, left open, could do that) counts until the game
// closes. The report then names HS Enhancer, and the host judges it.
void CheckHseLock() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_hseNextCheck) return;
    g_hseNextCheck = now + 5000;
    if (LockedStill()) {
        const std::vector<std::string> problems = HseLockProblems(ReadFileHead(HseIniPath()));
        if (problems.empty()) { g_hseMisses = 0; return; }
        if (++g_hseMisses < 2) return;
        g_hseUnlocked = true;
        HSM_LOG("modrules: HS Enhancer's settings changed during the game (%s): it counts as unlocked until the game closes", problems[0].c_str());
        return;
    }
    if (!g_hseStartOff || g_hseHeldOn || !enhancer::Detected()) return;
    const std::vector<std::string> problems = HseStartOffProblems(ReadFileHead(HseIniPath()));
    const bool on = !problems.empty();
    if (on && enhancer::hold::On()) {
        g_hseHeldOn = true;
        HSM_LOG("modrules: HS Enhancer turned on while its keys were held (%s): it counts as on until the game closes", problems[0].c_str());
    } else if (on != g_hseOn) {
        if (on) HSM_LOG("modrules: HS Enhancer: %s; a lobby that blocks it needs a restart to lock it, or turn it off again", problems[0].c_str());
        else HSM_LOG("modrules: HS Enhancer's features are all off again: lobbies that block it hold its keys, no restart");
    }
    g_hseOn = on;
}
bool OffStill() { return g_hseStartOff && !g_hseOn && !g_hseHeldOn; }

// The report from before mods went off for a lobby (session/hsm_mod_switch.h PlanBack): HSMMod-mods-all.dat, a ModList
// message, written by the game that restarts and kept across chained restarts while any mod is off or HS Enhancer is
// locked for a lobby. A game with everything on deletes it. g_fullSet: this game's is settled (read at the start of a
// game with mods off, or taken from the report at its first restart), so a second restart doesn't take a smaller one.
ModReport g_full;
bool g_fullKnown = false, g_fullSet = false;
std::wstring FullReportPath() { return ModFilePath(L"HSMMod-mods-all.dat"); }
void LoadFullReport() {
    const std::wstring path = FullReportPath();
    if (!g_modsOffThisGame && !g_hseLockedThisGame) { DeleteFileW(path.c_str()); return; }
    g_fullSet = true;
    const std::string bytes = ReadTextFile(path);
    ModListMsg m;
    g_fullKnown = !bytes.empty() && Decode(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), m) && m.report.complete;
    if (g_fullKnown) g_full = m.report;
    HSM_LOG("modrules: your mods from before the restart: %s", g_fullKnown ? "known (a lobby that takes them all offers them back)" : "not known (no lobby offers them back)");
}
void SaveFullReport() {
    if (!g_fullSet) { g_full = g.report; g_fullKnown = g.report.complete; g_fullSet = true; }
    bool saved = false;
    if (g_fullKnown) {
        ModListMsg m; m.report = g_full;
        const std::vector<uint8_t> b = Encode(m);
        saved = WriteFileBytes(FullReportPath(), std::string(b.begin(), b.end()));
    }
    if (!saved) { DeleteFileW(FullReportPath().c_str()); HSM_LOG("modrules: your mods from before could not be kept: no lobby offers them back"); }
}
}  // namespace

const ModReport* OwnBeforeSwitch() { return (g_modsOffThisGame || LockedStill()) && g_fullKnown ? &g_full : nullptr; }
bool HseLockedForLobby() { return LockedStill(); }
bool HseKeysHoldable() { return enhancer::hold::Ready(); }

bool HseLocked() { return LockedStill() || (OffStill() && enhancer::hold::On()); }
bool HseHoldable() { return LockedStill() || (OffStill() && enhancer::hold::Ready()); }

const ModReport& OwnAsGuest() {
    static ModReport locked;
    static uint32_t lockedAt = ~0u;
    if (!HseHoldable() || !HasHse(g.report)) return g.report;
    if (lockedAt != g.version) { locked = g.report; LockReportHse(locked); lockedAt = g.version; }
    return locked;
}

void HoldTick(const Lobby& lobby) {
    const bool wanted = !lobby.IsHost() && (lobby.Joining() || (lobby.InLobby() && RuleRefusesHse(lobby.RoomRule())));
    if (wanted == g_hseHoldWanted) return;
    g_hseHoldWanted = wanted;
    if (wanted) {
        g_hseNextCheck = 0;
        CheckHseLock();   // the file read again before the report says locked
        // HSE's menu key as HSE has it (read at its start; a new one is saved at once): the hold lets it through.
        enhancer::hold::MenuKey(HseMenuKey(ReadFileHead(HseIniPath())));
    }
    enhancer::hold::Set(wanted);
    if (!enhancer::Detected()) return;
    if (!wanted) HSM_LOG("modrules: HS Enhancer's keys are free again");
    else if (HseHoldable()) HSM_LOG("modrules: HS Enhancer's keys are held: this lobby blocks it");
    else HSM_LOG("modrules: this lobby blocks HS Enhancer, and its keys can't be held this game (%s): the host refuses it",
                 g_hseOn ? "a feature is on" : !g_hseStartOff ? "its features weren't turned off at the start" : "HSMMod isn't in its window hook");
}

bool TurnOffForNextGame(const std::vector<std::string>& mods, const std::vector<std::string>& dllsBesideExe, const std::vector<std::string>& paks, bool lockHse) {
    const std::string text = ReadTextFile(ModsTxtPath());
    if (!mods.empty()) {
        if (ReadTextFile(ModsKeepPath()).empty() && !text.empty() && !WriteFileBytes(ModsKeepPath(), text)) return false;
        if (!WriteFileBytes(ModsTxtPath(), ModsTxtWithOff(text, mods))) return false;
    }
    // A loaded DLL can still be renamed on Windows: this game keeps it, and the next one doesn't find it.
    std::string list = ReadTextFile(AsideListPath());
    unsigned dlls = 0;
    for (const std::string& name : dllsBesideExe) {
        AsideEntry e; e.name = name;
        if (!ParseAsideLine(name, e) || e.pak) continue;
        const std::wstring to = AsidePath(e);
        if (!to.empty() && MoveFileExW(to.c_str(), (to + L".off").c_str(), 0)) { list += AsideLine(e); ++dlls; }
        else HSM_LOG("modrules: could not set aside %s (error %lu)", name.c_str(), GetLastError());
    }
    if (!list.empty() && !WriteFileBytes(AsideListPath(), list)) return false;
    // A pak the engine has open can't be renamed from here: the next game does it before its engine starts (ApplySwitchEarly).
    SwitchFile sw; sw.pid = GetCurrentProcessId();
    for (const std::string& name : paks) {
        const std::string rel = modmaps::PakRelPath(name);
        if (PakRelOk(rel)) sw.paks.push_back(rel);
        else HSM_LOG("modrules: no pak file found for %s: it stays on", name.c_str());
    }
    sw.lockHse = lockHse || g_hseLockedThisGame;   // a locked game hands the lock on, like the mods it has off
    SaveFullReport();   // before anything goes off, or as the game before had it: what a lobby that takes it all brings back
    if ((!sw.paks.empty() || sw.lockHse) && !WriteFileBytes(SwitchFilePath(), EncodeSwitchFile(sw))) return false;
    if (!WriteFileBytes(ModsStatePath(), "next")) return false;
    // Already the game with mods off: the next one takes the whole set over (state "next", one aside list), so this
    // one's exit must put nothing back. HSE's file goes back now instead (this game's HSE read it long ago), and the next
    // game locks it again from the player's own file, after this one has closed.
    g_modsOffThisGame = false; g_asideCount = 0;
    if (g_hseRestoreAtExit) { PutHseBack(g_hseIni, g_hseOwn); g_hseRestoreAtExit = false; }
    HSM_LOG("modrules: for the next game: %u UE4SS mod(s) off in mods.txt, %u DLL(s) set aside, %u pak(s) to set aside%s", unsigned(mods.size()), dlls,
            unsigned(sw.paks.size()), sw.lockHse ? ", HS Enhancer locked" : "");
    return true;
}
bool ModsBackForNextGame(bool lockHse) {
    // Mods off for the next game already (a restart that didn't happen): this game's exit puts nothing back.
    if (!g_modsOffThisGame && ReadTextFile(ModsStatePath()) == "next") return false;
    // This game's exit puts every mod back (RestoreModsTxt) and HS Enhancer's own settings: the new game waits for it to
    // close before it reads a file (ApplySwitchEarly), then locks HSE again only when asked.
    SwitchFile sw; sw.pid = GetCurrentProcessId(); sw.lockHse = lockHse;
    if (!WriteFileBytes(SwitchFilePath(), EncodeSwitchFile(sw))) return false;
    HSM_LOG("modrules: for the next game: every mod back%s", lockHse ? ", HS Enhancer locked again" : ", HS Enhancer as you set it");
    return true;
}
// The process is exiting (DllMain): plain Windows calls on paths kept ready, and no logging. A crash skips this, and the next start puts it back.
void RestoreModsTxt() {
    if (g_hseRestoreAtExit) { if (g_hseOwnEmpty) DeleteFileW(g_hseIni); else CopyFileW(g_hseOwn, g_hseIni, FALSE); }
    if (!g_modsOffThisGame) return;
    for (int i = 0; i < g_asideCount; ++i) MoveFileExW(g_asideFrom[i], g_asideTo[i], 0);
    if (g_asideCount) DeleteFileW(g_asideList);
    if (g_modsKeep[0]) MoveFileExW(g_modsKeep, g_modsTxt, MOVEFILE_REPLACE_EXISTING);
    DeleteFileW(g_modsState);
}
// DllMain: file calls only, and no log (it opens later; ModsTxtAtStart logs what was set aside).
void ApplySwitchEarly() {
    const std::wstring path = SwitchFilePath();
    const std::string text = ReadTextFile(path);
    SwitchFile sw;
    const bool ok = !text.empty() && ParseSwitchFile(text, sw);
    if (!text.empty()) DeleteFileW(path.c_str());   // once, whatever happens next: no loop
    if (ok && sw.pid && sw.pid != GetCurrentProcessId())
        if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, sw.pid)) { WaitForSingleObject(h, 20000); CloseHandle(h); }
    SettleHseEarly(ok && sw.lockHse);   // every start: a copy left by a locked game is settled too
    if (!ok) return;
    std::string list = ReadTextFile(AsideListPath());
    for (const std::string& rel : sw.paks) {
        AsideEntry e; e.pak = true; e.name = rel;
        const std::wstring to = AsidePath(e);
        if (!to.empty() && MoveFileExW(to.c_str(), (to + L".off").c_str(), 0)) list += AsideLine(e);
    }
    if (!list.empty()) WriteFileBytes(AsideListPath(), list);
}

bool WriteRejoin(const RejoinFile& f) { return WriteFileBytes(RejoinFilePath(), EncodeRejoinFile(f)); }
bool TakeRejoin(RejoinFile& out) {
    const std::string text = ReadTextFile(RejoinFilePath());
    if (text.empty()) return false;
    DeleteFileW(RejoinFilePath().c_str());
    const bool ok = ParseRejoinFile(text, uint64_t(std::time(nullptr)), out);
    HSM_LOG("modrules: rejoin file: %s", ok ? "joining the lobby again after the restart" : "past its deadline or unreadable, ignored");
    return ok;
}

void Configure(const Config& cfg) {
    ModsTxtAtStart();
    LogHseStart();
    LoadFullReport();
    ApplyWolverineClass(cfg);
    EnableUe4ssHotReload(cfg);
    g_enabled = cfg.modRules;
    g_challenge = cfg.modChallenge;
    g_cfg = cfg;
    IniWriteTime(g_iniTime);
    ApplyRule(cfg);
    HSM_LOG("modrules: %s; my lobbies: %s%s", g_enabled ? "on" : "off (mod_rules=0)", RuleSummary(g_chosen).c_str(),
            g_enabled && !g_challenge ? "; code challenges off (mod_challenge=0)" : "");
}

bool Enabled() { return g_enabled; }

ModChoice Chosen() { return ChoiceOf(g_chosen); }

bool SetChoice(const ModChoice& c) {
    if (c == ChoiceOf(g_chosen)) return true;
    g_cfg.modRule = ModPresetKey(c.preset);
    g_cfg.blockHse = c.blockHse;
    g_cfg.modOfficialOnly = c.officialOnly;
    ApplyRule(g_cfg);
    const bool saved = SaveIniValues({{"mod_rule", g_cfg.modRule}, {"block_hse", c.blockHse ? "1" : "0"},
                                      {"mod_official", c.officialOnly ? "1" : "0"}}, "mod");
    IniWriteTime(g_iniTime);   // our own write: CheckIni must not read it back as a hand edit
    HSM_LOG("modrules: MODS tab: my lobbies: %s%s", RuleSummary(g_chosen).c_str(), saved ? "" : " (could not write HSMMod.ini)");
    return saved;
}
bool ChallengeEnabled() { return g_enabled && g_challenge; }

void Tick() {
    // Built with mod_rules=0 too: a client always sends its ModList, the host's rules need it.
    if (g_enabled) CheckIni();
    CheckHseLock();
    enhancer::Poll();
    const bool ue4ss = GetModuleHandleW(L"UE4SS.dll") != nullptr;   // UE4SS may load after HSMMod (ue4ss_order=after)
    const uint32_t mv = modmaps::ScanVersion();
    const bool scanDone = modmaps::Listed() && modmaps::AllHashed();
    // The UE4SS mods and proxies are read at the start, when UE4SS appears or goes, and after each finished pak rescan
    // (the player dropped files in and the scan ran again).
    const bool rescanned = scanDone && (!g.scanDone || mv != g.scanAtExtras);
    if (!g.started || ue4ss != g.ue4ssSeen || rescanned) {
        StartExtras();
        g.started = true;
        g.ue4ssSeen = ue4ss;
        g.scanAtExtras = mv;
    }
    g.scanDone = scanDone;
    uint32_t ev;
    { std::lock_guard<std::mutex> lock(g_extras.mu); ev = g_extras.version + (g_extras.running ? 1u << 31 : 0u); }
    const bool enh = enhancer::Detected(), locked = HseLocked();
    if (mv == g.mapsVersion && ev == g.extrasVersion && enh == g.enhancer && locked == g.hseLocked) return;
    g.mapsVersion = mv; g.extrasVersion = ev; g.enhancer = enh; g.hseLocked = locked;
    Rebuild();
}

const ModReport& Own() { return g.report; }
uint32_t OwnVersion() { return g.version; }
Official OwnOfficial() { return update::OfficialStatus(); }

ModRule HostRule(const Lobby& lobby) {
    ModRule r = g_chosen;
    if (r.preset == ModPreset::Bundle) {
        r.list.clear();
        const LobbyPack pack = lobby.Pack();
        for (const LocalPack& lp : modmaps::Packs())
            if (lp.valid && pack.any() && lp.manifest.name == pack.name) { r.list = BundleList(lp.manifest); break; }
        if (r.list.empty()) r.list = BundleFromReport(g.report);   // no pack announced (or not found): the host's own mods, every one required
    } else if (r.preset == ModPreset::Custom && !g_allowFromIni) {
        r.list = ListFromReport(g.report);
    }
    return EffectiveRule(r, g.report, OwnOfficial());
}

void LobbyTick(Lobby& lobby) {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (!lobby.InLobby() || now < next) return;
    next = now + 1000;
    lobby.SetOfficial(OwnOfficial());
    if (!lobby.IsHost()) return;
    if (!g_enabled) { ModRule none = NoRule(); none.blockHse = false; lobby.SetModRule(none); return; }
    if (!g.report.complete) return;
    lobby.SetModRule(HostRule(lobby));
}

// ---- the code challenge -----------------------------------------------------------------------------------------------

namespace {
struct Image {
    bool tried = false, ok = false;
    const uint8_t* base = nullptr;
    PeLayout pe;
    std::vector<uint32_t> relocs;
};
Image g_image;

const Image& OwnImage() {
    if (g_image.tried) return g_image;
    g_image.tried = true;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OwnImage), &self) || !self) { HSM_LOG("modrules: challenge: own module not found"); return g_image; }
    MODULEINFO mi{};
    if (!K32GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof mi) || mi.SizeOfImage == 0) { HSM_LOG("modrules: challenge: module size unknown"); return g_image; }
    const auto* base = static_cast<const uint8_t*>(mi.lpBaseOfDll);
    PeLayout pe;
    if (!ParsePeLayout(base, mi.SizeOfImage, pe)) { HSM_LOG("modrules: challenge: own PE headers not as expected"); return g_image; }
    std::vector<uint32_t> relocs;
    if (pe.relocSize && !ParseDir64Relocs(base + pe.relocRva, pe.relocSize, relocs)) { HSM_LOG("modrules: challenge: own relocations not as expected"); return g_image; }
    g_image.base = base; g_image.pe = pe; g_image.relocs = std::move(relocs); g_image.ok = true;
    HSM_LOG("modrules: challenge: .text %u bytes, %zu fixups", unsigned(pe.textSize), g_image.relocs.size());
    return g_image;
}
}  // namespace

bool Answer(const ModChallengeMsg& m, Sha256Digest& out) {
    const Image& im = OwnImage();
    return im.ok && ComputeAnswer(im.base, im.pe, im.relocs, uint64_t(reinterpret_cast<uintptr_t>(im.base)), m, out);
}

uint32_t CodeSize() { const Image& im = OwnImage(); return im.ok ? im.pe.textSize : 0; }

}  // namespace hsm::game::modrules

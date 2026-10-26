#include "gameplay/save_guard.h"
#include <windows.h>
#include "gameplay/travel.h"
#include "log.h"
#include "ue/script_hook.h"
#include <cstdio>
#include <cwchar>

namespace hsm::game {
using namespace hsm::ue;
namespace {

constexpr const wchar_t* kSave = L"GameProgress.sav";   // GI_Settings.cpp:1431 SaveGameToSlot(Save File, "GameProgress", 0)
constexpr const char* kHookOwner = "saveguard";

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out, bool& exists) {
    out.clear();
    exists = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    exists = true;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1LL << 30);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
    }
    CloseHandle(h);
    return ok;
}

// Written beside the target and moved over it, so a crash never leaves half a file.
bool WriteAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    const std::wstring tmp = path + L".hsmtmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = bytes.empty() || (WriteFile(h, bytes.data(), DWORD(bytes.size()), &put, nullptr) && put == bytes.size());
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

std::wstring BackupName(unsigned long pid) {
    wchar_t b[64];
    std::swprintf(b, 64, L"HSMMP_backup_%lu_GameProgress.sav", pid);
    return b;
}

bool ProcessAlive(unsigned long pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
    CloseHandle(p);
    return alive;
}

}  // namespace

void SaveGuard::Hook() {
    if (hooked_ || !ScriptHookActive()) return;
    // fightOnly: blocked in a session's fight levels only. In the Progression hub the game makes a new character
    // (Reset Player Character) and dresses it (Override Player Character Equipment) for the next fight; blocking them
    // there left the character screen stuck (test 2026-10-25). The in-memory character is the session's; the backup
    // still goes back over GameProgress when the session is over, and Save Game stays blocked everywhere.
    auto block = [this](const char* what, bool fightOnly = false) {
        return [this, what, fightOnly](ScriptCall&) {
            if (!policy_.Blocking() || (fightOnly && !matchLevel_)) return false;
            if (++blocked_ % 50 == 1) HSM_LOG("save: blocked the game's \"%s\" during a session (%u so far)", what, blocked_);
            return true;
        };
    };
    // Literal names, one call each: tools/check_reflection.py verifies HookScript names against the game.
    int n = HookScript(kHookOwner, "GI_Settings_C", "Save Game", block("Save Game"));
    n += HookScript(kHookOwner, "GI_Settings_C", "Reset Player Character", block("Reset Player Character", true));
    n += HookScript(kHookOwner, "GI_Settings_C", "Override Player Character Equipment", block("Override Player Character Equipment", true));
    n += HookScript(kHookOwner, "GI_Settings_C", "Delete Save", block("Delete Save"));
    // The game reloads the game instance from GameProgress on its level loads (it saved it just before, so the pair
    // kept the session's settings). With the save blocked, the load brought back the pre-session ones: test 2026-10-25,
    // every lobby fight came up in Progression (Free Mode Activated false), so Duel ran as Progression co-op, with the
    // players' own save loadouts and no spawners for the mode. While the guard is on, the game instance in memory is
    // the truth; Restore's own Load Game runs after the guard is off.
    n += HookScript(kHookOwner, "GI_Settings_C", "Load Game", block("Load Game"));
    hooked_ = n > 0;
    if (hooked_ || !hookFailLogged_) HSM_LOG("save: %d game save function(s) hooked", n);
    hookFailLogged_ = !hooked_;
}

void SaveGuard::Tick(Adapter& adapter, bool joined, bool matchLevel) {
    if (!leftChecked_) RestoreLeft(adapter);
    matchLevel_ = matchLevel;
    switch (policy_.Update(joined, matchLevel)) {
    case session::SaveAction::Backup: Backup(adapter); break;
    case session::SaveAction::Restore: Restore(adapter); break;
    case session::SaveAction::None: break;
    }
    // Item 9b: the stock lose screens (UI_GiveUp_C, UI_Lose_C, UI_NextFIght_C) are suppressed only while joined, and the
    // guard is on from the same tick, so their "return to tavern" never reaches Override Player Character Equipment
    // unblocked. Backup hooks once; if that found nothing, keep trying while the guard is on.
    if (policy_.Blocking() && !hooked_ && ++hookRetryTicks_ % 60 == 0) Hook();   // about once a second
}

void SaveGuard::Backup(Adapter& adapter) {
    Hook();
    blocked_ = 0;
    settings_ = CaptureGameSettings();
    backup_.clear();
    const std::wstring dir = adapter.SaveGamesDir();
    if (dir.empty()) { HSM_LOG("save: no save folder; the game's own saves are still blocked for this session"); return; }
    std::vector<uint8_t> bytes;
    bool exists = false;
    if (!ReadAll(dir + kSave, bytes, exists)) { HSM_LOG("save: could not read %ls; blocked for this session, no backup", kSave); return; }
    const std::wstring name = BackupName(GetCurrentProcessId());
    if (!WriteAll(dir + name, bytes)) { HSM_LOG("save: could not write the backup; blocked for this session"); return; }
    backup_ = name;   // an empty backup: there was no save, and none is kept after the session
    HSM_LOG("save: GameProgress backed up (%zu bytes%s); the game's own saves are off until the session is over",
            bytes.size(), exists ? "" : ", no save yet");
}

// Puts the backup back over GameProgress.sav when they differ (an empty backup: deletes a save the session made), then
// deletes the backup. False when it could not.
bool SaveGuard::PutBack(const std::wstring& dir, const std::wstring& backup, const char* why) {
    std::vector<uint8_t> was, now;
    bool backupExists = false, saveExists = false;
    if (!ReadAll(dir + backup, was, backupExists) || !backupExists) { HSM_LOG("save: backup %ls unreadable (%s)", backup.c_str(), why); return false; }
    if (!ReadAll(dir + kSave, now, saveExists)) { HSM_LOG("save: %ls unreadable (%s); backup kept", kSave, why); return false; }
    bool ok = true;
    if (was.empty()) {
        if (saveExists) ok = DeleteFileW((dir + kSave).c_str()) != 0;
        HSM_LOG("save: %s: no save before the session; %s", why, saveExists ? (ok ? "the one made since is removed" : "could not remove the one made since") : "none made since");
    } else if (!saveExists || now != was) {
        ok = WriteAll(dir + kSave, was);
        HSM_LOG("save: %s: the game had changed GameProgress (%zu -> %zu bytes); %s", why, was.size(), now.size(),
                ok ? "the backup is put back" : "putting the backup back FAILED, backup kept");
    } else {
        HSM_LOG("save: %s: GameProgress unchanged", why);
    }
    if (ok) DeleteFileW((dir + backup).c_str());
    return ok;
}

void SaveGuard::Reload(Adapter& adapter) {
    UObject* gi = adapter.GameInstance();
    if (!gi) return;
    Call load(gi, "Load Game");   // GI_Settings.cpp:603: every saved field back into the game instance
    if (!load.valid() || !load.invoke()) HSM_LOG("save: Load Game could not be called");
}

void SaveGuard::Restore(Adapter& adapter) {
    const std::wstring dir = adapter.SaveGamesDir();
    if (!backup_.empty() && !dir.empty()) PutBack(dir, backup_, "session over");
    backup_.clear();
    Reload(adapter);
    const size_t n = ApplyGameSettings(settings_);
    settings_.clear();
    HSM_LOG("save: guard off; %u game save call(s) were blocked, game instance reloaded, %zu setting(s) as before the session", blocked_, n);
}

void SaveGuard::RestoreLeft(Adapter& adapter) {
    const std::wstring dir = adapter.SaveGamesDir();
    if (dir.empty()) return;   // the engine is not up yet: next tick
    leftChecked_ = true;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"HSMMP_backup_*_GameProgress.sav").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    bool restored = false;
    do {
        unsigned long pid = 0;
        if (std::swscanf(fd.cFileName, L"HSMMP_backup_%lu_", &pid) != 1 || !pid) continue;
        const bool own = pid == GetCurrentProcessId();
        if (!session::RestoreLeftBackup(true, !own && ProcessAlive(pid), own)) continue;
        restored = PutBack(dir, fd.cFileName, "left by a session that did not end") || restored;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (restored) Reload(adapter);
}

}  // namespace hsm::game

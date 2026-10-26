// Keeps a multiplayer session out of the player's own game save (session/hsm_save_guard.h has the rule).
//
// While the guard is on: GameProgress.sav is backed up (HSMMP_backup_<pid>_GameProgress.sav next to it), and the
// game instance's own Save Game, Reset Player Character, Override Player Character Equipment, Delete Save and Load
// Game are blocked in the script VM. When it goes off: the backup is put back if the game wrote the save anyway, the game
// instance is reloaded from it (Load Game) and the settings it had before the session (play mode, Free Mode, ...) are
// written back, so the tavern after a session is the one before it. A backup whose process is gone (a crash) is put
// back at the next start. Game thread only.
#pragma once
#include "gameplay/adapter.h"
#include "hsm_protocol.h"
#include "session/hsm_save_guard.h"
#include <string>
#include <vector>

namespace hsm::game {

class SaveGuard {
public:
    // Every tick. joined: in a session (lobby joined or hosting); matchLevel: this machine is in a session fight level.
    void Tick(Adapter& adapter, bool joined, bool matchLevel);
    bool Blocking() const { return policy_.Blocking(); }

private:
    void Hook();
    void Backup(Adapter& adapter);
    void Restore(Adapter& adapter);
    void RestoreLeft(Adapter& adapter);   // once per process: backups of sessions whose process is gone
    static bool PutBack(const std::wstring& dir, const std::wstring& backup, const char* why);
    static void Reload(Adapter& adapter);

    session::SaveGuardPolicy policy_;
    bool hooked_ = false, leftChecked_ = false, hookFailLogged_ = false;
    bool matchLevel_ = false;             // this tick's matchLevel (the fight-only blocks)
    std::wstring backup_;                 // this session's backup file (empty: none made)
    std::vector<Setting> settings_;       // the game instance's settings before the session
    unsigned blocked_ = 0;                // calls blocked this session
    unsigned hookRetryTicks_ = 0;
};

}  // namespace hsm::game

// When the player's own game save is protected from a multiplayer session (pure, no engine types).
//
// A session changes the game instance in place (Free Mode on, the play and game modes, the fight's settings), and the
// game persists the game instance through its own Save Game from a dozen places (level loads, give-up, death screens,
// the pause menu); a Progression death screen even makes a new character (Reset Player Character). Two-player test
// 2026-10-25: a player found their single-player character gone after a session. So while a session is on, the save
// is backed up, the game's own save/reset functions are blocked (and its Load Game, which would put the pre-session
// settings back on a level load), and when it is over the backup is put back and the game instance reloaded from it:
// a session leaves no trace in GameProgress.
//
// The guard comes on when the player joins a session (lobby or host). It stays on after the session ends while this
// machine is still in a fight level: the level was set up as a session fight, and the game's own death or give-up in it
// would otherwise save, or reset the character, with the session's settings. It goes off (restore) in a menu or hub.
#pragma once
#include <cstdint>

namespace hsm::session {

enum class SaveAction : uint8_t {
    None,       // nothing to do this tick
    Backup,     // the guard comes on: back the save up (unless a backup is already there) and start blocking
    Restore,    // the guard goes off: put the backup back, reload the game instance from it, stop blocking
};

class SaveGuardPolicy {
public:
    // One tick: joined = in a session (lobby joined or hosting); fightLevel = this machine is in a fight level.
    SaveAction Update(bool joined, bool fightLevel) {
        if (!on_) {
            if (!joined) return SaveAction::None;
            on_ = true;
            return SaveAction::Backup;
        }
        if (joined || fightLevel) return SaveAction::None;
        on_ = false;
        return SaveAction::Restore;
    }
    bool Blocking() const { return on_; }   // the game's own Save Game / Reset Player Character / ... are blocked

private:
    bool on_ = false;
};

// A backup left by a session whose process is gone (a crash, or the game closed mid-session) is put back at the next
// start, before the player can play on it; one a live process still owns (two copies of the game on one machine) is
// left alone.
inline bool RestoreLeftBackup(bool backupExists, bool ownerAlive, bool ownBackup) {
    return backupExists && !ownBackup && !ownerAlive;
}

// What to do with a backup file left in the save folder (found at the next start, by RestoreLeft or the startup sweep).
//   Restore   - put it back over GameProgress.sav (the session that made it ended without restoring)
//   KeepAside - do not touch the save; rename the backup to HSMMP_stale_<utc>_... so nothing restores it later
//   Ignore    - not ours to touch (no such file, our own live session's, or another live process's)
// The stale rules (S1): an empty backup means "there was no save", and restoring it would delete one, so it is never
// restored; a save written more than kSaveNewerSlackSec after the backup is progress made without the mod's help
// (the mod was inert, or the session ended unseen) and the backup would wipe it.
enum class LeftBackup : uint8_t { Ignore, Restore, KeepAside };
constexpr int64_t kSaveNewerSlackSec = 60;
inline LeftBackup DecideLeftBackup(bool exists, bool ownerAlive, bool own, uint64_t backupBytes, int64_t backupMtimeSec,
                                   bool saveExists, int64_t saveMtimeSec) {
    if (!RestoreLeftBackup(exists, ownerAlive, own)) return LeftBackup::Ignore;
    if (backupBytes == 0) return LeftBackup::KeepAside;
    if (saveExists && saveMtimeSec - backupMtimeSec > kSaveNewerSlackSec) return LeftBackup::KeepAside;
    return LeftBackup::Restore;
}

}  // namespace hsm::session

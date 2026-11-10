// Lobby mod rules, this machine's side (docs/PLAN_MOD_RULES.md; the rules are shared/session/hsm_mod_rules.h, the code
// challenge shared/session/hsm_mod_challenge.h). Three jobs:
//   report     what this machine runs: mod paks (gameplay/mod_maps.cpp's scan, each a map pak or a content pak), the
//              UE4SS mods UE4SS has enabled (stock ones apart; each hashed over Scripts/main.lua and dlls/main.dll), the
//              other proxy DLLs next to the exe, and a detected Half Sword Enhancer. Names and hashes only.
//   rule       the host's chosen rule from HSMMod.ini ([mod] mod_rule, block_hse, mod_official, mod_allow), made
//              effective against the host's own report and build (EffectiveRule)
//   challenge  this DLL's own code, relocation-normalised, for the host's ModChallenge (both sides of it)
// Game thread only, except where a function says otherwise; the file work runs on a worker thread.
#pragma once
#include "config.h"
#include "session/hsm_mod_challenge.h"
#include "session/hsm_mod_rules.h"
#include "session/hsm_mod_switch.h"
#include <cstdint>
#include <string>

namespace hsm::game {
class Lobby;

namespace modrules {

void Configure(const Config& cfg);   // once at start
// Mods off for the next game only (session/hsm_mod_switch.h: what a lobby's rule refuses). mods.txt gets these UE4SS
// mods off; the player's own file is kept, and comes back when that game closes, or at the next start if it crashed.
// DLLs beside the exe are renamed to <name>.off, and renamed back the same way. Paks (the report's names) go into
// HSMMod-switch.txt for the next game's ApplySwitchEarly, since this game holds them open. Nothing is deleted.
// lockHse: the next game locks HS Enhancer (it stays loaded; HseLocked). A locked game always hands the lock on.
bool TurnOffForNextGame(const std::vector<std::string>& mods, const std::vector<std::string>& dllsBesideExe = {}, const std::vector<std::string>& paks = {},
                        bool lockHse = false);
// DllMain, before the engine runs: a game that restarted for a lobby left HSMMod-switch.txt; this one waits for it to
// close (20 s at most), renames its paks to <name>.pak.off and lists them for RestoreModsTxt, and locks HS Enhancer's
// settings file when asked (match/hsm_online_rules.h LockedHseIni; the player's file is copied first). Every other start
// puts back a copy an earlier locked game left, then turns HSE's features off in its file, its keys kept
// (HseStartOffIni). Plain Win32 (loader lock, no log yet). A pak that can't be renamed, or a file that can't be
// written, leaves the mod as it is: the host then refuses it in plain words, no second restart.
void ApplySwitchEarly();
void RestoreModsTxt();                // at exit: puts it back if this was that game, and HS Enhancer's own settings
// HS Enhancer can't do anything here: this game locked it and its file is locked still, or it started with its features
// off, they are off still (its file is read again every 5 s) and its keys are held (enhancer::hold, from HoldTick). The
// report then names it locked (session/hsm_mod_rules.h ModKind::HseLocked), which every rule takes.
bool HseLocked();
// HseLocked would be true in a lobby that blocks HS Enhancer: locked, or off with its keys ready to be held. Lobbies
// that block it then need no restart.
bool HseHoldable();
// The report as a lobby that blocks HS Enhancer would get it once joined (HS Enhancer locked when HseHoldable), for
// the browser's pre-check and the switch-off plan.
const ModReport& OwnAsGuest();
// Each frame: HS Enhancer's keys held while this machine joins a lobby, or is a guest in one whose rule refuses HS
// Enhancer (session/hsm_mod_rules.h RuleRefusesHse), and free everywhere else. Logs each change.
void HoldTick(const Lobby& lobby);
// The lobby a restarted game joins on its own (HSMMod-rejoin.txt next to HSMMod.ini: session/hsm_mod_switch.h
// RejoinFile, the lobby's password in plain text for the kRejoinWindowS it lives). TakeRejoin reads and deletes it;
// false: none, or past its deadline.
bool WriteRejoin(const RejoinFile& f);
bool TakeRejoin(RejoinFile& out);
void ApplyWolverineFile(const Config& cfg);   // puts the picked class's parts in Half Wolverine's settings file
bool Enabled();                      // [mod] mod_rules=1 (default): this machine's lobbies enforce a rule, its browser pre-checks
bool ChallengeEnabled();             // [mod] mod_challenge=1 (default)

// Each frame: polls the Enhancer detector and rebuilds the report when a source changed (the UE4SS and proxy part on a
// worker thread). Logs the report once each time it changes and is complete.
void Tick();
const ModReport& Own();              // complete once every scan is done and every hash is in
uint32_t OwnVersion();               // changes whenever Own() does
Official OwnOfficial();              // the update check's verdict on this DLL (net/update_check.h)

// The rule this machine enforces as host in `lobby` (its announced pack makes the Bundle list): the INI's choice, with
// HS Enhancer allowed when this machine runs it and official-only off when this build is not official.
ModRule HostRule(const Lobby& lobby);
// The host's choice as the INI has it (preset and the two switches; no Bundle list, no loosening), for the lobby's MODS
// tab. SetChoice is that tab's steppers: it takes effect at once (the next ModRulesTick announces it, LobbyTick
// republishes it) and writes mod_rule, block_hse and mod_official under [mod] in HSMMod.ini. False: not written (the
// rule still changed for this run).
ModChoice Chosen();
bool SetChoice(const ModChoice& c);
// Each frame while in a Steam lobby, acting once a second: every member keeps its official-build verdict in member data;
// the host announces HostRule in lobby data (when its report is complete). mod_rules=0 announces Anything with HS
// Enhancer allowed: such a host enforces nothing.
void LobbyTick(Lobby& lobby);

// The code challenge on this DLL as it is mapped now. False when its image cannot be read as a PE (the client answers
// Unable, the host sends no challenge). Same build, same answer: the host compares with its own.
bool Answer(const ModChallengeMsg& m, Sha256Digest& out);
uint32_t CodeSize();                 // .text bytes of this DLL (0: unreadable); the host picks ranges in it

}  // namespace modrules
}  // namespace hsm::game

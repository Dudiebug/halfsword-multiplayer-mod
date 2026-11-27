// The words of the notices (UI overhaul PR2): every system message the mod shows, in one place so the length and the
// wording are tested. A fixed message is a row of kFixed (kind, title, text, key); one with a name or a value in it is
// a builder that returns a Msg. The session hands a Msg to ui::Notify. Plain words: no INI keys, no jargon, no
// "Experimental"; a title is a short topic (CRYPTS, LOBBY, CLASSES, REPORTS), never "HSMMod" or "SERVER".
// Keys: a notice that may come again, or that a later one settles, shares a key (the stack keeps one of them).
#pragma once
#include "match/hsm_notice.h"
#include "session/hsm_report.h"
#include <cmath>
#include <cstdint>
#include <string>

namespace hsm::notices {

struct Msg { NoticeKind kind = NoticeKind::Info; std::string title, text, key; };

enum class Id : uint8_t {
    CryptsStartApart, CryptsStartTogether, CryptsMinute, CryptsStopped, CryptsNoRoom, CryptsLate,
    CryptsEndNothing, CryptsEndDied, CryptsEndOut, CryptsEndOutTogether, CryptsKeyLine,
    NotInLobby, ChatSlow, ChatNotSent, HostGone, NewHostSelf, LeftBaseStats,
    UnrestrictedWarn, HostAllowsHse, HostPhysicsParts, SheatheClaws, ClassSelfOk, ClassHostOk,
    InventoryFailed, SettingsFailed, ScriptHookMissing,
    NobodyInFront, GroundCleared,
    BugHint, HelpImproveSending, CrashHint,
    ReportsOff, LobbyNotInCrypts, LobbyNone, LobbyNotInProgression, ChatPaused, PhotoNoMatch, PhotoNotAllowed, PhotoNotReady, PhotoDead,
    LobbyRejoin, LobbyLost,
    Count
};

// `longer`: the line carries a fact the player should have whole (what a log holds), so it may use kNoticeMaxBytes.
struct Fixed { Id id; NoticeKind kind; const char* title; const char* text; const char* key; bool longer; };

inline constexpr Fixed kFixed[] = {
    {Id::CryptsStartApart, NoticeKind::Info, "CRYPTS", "Gather your kit. When time is up, you fight in what you carry.", "crypts-start", false},
    {Id::CryptsStartTogether, NoticeKind::Info, "CRYPTS", "Loot the Crypts together. When time is up, you fight in what you carry.", "crypts-start", false},
    {Id::CryptsMinute, NoticeKind::Warn, "CRYPTS", "1 minute left to gather.", "crypts-minute", false},
    {Id::CryptsStopped, NoticeKind::Info, "CRYPTS", "The Crypts run is stopped.", "crypts-end", false},
    {Id::CryptsNoRoom, NoticeKind::Warn, "CRYPTS", "The Crypts could not be opened here. You fight in what the host deals.", "crypts-start", false},
    {Id::CryptsLate, NoticeKind::Info, "CRYPTS", "The Crypts run is nearly over. You fight as a fresh character.", "crypts-start", false},
    {Id::CryptsEndNothing, NoticeKind::Info, "CRYPTS", "Nothing gathered: you fight as a fresh character. Wait in the menu for the battle.", "crypts-end", false},
    {Id::CryptsEndDied, NoticeKind::Info, "CRYPTS", "You died in the Crypts. Your kit is kept. Wait in the menu for the battle.", "crypts-end", false},
    {Id::CryptsEndOut, NoticeKind::Info, "CRYPTS", "Out of the Crypts with your kit. Wait in the menu for the battle.", "crypts-end", false},
    {Id::CryptsEndOutTogether, NoticeKind::Info, "CRYPTS", "Out of the Crypts with your kit. Wait in the menu for the battle.", "crypts-end", false},
    {Id::CryptsKeyLine, NoticeKind::Info, "CRYPTS", "A locked room shows once it is built. Rooms appear as you walk.", "crypts-keyline", false},
    {Id::NotInLobby, NoticeKind::Info, "LOBBY", "You are not in a lobby. Join one to chat.", "lobby-chat", false},
    {Id::ChatSlow, NoticeKind::Info, "LOBBY", "Slow down. You are sending messages too fast.", "lobby-chat", false},
    {Id::ChatNotSent, NoticeKind::Warn, "LOBBY", "The message could not be sent. Try again.", "lobby-chat", false},
    {Id::HostGone, NoticeKind::Warn, "LOBBY", "The host is gone. Waiting for a new host...", "migration", false},
    {Id::NewHostSelf, NoticeKind::Done, "LOBBY", "The host left. You are the new host.", "migration", false},
    {Id::LeftBaseStats, NoticeKind::Error, "LOBBY", "You left the lobby: something other than your class changed your fighter's stats.", "class-self", false},
    {Id::UnrestrictedWarn, NoticeKind::Warn, "CLASSES", "Unrestricted Classes: no balance, and fighters past the game's limits. Expect crashes.", "class-unrestricted", false},
    {Id::HostAllowsHse, NoticeKind::Warn, "CLASSES", "The host allows the Half Sword Enhancer mod here. It can change anything.", "class-enhancer", false},
    {Id::HostPhysicsParts, NoticeKind::Warn, "CLASSES", "Host physics is on, so Half Wolverine's parts don't work. Ask the host for Per player sync.", "class-physics", false},
    {Id::SheatheClaws, NoticeKind::Warn, "CLASSES", "Sheathe your claws to hold a weapon.", "claws", false},
    {Id::ClassSelfOk, NoticeKind::Done, "CLASSES", "Your class fits the rules again.", "class-self", false},
    {Id::ClassHostOk, NoticeKind::Done, "CLASSES", "The host's class fits the rules again.", "class-host", false},
    {Id::InventoryFailed, NoticeKind::Error, "GAME", "The inventory screen would not open. Close the menu and try again.", "game-screen", false},
    {Id::SettingsFailed, NoticeKind::Error, "GAME", "The settings screen would not open. Close the menu and try again.", "game-screen", false},
    {Id::ScriptHookMissing, NoticeKind::Error, "GAME", "Multiplayer cannot start: this game version is not supported yet. Update HSMMod.", "multiplayer", false},
    {Id::NobodyInFront, NoticeKind::Info, "MATCH", "Nobody in front of you. Face a fighter and press C again.", "roulette", false},
    {Id::GroundCleared, NoticeKind::Info, "MATCH", "Ground cleared.", "ground-cleared", false},
    {Id::BugHint, NoticeKind::Info, "REPORTS", "Found a bug? In the chat, type /bug and what happened. Your log goes with it.", "bug-hint", false},
    {Id::HelpImproveSending, NoticeKind::Info, "REPORTS",
     "Help improve the mod is on: sending your log too. Steam IDs, IPs and your Windows user name are removed; player names stay.", "report-log", true},
    {Id::CrashHint, NoticeKind::Info, "REPORTS", "The game crashed last session. Turn on Help improve the mod to send crash reports.", "crash-hint", false},
    // The pause menu (match/hsm_pause_panel.h): what a click on a dimmed button says. One key, so a second click replaces the first.
    {Id::ReportsOff, NoticeKind::Info, "REPORTS", "Bug reports are switched off on this PC. Switch them on in the mod's settings, then restart.", "pause-why", false},
    {Id::LobbyNotInCrypts, NoticeKind::Info, "LOBBY", "Not during a Crypts run. Use LEAVE TO MAIN MENU.", "pause-why", false},
    {Id::LobbyNone, NoticeKind::Info, "LOBBY", "There is no lobby to go back to. Use LEAVE TO MAIN MENU.", "pause-why", false},
    {Id::LobbyNotInProgression, NoticeKind::Info, "LOBBY", "Not during a Progression fight. Use LEAVE TO MAIN MENU.", "pause-why", false},
    // The chat key pressed while the menu is open; what was typed is kept for the next time.
    {Id::ChatPaused, NoticeKind::Info, "CHAT", "Close the menu to chat.", "chat-paused", false},
    // The photo mode key that did nothing (match/hsm_freecam.h PhotoKeyNotice).
    {Id::PhotoNoMatch, NoticeKind::Info, "PHOTO", "Photo mode works during a match.", "photo-key", false},
    {Id::PhotoNotAllowed, NoticeKind::Info, "PHOTO", "The host turned free cameras off for this match, photo mode with them.", "photo-key", false},
    {Id::PhotoNotReady, NoticeKind::Info, "PHOTO", "Photo mode could not start here.", "photo-key", false},
    {Id::PhotoDead, NoticeKind::Info, "PHOTO", "Photo mode needs a living fighter.", "photo-key", false},
    // Steam left this machine out of its lobby after a lost connection (session/hsm_lobby_strand.h): it joins again, or gives up.
    {Id::LobbyRejoin, NoticeKind::Warn, "LOBBY", "The connection to Steam dropped and you left the lobby. Joining it again...", "lobby-rejoin", false},
    {Id::LobbyLost, NoticeKind::Error, "LOBBY", "Could not get back into the lobby. Join it again from the list.", "lobby-rejoin", false},
};
static_assert(sizeof(kFixed) / sizeof(kFixed[0]) == size_t(Id::Count), "one row per Id, in order");

inline const Fixed& Of(Id id) { return kFixed[size_t(id)]; }
inline Msg Make(Id id) { const Fixed& f = Of(id); return {f.kind, f.title, f.text, f.key}; }

// A line the session builds elsewhere (a rule summary, the update check, the profiler): made fit, never over the cap.
inline Msg Plain(NoticeKind kind, const std::string& title, const std::string& text, const std::string& key = std::string()) {
    return {kind, title, NoticeFit(text), key};
}

// ---------------------------------------------------------------- lobby and classes
inline Msg Presence(const std::string& name, bool joined) {
    return {NoticeKind::Info, "LOBBY", NoticeFit((name.empty() ? std::string("?") : name) + (joined ? " joined." : " left.")), std::string()};
}
// The host's view of a guest who used BACK TO LOBBY (session/hsm_bench.h): out of the match, still in the lobby. Keyed by the name, so
// the same player clicking again is one notice.
inline Msg WentBackToLobby(const std::string& name) {
    return {NoticeKind::Info, "LOBBY", NoticeFit((name.empty() ? std::string("A player") : name) + " went back to the lobby."), "back-to-lobby:" + name};
}
inline Msg NewHost(const std::string& name) {
    return {NoticeKind::Info, "LOBBY", NoticeFit("The host left. " + (name.empty() ? std::string("Another player") : name) + " is the new host."), "migration"};
}
inline Msg RuleChanged(const std::string& summary) { return Plain(NoticeKind::Info, "LOBBY", summary, "rule-changed"); }
// Why START will not start (match/hsm_start_gate.h), or why a click on a dimmed control did nothing. One key: a second click
// replaces the notice instead of stacking another.
inline Msg StartWhy(const std::string& why) { return Plain(NoticeKind::Warn, "LOBBY", why, "start-why"); }
inline Msg StatsPastLimits(const std::string& name, const std::string& summary) {
    return {NoticeKind::Warn, "CLASSES", NoticeFit((name.empty() ? std::string("A player") : name) + "'s stats are past the limits: " + summary), std::string()};
}
inline Msg NoClassCard(const std::string& name) {
    return {NoticeKind::Warn, "CLASSES", NoticeFit((name.empty() ? std::string("A player") : name) + " isn't sending a class card."), std::string()};
}
// `why`: OnlineBlockReason, a sentence ("Your class breaks the rules: ...."). What to do stays whole; the reason gives way.
inline Msg ClassSelfBreaks(const std::string& why) {
    return {NoticeKind::Warn, "CLASSES", NoticeJoin(why, "Fix it within a minute or you leave the lobby."), "class-self"};
}
inline Msg ClassSelfLeft(const std::string& why) {
    return {NoticeKind::Error, "LOBBY", NoticeJoin("You left the lobby. " + why, "Fix it and join again."), "class-self"};
}
inline Msg ClassHostBreaks(const std::string& problem) {
    return {NoticeKind::Warn, "CLASSES", NoticeJoin("The host breaks the class rules: " + problem + ".", "You leave in a minute unless it's fixed."), "class-host"};
}
inline Msg ClassHostLeft(const std::string& problem) {
    return {NoticeKind::Error, "LOBBY", NoticeFit("You left the lobby: the host breaks the class rules: " + problem + "."), "class-host"};
}
// The host's view of another player: keyed by the name, so a player who stays broken is one notice.
inline Msg ClassPlayerBreaks(const std::string& name, const std::string& problem) {
    return {NoticeKind::Warn, "CLASSES", NoticeFit(name + " breaks the class rules: " + problem + "."), "class-of:" + name};
}
inline Msg ClassPlayerRemoved(const std::string& name, const std::string& problem) {
    return {NoticeKind::Warn, "LOBBY", NoticeFit(name + " was removed: " + problem + "."), "class-of:" + name};
}

// ---------------------------------------------------------------- the match
inline Msg FrameRate(double fps, int floorFps) {
    return {NoticeKind::Warn, "FRAME RATE",
            NoticeFit("Your frame rate is " + std::to_string(std::lround(fps)) + " FPS, under " + std::to_string(floorFps) + ". The others see your fighter in bigger steps."),
            "frame-rate"};
}
inline Msg ChaosStats(const std::string& line) { return {NoticeKind::Info, "CHAOS", NoticeFit("Your stats this round: " + line), "chaos-stats"}; }

// ---------------------------------------------------------------- bug reports
// The answer to a report, a crash report or a log. The issue link is not here: the multiplayer page's note keeps the
// full line (ReportOutcomeLine) with it, a notice is for knowing it went.
inline Msg ReportResult(ReportJob job, const ReportVerdict& v, bool shared) {
    const bool log = job == ReportJob::Log;
    const std::string what = job == ReportJob::Crash ? "Crash report" : log ? "Your log" : "Bug report";
    std::string reason = v.reason;
    if (!reason.empty() && reason[0] >= 'a' && reason[0] <= 'z') reason[0] = char(reason[0] - 'a' + 'A');
    Msg m{NoticeKind::Done, "REPORTS", std::string(), "report-result"};
    switch (v.outcome) {
        case ReportOutcome::Done:
            m.text = log ? "Your log was added to the bug report. Thanks!"
                         : what + " filed. Thanks!" + (shared ? " The other players were asked to add their logs." : "");
            break;
        case ReportOutcome::MaybeDone:
            m.kind = NoticeKind::Warn;
            m.text = log ? NoticeJoin(reason + ":", "your log may have been added.")
                         : NoticeJoin(reason + ": your " + (job == ReportJob::Crash ? "crash" : "bug") + " report may have been filed.",
                                      "Check the mod's GitHub issues before reporting again.");
            break;
        case ReportOutcome::NotSent:
            m.kind = NoticeKind::Error;
            m.text = NoticeFit(what + (log ? " was not sent: " : " not sent: ") + v.reason + ".");
            break;
        case ReportOutcome::Refused:
            m.kind = NoticeKind::Error;
            m.text = NoticeFit(what + (log ? " was not added: " : " not filed: ") + v.reason + ".");
            break;
    }
    return m;
}
// Another player filed a report. With Help improve on the log goes along by itself; otherwise the player is asked.
inline Msg ReportFiled(const std::string& name, bool autoSend) {
    const std::string who = name.empty() ? std::string("A player") : name;
    if (autoSend) return {NoticeKind::Info, "REPORTS", NoticeFit(who + " filed a bug report."), "report-offer"};
    return {NoticeKind::Warn, "REPORTS", NoticeJoin(who + " filed a bug report.", "Type /sendlog to add your log, or /nolog."), "report-offer"};
}

// ---------------------------------------------------------------- the pause menu and the chat
// A line made a sentence: a full stop where it ends without one (the loadout rules' lines were written for a hint line).
inline std::string Sentence(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (!s.empty() && s.back() != '.' && s.back() != '!' && s.back() != '?') s += '.';
    return s;
}
// The answer to CHANGE LOADOUT (the host's verdict, or why it was not asked). One key: "Sending your loadout..." gives way to the answer.
inline Msg LoadoutAnswer(NoticeKind kind, const std::string& line) { return Plain(kind, "LOADOUT", Sentence(line), "loadout"); }
// A click on a CHANGE LOADOUT the rules dim (the Crypts gather, in a fight, a host-dealt kit): the rule's own line.
inline Msg LoadoutWhy(const std::string& why) { return Plain(NoticeKind::Info, "LOADOUT", Sentence(why), "pause-why"); }
// REPORT A BUG waited for the chat and it never came: where to type the report instead. `key`: the chat key's name ("Enter", "T").
inline Msg BugChatTimeout(const std::string& key) {
    return Plain(NoticeKind::Info, "REPORTS", "The chat did not open. Press " + (key.empty() ? std::string("Enter") : key) + ", then type /bug and what happened.", "bug-hint");
}

}  // namespace hsm::notices

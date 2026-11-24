// The notice catalogue: every fixed line within its cap, in plain words (no INI keys, no jargon, no "Experimental"), and
// the builders' lines within the cap with what the player must do kept whole.
#include "match/hsm_notice_text.h"
#include <cctype>
#include <cstdio>
#include <set>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::notices;

static std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static bool Valid8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        size_t len = 0; uint32_t cp = 0;
        if (!ChatUtf8Next(s, i, len, cp)) return false;
        i += len;
    }
    return true;
}

static void TestCatalogue() {
    std::set<std::string> keys;
    int longer = 0;
    for (size_t i = 0; i < size_t(Id::Count); ++i) {
        const Fixed& f = kFixed[i];
        const std::string text = f.text, title = f.title, key = f.key;
        CHECK(size_t(f.id) == i);                                                  // one row per Id, in order
        CHECK(!text.empty() && !title.empty() && !key.empty());
        CHECK(text.size() <= (f.longer ? kNoticeMaxBytes : kNoticeFixedBytes));    // the cap
        if (f.longer) { ++longer; CHECK(text.size() > kNoticeFixedBytes); }        // "longer" only where it is needed
        CHECK(text == SanitizeNotice(text) && Valid8(text));                       // already clean: what is written is shown
        CHECK(title.size() <= kNoticeTitleBytes && SanitizeNotice(title) == title);
        for (char c : title) CHECK(!std::islower(static_cast<unsigned char>(c)));  // a title is a topic in capitals
        CHECK(key.size() <= kNoticeKeyBytes && key.find(' ') == std::string::npos);
        const char last = text.back();
        CHECK(last == '.' || last == '?');                                         // a sentence
        // Plain words.
        const std::string low = Lower(text);
        for (const char* bad : {"untested", "experimental", ".ini", "_", "=", "script vm", "owner sync", "hs enhancer", "see the log", "(hsmmod.",
                                "server:", "armor", "color"})
            CHECK(!Has(low, bad));
        CHECK(title != "HSMMOD" && title != "SERVER");                             // a topic, not a sender
        // The look of the whole row, as the stack would show it.
        NoticeStack s;
        const Msg m = Make(f.id);
        CHECK(s.Push({m.kind, m.title, m.text, m.key, 0, 0}, 0));
        const NoticeScene v = s.View(1);
        CHECK(v.rows.size() == 1 && v.rows[0].text == text && v.rows[0].title == title);   // the stack cuts nothing
        keys.insert(key);
    }
    CHECK(longer <= 2);                                                            // a longer line is the exception
    CHECK(Of(Id::GroundCleared).kind == NoticeKind::Info && Of(Id::CryptsMinute).kind == NoticeKind::Warn);
    CHECK(Of(Id::ClassSelfOk).kind == NoticeKind::Done && Of(Id::InventoryFailed).kind == NoticeKind::Error);
}

// The approved wording (the mock) where it exists.
static void TestApprovedText() {
    CHECK(std::string(Of(Id::CryptsStartApart).text) == "Gather your kit. When time is up, you fight in what you carry.");
    CHECK(std::string(Of(Id::CryptsStartTogether).text) == "Loot the Crypts together. When time is up, you fight in what you carry.");
    CHECK(std::string(Of(Id::CryptsMinute).text) == "1 minute left to gather.");
    CHECK(std::string(Of(Id::CryptsKeyLine).text) == "A locked room shows once it is built. Rooms appear as you walk.");
    CHECK(std::string(Of(Id::NotInLobby).text) == "You are not in a lobby. Join one to chat.");
    CHECK(std::string(Of(Id::NobodyInFront).text) == "Nobody in front of you. Face a fighter and press C again.");
    CHECK(std::string(Of(Id::ClassSelfOk).text) == "Your class fits the rules again.");
    CHECK(std::string(Of(Id::UnrestrictedWarn).text) == "Unrestricted Classes: no balance, and fighters past the game's limits. Expect crashes.");
    CHECK(std::string(Of(Id::InventoryFailed).text) == "The inventory screen would not open. Close the menu and try again.");
    // Titles: the topics the plan names.
    CHECK(std::string(Of(Id::CryptsStopped).title) == "CRYPTS" && std::string(Of(Id::NotInLobby).title) == "LOBBY");
    CHECK(std::string(Of(Id::SheatheClaws).title) == "CLASSES" && std::string(Of(Id::BugHint).title) == "REPORTS");
    // Host physics names the lobby's own Sync row words ("Host physics", "Per player"), not "Owner Sync".
    CHECK(Has(Of(Id::HostPhysicsParts).text, "Host physics") && Has(Of(Id::HostPhysicsParts).text, "Per player"));
    CHECK(Has(Of(Id::HostAllowsHse).text, "Half Sword Enhancer"));
    // The crypts keys: a start line, the minute warning and the end are settled by their own keys.
    CHECK(std::string(Of(Id::CryptsMinute).key) == "crypts-minute" && std::string(Of(Id::CryptsStartApart).key) == "crypts-start");
    CHECK(std::string(Of(Id::CryptsEndDied).key) == "crypts-end");
    // The key marker's explanation is its own notice: one that does not replace the start line or the end line.
    CHECK(std::string(Of(Id::CryptsKeyLine).key) == "crypts-keyline" && Of(Id::CryptsKeyLine).kind == NoticeKind::Info);
    CHECK(std::string(Of(Id::CryptsKeyLine).title) == "CRYPTS");
    CHECK(std::string(Of(Id::ClassSelfOk).key) == std::string(ClassSelfBreaks("x").key));          // the fix replaces the warning
    CHECK(std::string(Of(Id::ClassHostOk).key) == std::string(ClassHostBreaks("x").key));
}

static void TestClassBuilders() {
    const std::string why = "Your class breaks the rules: strength is past the limit.";
    Msg m = ClassSelfBreaks(why);
    CHECK(m.kind == NoticeKind::Warn && m.title == "CLASSES" && m.key == "class-self");
    CHECK(m.text == why + " Fix it within a minute or you leave the lobby.");
    const std::string longWhy = "Your class breaks the rules: " + std::string(300, 'r') + ".";
    m = ClassSelfBreaks(longWhy);
    CHECK(m.text.size() <= kNoticeMaxBytes && Valid8(m.text));
    CHECK(m.text.size() > kNoticeMaxBytes - 10);                                                    // the reason still fills the rest
    const std::string fix = "Fix it within a minute or you leave the lobby.";
    CHECK(m.text.size() > fix.size() && m.text.substr(m.text.size() - fix.size()) == fix);          // the way out stays whole
    CHECK(Has(m.text, "..."));
    m = ClassSelfLeft(why);
    CHECK(m.kind == NoticeKind::Error && m.title == "LOBBY" && m.key == "class-self");
    CHECK(m.text == "You left the lobby. " + why + " Fix it and join again.");
    m = ClassSelfLeft(longWhy);
    const std::string again = "Fix it and join again.";
    CHECK(m.text.size() <= kNoticeMaxBytes && m.text.substr(m.text.size() - again.size()) == again);
    m = ClassHostBreaks("strength is past the limit");
    CHECK(m.kind == NoticeKind::Warn && m.key == "class-host");
    CHECK(m.text == "The host breaks the class rules: strength is past the limit. You leave in a minute unless it's fixed.");
    m = ClassHostBreaks(std::string(400, 'p'));
    const std::string leave = "You leave in a minute unless it's fixed.";
    CHECK(m.text.size() <= kNoticeMaxBytes && m.text.substr(m.text.size() - leave.size()) == leave);
    m = ClassHostLeft("strength is past the limit");
    CHECK(m.kind == NoticeKind::Error && m.title == "LOBBY" && m.key == "class-host");
    CHECK(m.text == "You left the lobby: the host breaks the class rules: strength is past the limit.");
    CHECK(ClassHostLeft(std::string(400, 'p')).text.size() <= kNoticeMaxBytes);
    m = ClassPlayerBreaks("Ann", "the class needs Half Wolverine");
    CHECK(m.kind == NoticeKind::Warn && m.key == "class-of:Ann" && m.text == "Ann breaks the class rules: the class needs Half Wolverine.");
    m = ClassPlayerRemoved("Ann", "the class needs Half Wolverine");
    CHECK(m.key == "class-of:Ann" && m.text == "Ann was removed: the class needs Half Wolverine.");   // settles the warning above
    CHECK(ClassPlayerBreaks(std::string(300, 'n'), "x").text.size() <= kNoticeMaxBytes);
    m = NoClassCard("");
    CHECK(m.text == "A player isn't sending a class card." && m.kind == NoticeKind::Warn);
    CHECK(NoClassCard("Bob").text == "Bob isn't sending a class card.");
    m = StatsPastLimits("Bob", "run +90%, strength +60%");
    CHECK(m.kind == NoticeKind::Warn && m.text == "Bob's stats are past the limits: run +90%, strength +60%");
    m = StatsPastLimits("Bob", std::string(400, 's'));
    CHECK(m.text.size() <= kNoticeMaxBytes && Has(m.text, "Bob's stats are past the limits: "));
}

static void TestOtherBuilders() {
    Msg m = Presence("Ann", true);
    CHECK(m.kind == NoticeKind::Info && m.title == "LOBBY" && m.text == "Ann joined." && m.key.empty());
    CHECK(Presence("Ann", false).text == "Ann left." && Presence("", true).text == "? joined.");
    m = NewHost("Bob");
    CHECK(m.text == "The host left. Bob is the new host." && m.key == "migration" && m.key == std::string(Of(Id::HostGone).key));
    CHECK(Presence(std::string(300, 'a'), true).text.size() <= kNoticeMaxBytes);
    m = FrameRate(24.4, 30);
    CHECK(m.kind == NoticeKind::Warn && m.title == "FRAME RATE" && m.key == "frame-rate");
    CHECK(m.text == "Your frame rate is 24 FPS, under 30. The others see your fighter in bigger steps.");
    m = ChaosStats("run +10%, strength -5%");
    CHECK(m.title == "CHAOS" && m.text == "Your stats this round: run +10%, strength -5%");
    m = RuleChanged("Host changed mods: Classes");
    CHECK(m.key == "rule-changed" && m.text == "Host changed mods: Classes" && m.title == "LOBBY");
    CHECK(RuleChanged(std::string(500, 'm')).text.size() <= kNoticeMaxBytes);
    m = StartWhy("Duel is for 2 players. Pick another mode.");
    CHECK(m.kind == NoticeKind::Warn && m.title == "LOBBY" && m.key == "start-why" && m.text == "Duel is for 2 players. Pick another mode.");
    CHECK(StartWhy("Only the host changes this.").key == StartWhy("Checking the map pack.").key);   // a second click replaces the first
    CHECK(StartWhy(std::string(300, 'w')).text.size() <= kNoticeMaxBytes);
    m = Plain(NoticeKind::Info, "UPDATE", "HSMMod 0.8.1 is out.", "update");
    CHECK(m.text == "HSMMod 0.8.1 is out." && m.key == "update");
    CHECK(Plain(NoticeKind::Info, "X", "a\nb\tc").text == "a b c");
}

static void TestReportBuilders() {
    const std::string good = "{\"id\":\"Ab3dEf7hJk\",\"issue\":\"https://github.com/Dudiebug/halfsword-multiplayer-mod/issues/7\",\"token\":\"tok_abcdefghijklmnop-123\"}";
    using T = ReportTransport;
    auto verdict = [&](ReportJob j, T t, bool sent, unsigned st, const std::string& b) { return JudgeReportAnswer(j, t, sent, st, b); };
    // Filed: Done, no link (the page's note keeps the full line with it), the other players asked when shared.
    ReportVerdict v = verdict(ReportJob::Report, T::Ok, true, 201, good);
    Msg m = ReportResult(ReportJob::Report, v, false);
    CHECK(m.kind == NoticeKind::Done && m.title == "REPORTS" && m.key == "report-result");
    CHECK(m.text == "Bug report filed. Thanks!");
    CHECK(!Has(m.text, "http") && !Has(m.text, "github.com") && Has(ReportOutcomeLine(ReportJob::Report, v), "https://github.com/"));
    CHECK(ReportResult(ReportJob::Report, v, true).text == "Bug report filed. Thanks! The other players were asked to add their logs.");
    CHECK(ReportResult(ReportJob::Crash, v, false).text == "Crash report filed. Thanks!");
    CHECK(ReportResult(ReportJob::Log, verdict(ReportJob::Log, T::Ok, true, 200, "{}"), false).text == "Your log was added to the bug report. Thanks!");
    // Not understood: it may have been filed (a warning, not "filed").
    v = verdict(ReportJob::Report, T::Ok, true, 201, "{}");
    m = ReportResult(ReportJob::Report, v, false);
    CHECK(m.kind == NoticeKind::Warn && Has(m.text, "may have been filed") && Has(m.text, "Check the mod's GitHub issues before reporting again."));
    CHECK(!Has(m.text, "report filed") && !Has(m.text, "Thanks"));
    CHECK(Has(ReportResult(ReportJob::Crash, v, false).text, "crash report may have been filed"));
    m = ReportResult(ReportJob::Log, verdict(ReportJob::Log, T::Timeout, true, 0, ""), false);
    CHECK(m.kind == NoticeKind::Warn && m.text == "No answer from the report server: your log may have been added.");
    // Not sent and refused: an error with the reason.
    m = ReportResult(ReportJob::Report, verdict(ReportJob::Report, T::NoConnection, false, 0, ""), false);
    CHECK(m.kind == NoticeKind::Error && m.text == "Bug report not sent: no internet connection.");
    m = ReportResult(ReportJob::Log, verdict(ReportJob::Log, T::NoConnection, false, 0, ""), false);
    CHECK(m.kind == NoticeKind::Error && m.text == "Your log was not sent: no internet connection.");
    m = ReportResult(ReportJob::Report, verdict(ReportJob::Report, T::Ok, true, 429, "{\"error\":\"too many reports from you, try again later\"}"), false);
    CHECK(m.kind == NoticeKind::Error && m.text == "Bug report not filed: too many reports from you, try again later.");
    m = ReportResult(ReportJob::Log, verdict(ReportJob::Log, T::Ok, true, 500, "{}"), false);
    CHECK(m.kind == NoticeKind::Error && m.text == "Your log was not added: the report server answered 500.");
    // Every job and outcome fits the cap and is never a "filed" line unless it was.
    for (ReportJob j : {ReportJob::Report, ReportJob::Crash, ReportJob::Log})
        for (T t : {T::Ok, T::Unavailable, T::NoConnection, T::Timeout, T::Certificate, T::Unreachable, T::Other})
            for (bool sent : {false, true})
                for (unsigned st : {0u, 200u, 201u, 204u, 413u, 429u, 502u, 504u}) {
                    const ReportVerdict r = verdict(j, t, sent, st, "{}");
                    const Msg x = ReportResult(j, r, true);
                    CHECK(!x.text.empty() && x.text.size() <= kNoticeMaxBytes && Valid8(x.text));
                    if (r.outcome != ReportOutcome::Done) CHECK(!Has(x.text, "report filed") && !Has(x.text, "Thanks") && !Has(x.text, "was added to"));
                    CHECK(x.kind == (r.outcome == ReportOutcome::Done ? NoticeKind::Done : r.outcome == ReportOutcome::MaybeDone ? NoticeKind::Warn : NoticeKind::Error));
                }
    // A reason from the relay that is long still fits.
    ReportVerdict odd; odd.outcome = ReportOutcome::Refused; odd.reason = std::string(200, 'z');
    CHECK(ReportResult(ReportJob::Report, odd, false).text.size() <= kNoticeMaxBytes);
    // Another player's report.
    m = ReportFiled("Ann", true);
    CHECK(m.kind == NoticeKind::Info && m.text == "Ann filed a bug report." && m.key == "report-offer");
    m = ReportFiled("Ann", false);
    CHECK(m.kind == NoticeKind::Warn && m.text == "Ann filed a bug report. Type /sendlog to add your log, or /nolog.");
    CHECK(ReportFiled("", false).text.rfind("A player filed a bug report.", 0) == 0);
    CHECK(ReportFiled(std::string(300, 'a'), false).text.size() <= kNoticeMaxBytes);
    const std::string ask = "Type /sendlog to add your log, or /nolog.", asked = ReportFiled(std::string(300, 'a'), false).text;
    CHECK(asked.substr(asked.size() - ask.size()) == ask);
}

// The pause menu and the chat (UI overhaul PR5): what a dimmed button says, the chat key with the menu open, the photo key
// that did nothing, and the answers CHANGE LOADOUT and REPORT A BUG give.
static void TestPauseAndChatNotices() {
    CHECK(std::string(Of(Id::ReportsOff).text) == "Bug reports are switched off on this PC. Switch them on in the mod's settings, then restart.");
    CHECK(std::string(Of(Id::ReportsOff).title) == "REPORTS" && std::string(Of(Id::ReportsOff).key) == "pause-why");
    CHECK(std::string(Of(Id::LobbyNotInCrypts).text) == "Not during a Crypts run. Use LEAVE TO MAIN MENU.");
    CHECK(std::string(Of(Id::LobbyNotInCrypts).title) == "LOBBY" && std::string(Of(Id::LobbyNotInCrypts).key) == "pause-why");
    CHECK(Of(Id::LobbyNotInCrypts).kind == NoticeKind::Info && !Of(Id::LobbyNotInCrypts).longer);
    // BACK TO LOBBY for a guest too (session/hsm_bench.h): the other two reasons a click can be dimmed, in the same voice.
    CHECK(std::string(Of(Id::LobbyNone).text) == "There is no lobby to go back to. Use LEAVE TO MAIN MENU.");
    CHECK(std::string(Of(Id::LobbyNotInProgression).text) == "Not during a Progression fight. Use LEAVE TO MAIN MENU.");
    for (Id id : {Id::LobbyNone, Id::LobbyNotInProgression}) {
        CHECK(std::string(Of(id).title) == "LOBBY" && std::string(Of(id).key) == "pause-why" && Of(id).kind == NoticeKind::Info && !Of(id).longer);
    }
    CHECK(std::string(Of(Id::ChatPaused).text) == "Close the menu to chat." && std::string(Of(Id::ChatPaused).title) == "CHAT");
    CHECK(std::string(Of(Id::ChatPaused).key) == "chat-paused" && Of(Id::ChatPaused).kind == NoticeKind::Info);   // pressed again, it replaces itself
    CHECK(std::string(Of(Id::PhotoNoMatch).text) == "Photo mode works during a match.");
    CHECK(std::string(Of(Id::PhotoNotAllowed).text) == "The host turned free cameras off for this match, photo mode with them.");
    CHECK(std::string(Of(Id::PhotoNotReady).text) == "Photo mode could not start here.");
    CHECK(std::string(Of(Id::PhotoDead).text) == "Photo mode needs a living fighter.");
    for (Id id : {Id::PhotoNoMatch, Id::PhotoNotAllowed, Id::PhotoNotReady, Id::PhotoDead}) {
        CHECK(std::string(Of(id).title) == "PHOTO" && std::string(Of(id).key) == "photo-key");   // the next refusal replaces the last
        CHECK(Of(id).kind == NoticeKind::Info && !Of(id).longer);
    }
    // Two dimmed buttons share one key, so a second click replaces the first answer.
    CHECK(std::string(Of(Id::ReportsOff).key) == std::string(Of(Id::LobbyNotInCrypts).key));
    CHECK(std::string(Of(Id::LobbyNone).key) == std::string(Of(Id::LobbyNotInCrypts).key) && std::string(Of(Id::LobbyNotInProgression).key) == std::string(Of(Id::LobbyNotInCrypts).key));
    // A sentence out of a rule's line: a full stop where it has none, nothing doubled, nothing left of a trailing blank.
    CHECK(Sentence("The host deals everyone's kit") == "The host deals everyone's kit.");
    CHECK(Sentence("Not during the fight.") == "Not during the fight." && Sentence("Really?") == "Really?" && Sentence("Done!") == "Done!");
    CHECK(Sentence("Wait  \t") == "Wait." && Sentence("").empty() && Sentence("   ").empty());
    // CHANGE LOADOUT: the answer replaces "Sending your loadout...", a dimmed click shows the rule.
    Msg m = LoadoutAnswer(NoticeKind::Done, "Loadout changed: you come back in it");
    CHECK(m.kind == NoticeKind::Done && m.title == "LOADOUT" && m.key == "loadout" && m.text == "Loadout changed: you come back in it.");
    CHECK(LoadoutAnswer(NoticeKind::Error, "x").kind == NoticeKind::Error);
    CHECK(LoadoutAnswer(NoticeKind::Info, std::string(300, 'w')).text.size() <= kNoticeMaxBytes);
    m = LoadoutWhy("The host deals everyone's kit");
    CHECK(m.kind == NoticeKind::Info && m.title == "LOADOUT" && m.key == "pause-why" && m.text == "The host deals everyone's kit.");
    CHECK(LoadoutWhy(std::string(300, 'w')).text.size() <= kNoticeMaxBytes && LoadoutWhy("a").key == std::string(Of(Id::ReportsOff).key));
    // REPORT A BUG when the chat never opened: where to type the report instead, with the chat key's own name.
    m = BugChatTimeout("T");
    CHECK(m.kind == NoticeKind::Info && m.title == "REPORTS" && m.key == "bug-hint");
    CHECK(m.text == "The chat did not open. Press T, then type /bug and what happened.");
    CHECK(BugChatTimeout("").text == "The chat did not open. Press Enter, then type /bug and what happened.");
    CHECK(m.key == std::string(Of(Id::BugHint).key));                              // it replaces the standing bug hint
    CHECK(BugChatTimeout(std::string(300, 'k')).text.size() <= kNoticeMaxBytes);
    // The host hears of a guest's BACK TO LOBBY: keyed by the name, so the same player clicking twice is one notice.
    m = WentBackToLobby("Ann");
    CHECK(m.kind == NoticeKind::Info && m.title == "LOBBY" && m.text == "Ann went back to the lobby." && m.key == "back-to-lobby:Ann");
    CHECK(WentBackToLobby("Bo").key != m.key && WentBackToLobby("").text == "A player went back to the lobby.");
    CHECK(WentBackToLobby(std::string(300, 'n')).text.size() <= kNoticeMaxBytes);
}

int main() {
    TestCatalogue();
    TestApprovedText();
    TestClassBuilders();
    TestOtherBuilders();
    TestReportBuilders();
    TestPauseAndChatNotices();
    if (g_fail) { std::printf("%d notice text checks failed\n", g_fail); return 1; }
    std::printf("all notice text tests passed\n");
    return 0;
}

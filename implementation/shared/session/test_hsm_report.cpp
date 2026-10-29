// Unit tests for hsm_report.h. Run via implementation/shared/run_tests.sh test_hsm_report.
#include "session/hsm_report.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestCommands() {
    ReportCommand c = ParseReportCommand("/report  the flail floats  ");
    CHECK(c.kind == ReportCommandKind::Report && c.text == "the flail floats");
    CHECK(ParseReportCommand("/REPORT").kind == ReportCommandKind::Report && ParseReportCommand("/report").text.empty());
    CHECK(ParseReportCommand("/bug grab broke").kind == ReportCommandKind::Report);
    CHECK(ParseReportCommand("/sendlog").kind == ReportCommandKind::SendLog);
    CHECK(ParseReportCommand("/SendLog").kind == ReportCommandKind::SendLog);
    CHECK(ParseReportCommand("/nolog").kind == ReportCommandKind::NoLog);
    // chat, and near misses, stay chat
    for (const char* s : {"", "report this", "hi /report", "/reports", "/sendlog please", "/", "/nologs"})
        CHECK(ParseReportCommand(s).kind == ReportCommandKind::None);
}

static void TestRedaction() {
    const uint64_t salt = 0x1234;
    const std::string id = "76561198012345678";
    const std::string in = "steam: I am 76561198012345678 (Dudie); peer 76561198012345678 joined\n";
    const std::string out = RedactLog(in, salt);
    CHECK(out.find(id) == std::string::npos);
    const std::string tag = "steam:" + RedactHash(id, salt);
    CHECK(out.find(tag) != std::string::npos && out.find(tag) != out.rfind(tag));   // the same id reads the same
    CHECK(out.find("(Dudie)") != std::string::npos);                                 // names stay
    CHECK(RedactHash(id, salt) != RedactHash(id, salt + 1));                        // a new salt per report
    // not a SteamID: other lengths, other prefixes, inside a word
    for (const char* keep : {"7656119801234567", "1234567890123456", "1234567890123456789", "x76561198012345678", "76561198012345678x"})
        CHECK(RedactLog(keep, salt) == keep);
    // R7: a Steam lobby id (18 digits) and any other 17-18 digit id (a group) go too
    const std::string lobby = RedactLog("lobby 109775241234567890 created, group=12345678901234567.", salt);
    CHECK(lobby.find("109775241234567890") == std::string::npos && lobby.find("12345678901234567") == std::string::npos);
    CHECK(lobby.find("steamid:" + RedactHash("109775241234567890", salt)) != std::string::npos);
    CHECK(lobby.find("created, group=steamid:") != std::string::npos);

    // IPv6 (0.8.0-pre7): full, compressed, bracketed with a port, with a zone, with an IPv4 tail; loopback stays
    {
        const std::string v6 = RedactLog("peer 2001:db8:85a3:0:0:8a2e:370:7334 and [2001:db8::1]:27015, fe80::1ff:fe23:4567:890a%eth0; "
                                         "map ::ffff:10.0.0.5 up", salt);
        for (const char* gone : {"2001:db8:85a3", "2001:db8::1", "fe80::", "10.0.0.5", "ffff"}) CHECK(v6.find(gone) == std::string::npos);
        CHECK(v6.find("[ip6:" + RedactHash("2001:db8::1", salt) + "]:27015") != std::string::npos);
        CHECK(v6.find("ip6:" + RedactHash("2001:db8:85a3:0:0:8a2e:370:7334", salt)) != std::string::npos);
        CHECK(v6.find("eth0") == std::string::npos);
        for (const char* keep : {"bind ::1 ok", "any ::", "[12:34:56.789]", "hsm::game::Adapter", "std::abs", "a:b:c", "dead:beef",
                                 "mac aa:bb:cc:dd:ee:ff", "0x1028 / 0x0E68", "ratio 1:2", "add::x", "Cafe::Bean"})
            CHECK(RedactLog(keep, salt) == keep);
        CHECK(RedactLog("addr 2001:db8::7: failed", salt) == "addr ip6:" + RedactHash("2001:db8::7", salt) + ": failed");
    }
    const std::string ip = RedactLog("config: role=client host=192.168.1.254 port=27777; net: 10.0.0.5:27777 up", salt);
    CHECK(ip.find("192.168.1.254") == std::string::npos && ip.find("10.0.0.5") == std::string::npos);
    CHECK(ip.find("ip:" + RedactHash("10.0.0.5", salt) + ":27777") != std::string::npos);
    CHECK(RedactLog("host=127.0.0.1 bind 0.0.0.0", salt) == "host=127.0.0.1 bind 0.0.0.0");
    for (const char* keep : {"v0.6.5", "1.2.3", "[12:34:56.789]", "256.1.1.1", "1.2.3.4.5", "pos 1.5,2.25", "a1.2.3.4"})
        CHECK(RedactLog(keep, salt) == keep);

    CHECK(RedactLog("C:\\Users\\devadmin\\AppData\\Local\\HalfswordUE5\\Saved", salt) == "C:\\Users\\<user>\\AppData\\Local\\HalfswordUE5\\Saved");
    CHECK(RedactLog("/home/dudie/.steam/x and /var/home/dudie/y", salt) == "/home/<user>/.steam/x and /var/home/<user>/y");
    CHECK(RedactLog("Z:/Users/Bob Smith/Documents", salt) == "Z:/Users/<user>/Documents");
    CHECK(RedactLog("Steam Users said hi; home/run", salt) == "Steam Users said hi; home/run");   // no folder after it
    CHECK(RedactLog("", salt).empty());

    // R7: Windows paths in any case, the escaped form of a JSON-ish line, a folder that ends the line or the string
    CHECK(RedactLog("c:\\users\\Bob\\AppData", salt) == "c:\\users\\<user>\\AppData");
    CHECK(RedactLog("C:\\USERS\\BOB\\x", salt) == "C:\\USERS\\<user>\\x");
    CHECK(RedactLog("{\"path\":\"C:\\\\Users\\\\bob\\\\AppData\\\\Local\"}", salt) == "{\"path\":\"C:\\\\Users\\\\<user>\\\\AppData\\\\Local\"}");
    CHECK(RedactLog("profile C:\\Users\\bob\nnext", salt) == "profile C:\\Users\\<user>\nnext");
    CHECK(RedactLog("\"C:\\Users\\bob\"", salt) == "\"C:\\Users\\<user>\"");
    CHECK(RedactLog("C:\\Users\\Public", salt) == "C:\\Users\\<user>");                 // folder names are not told apart
    CHECK(RedactLog("MyUsers\\bob\\x and Users \\x", salt) == "MyUsers\\bob\\x and Users \\x");
    // R7: host names after host=; addresses keep their own rule, loopback and an empty value stay
    const std::string host = RedactLog("config: role=client host=DESKTOP-BOB.example.net port=27777 host=mybox", salt);
    CHECK(host.find("DESKTOP-BOB") == std::string::npos && host.find("mybox") == std::string::npos);
    CHECK(host.find("host=host:" + RedactHash("DESKTOP-BOB.example.net", salt) + " port=27777") != std::string::npos);
    CHECK(RedactLog("HOST=Bob-PC,", salt) == "HOST=host:" + RedactHash("Bob-PC", salt) + ",");
    CHECK(RedactLog("host=localhost host= port=1 ghost=bob", salt) == "host=localhost host= port=1 ghost=bob");
    CHECK(RedactLog("host=10.0.0.5:27777", salt) == "host=ip:" + RedactHash("10.0.0.5", salt) + ":27777");
}

static void TestTail() {
    CHECK(LogTail("short", 100) == "short");
    const std::string log = "line one\nline two\nline three\n";
    const std::string t = LogTail(log, 14);
    CHECK(t.find("line three\n") != std::string::npos && t.find("line two") == std::string::npos && t.rfind("[... ", 0) == 0);
    CHECK(LogTail("ab\ncd\n", 100, 10) == "[... 13 earlier bytes not sent]\ncd\n");   // read from mid-file: the cut line goes
    CHECK(LogTail(std::string(50, 'x'), 10).find(std::string(10, 'x')) != std::string::npos);   // no newline: the raw tail
}

static void TestJson() {
    CHECK(JsonEscape("a\"b\\c\nd\te\x01") == "a\\\"b\\\\c\\nd\\te\\u0001");
    CHECK(ValidUtf8("ok \xC3\xA9 \xFF end") == "ok \xC3\xA9 ? end");
    ReportFacts f; f.modBuild = "0.6.6 (abc)"; f.protocol = 18; f.players = 2; f.role = "host"; f.level = "/Game/Maps/Arena\"x";
    const std::string j = ReportJson("grab \x01" "broke", f, {{"HSMMod.log", "a\nb"}});
    CHECK(j.find("\"description\":\"grab broke\"") != std::string::npos);            // control characters dropped
    CHECK(j.find("\"protocol\":18") != std::string::npos && j.find("\"players\":2") != std::string::npos);
    CHECK(j.find("Arena\\\"x") != std::string::npos);
    CHECK(j.find("{\"name\":\"HSMMod.log\",\"text\":\"a\\nb\"}") != std::string::npos);
    CHECK(AttachJson("tok", f, {}).find("\"token\":\"tok\"") != std::string::npos && AttachJson("tok", f, {}).find("\"logs\":[]") != std::string::npos);

    const std::string ans = R"({"id":"r7Kq2","issue":"https://github.com/a/b/issues/12","token":"Ab3_x-9QwertyUiop12","note":"a \"q\""})";
    CHECK(JsonStringField(ans, "id") == "r7Kq2" && JsonStringField(ans, "token") == "Ab3_x-9QwertyUiop12");
    CHECK(JsonStringField(ans, "note") == "a \"q\"" && JsonStringField(ans, "missing").empty());
    CHECK(JsonStringField(R"({"id": 5})", "id").empty() && JsonStringField(R"({"id":"unterminated)", "id").empty());
}

static void TestIdsAndShare() {
    CHECK(ValidReportId("r7Kq2") && !ValidReportId("") && !ValidReportId("a-b") && !ValidReportId(std::string(33, 'a')));
    CHECK(ValidReportToken("Ab3_x-9QwertyUiop12") && !ValidReportToken("short") && !ValidReportToken(std::string(20, 'a') + "/"));
    CHECK(ValidIssueUrl("https://github.com/Dudiebug/halfsword-multiplayer-mod/issues/12"));
    for (const char* bad : {"http://github.com/a/b/issues/1", "https://github.com.evil.io/a/issues/1", "https://github.com/a/b/pull/1",
                            "https://github.com/a/b/issues/1?x=<script>", "javascript:alert(1)"})
        CHECK(!ValidIssueUrl(bad));

    ReportShareMsg m; m.fromSlot = 2; m.id = "r7Kq2"; m.token = "Ab3_x-9QwertyUiop12";
    std::vector<uint8_t> b = Encode(m);
    ReportShareMsg back; MsgType t;
    CHECK(Decode(b.data(), b.size(), back) && back.fromSlot == 2 && back.id == m.id && back.token == m.token);
    CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::ReportShare);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, back));
    std::vector<uint8_t> extra = b; extra.push_back(0); CHECK(!Decode(extra.data(), extra.size(), back));
    ReportShareMsg badId = m; badId.id = "../x"; std::vector<uint8_t> bb = Encode(badId); CHECK(!Decode(bb.data(), bb.size(), back));
    ReportShareMsg badTok = m; badTok.token = "tiny"; bb = Encode(badTok); CHECK(!Decode(bb.data(), bb.size(), back));
    ReportShareMsg badSlot = m; badSlot.fromSlot = uint8_t(kMaxWillies); bb = Encode(badSlot); CHECK(!Decode(bb.data(), bb.size(), back));
}

static void TestOffer() {
    ReportOffer o; std::string id, tok;
    CHECK(!o.Open(0) && !o.Take(0, id, tok));
    o.Offer("r1", "Ab3_x-9QwertyUiop12", 1000);
    CHECK(o.Open(1000 + kReportOfferMs - 1) && !o.Open(1000 + kReportOfferMs));
    CHECK(o.Take(5000, id, tok) && id == "r1" && tok == "Ab3_x-9QwertyUiop12");
    CHECK(!o.Take(5001, id, tok));                                          // answered once
    o.Offer("r2", "Ab3_x-9QwertyUiop12", 0xFFFFFF00u);                      // across the 32-bit tick wrap
    CHECK(o.Open(0x10u) && o.Take(0x10u, id, tok) && id == "r2");
    o.Offer("r3", "Ab3_x-9QwertyUiop12", 0); o.Decline(); CHECK(!o.Take(1, id, tok));
    o.Offer("r4", "Ab3_x-9QwertyUiop12", 0); CHECK(!o.Take(kReportOfferMs, id, tok) && !o.Open(0));   // expired: closed
}

static void TestCrashLine() {
    CHECK(CrashLineOf("").empty() && CrashLineOf("hsm: server ready\nmatch: live\n").empty());
    const std::string log = "a\ncrash: fault 0xC0000005 at HalfSwordUE5-Win64-Shipping.exe+0x1A2B reading 0x0 (thread 7); the game may close now\r\n"
                            "crash: stack code addresses: dwmapi.dll+0x10\n";
    CHECK(CrashLineOf(log) == "crash: fault 0xC0000005 at HalfSwordUE5-Win64-Shipping.exe+0x1A2B reading 0x0 (thread 7); the game may close now");
    CHECK(CrashLineOf("crash: fault 1\ncrash: fault 2") == "crash: fault 2");                 // the last one
    std::string longLine = "crash: fault " + std::string(500, 'x');
    CHECK(CrashLineOf(longLine).size() == kCrashLineMax);
    CHECK(CrashLineOf(std::string("crash: fault \x01\x7f\xc3\xa9" "ok")) == "crash: fault ok");   // control and non-ASCII bytes dropped
    // a fault the game played on past, then a normal quit: not a crash; without the quit it is one (the engine's crash
    // reporter ends the game seconds after the fault: public #14), and so is a later fault that ended the session
    const std::string survived =
        "[02:30:18.947][10664] crash: fault 0xC0000005 at G.exe+0x4A33C5A reading 0x4 (thread 10664); the game may close now\n"
        "[02:30:28.950][3308] crash: the game still runs 10 s after the fault on thread 10664\n"
        "[02:31:10.750][3308] session: pause menu open; cursor shown\n";
    const std::string quit = "[02:31:20.001][3308] exit: the game closed normally\r\n";
    CHECK(std::string(kCleanExitLine) == "exit: the game closed normally");
    CHECK(CrashLineOf(survived + quit).empty());
    CHECK(CrashLineOf(survived) == "crash: fault 0xC0000005 at G.exe+0x4A33C5A reading 0x4 (thread 10664); the game may close now");
    CHECK(CrashLineOf(survived + "[02:31:13.294][21600] crash: fault 0xC0000005 at ntdll.dll+0x304D4 writing 0xE0 (thread 21600)\n")
          == "crash: fault 0xC0000005 at ntdll.dll+0x304D4 writing 0xE0 (thread 21600)");
    CHECK(CrashLineOf(quit + "crash: fault 9 (thread 1)") == "crash: fault 9 (thread 1)");   // a crash on the way out (T6)
    CHECK(CrashLineOf("[1][2] crash: fault 9\n[3][4] chat: exit: the game closed normally\n") == "crash: fault 9");   // quoted
    // the next start's own log quotes the crash line it reported: that start did not crash (public #7)
    const std::string quoted =
        "[02:31:24.836][12936] report: the last session crashed; sending a crash report (help_improve is on): crash: fault "
        "0xC0000005 at ?+0x7FFC4FC41800 executing 0x7FFC4FC41800 (thread 3308); the game may close now\n"
        "[02:31:25.010][12936] report: the last session crashed (crash: fault 0xC0000005 at ?+0x1 (thread 1)); help_improve is off\n"
        "[02:35:03.066][12936] steam: could not reach the Steam servers (result 3)\n";
    CHECK(CrashLineOf(quoted).empty());
    CHECK(CrashLineOf("[1][2] crash: fault 7 (thread 2)\n" + quoted) == "crash: fault 7 (thread 2)");   // the real one before them
    CHECK(LastLogLineAt("crash: fault 1", "crash: fault ") == 0);
    CHECK(LastLogLineAt("x crash: fault 1", "crash: fault ") == std::string::npos);
    CHECK(LastLogLineAt("a]crash: fault 1", "crash: fault ") == std::string::npos);
    CHECK(CrashReportText("crash: fault 1") == "Crash (sent automatically): crash: fault 1");
}

static void TestPendingShare() {
    PendingShare p; std::string id, tok;
    CHECK(!p.Held(0) && !p.Take(0, id, tok));
    p.Hold("r1", "Ab3_x-9QwertyUiop12", 1000);
    CHECK(p.Held(1000 + kPendingShareMs - 1) && !p.Held(1000 + kPendingShareMs));
    CHECK(p.Take(2000, id, tok) && id == "r1" && tok == "Ab3_x-9QwertyUiop12" && !p.Held(2000) && !p.Take(2001, id, tok));
    p.Hold("r2", "Ab3_x-9QwertyUiop12", 0xFFFFFF00u);                      // across the 32-bit tick wrap
    CHECK(p.Take(0x10u, id, tok) && id == "r2");
    p.Hold("r3", "Ab3_x-9QwertyUiop12", 0); CHECK(!p.Take(kPendingShareMs, id, tok) && !p.Held(0));   // expired: gone
}

// The outcome line for every way a request can end: "filed" only for a real filing, "may have been filed" once the
// request was out, "not sent" only when it never got out.
static void TestOutcomes() {
    using T = ReportTransport;
    using O = ReportOutcome;
    const std::string good = "{\"id\":\"Ab3dEf7hJk\",\"issue\":\"https://github.com/Dudiebug/halfsword-multiplayer-mod/issues/7\",\"token\":\"tok_abcdefghijklmnop-123\"}";
    auto line = [](ReportJob j, T t, bool sent, unsigned st, const std::string& b) { return ReportOutcomeLine(j, JudgeReportAnswer(j, t, sent, st, b)); };
    auto has = [](const std::string& s, const char* part) { return s.find(part) != std::string::npos; };

    // 201 with a valid id, token and issue URL: filed, with the link (200 counts too).
    ReportVerdict v = JudgeReportAnswer(ReportJob::Report, T::Ok, true, 201, good);
    CHECK(v.outcome == O::Done && v.id == "Ab3dEf7hJk" && v.token == "tok_abcdefghijklmnop-123" && has(v.issue, "/issues/7"));
    CHECK(ReportOutcomeLine(ReportJob::Report, v) == "Bug report filed: https://github.com/Dudiebug/halfsword-multiplayer-mod/issues/7 Thanks!");
    CHECK(has(ReportOutcomeLine(ReportJob::Report, v, true), "The other players were asked to add their logs."));
    CHECK(JudgeReportAnswer(ReportJob::Report, T::Ok, true, 200, good).outcome == O::Done);
    CHECK(has(line(ReportJob::Crash, T::Ok, true, 201, good), "Crash report filed: https://"));

    // 201 with a bad answer: never "filed" (it may have been), and no id or token kept to share.
    for (const char* bad : {"", "{}", "not json", "{\"id\":\"Ab3dEf7hJk\",\"token\":\"tok_abcdefghijklmnop-123\"}",                  // no issue
                            "{\"id\":\"Ab3dEf7hJk\",\"issue\":\"https://evil.example/issues/7\",\"token\":\"tok_abcdefghijklmnop-123\"}",
                            "{\"id\":\"../x\",\"issue\":\"https://github.com/a/b/issues/1\",\"token\":\"tok_abcdefghijklmnop-123\"}",
                            "{\"id\":\"Ab3dEf7hJk\",\"issue\":\"https://github.com/a/b/issues/1\",\"token\":\"short\"}"}) {
        ReportVerdict b = JudgeReportAnswer(ReportJob::Report, T::Ok, true, 201, bad);
        CHECK(b.outcome == O::MaybeDone && b.id.empty() && b.token.empty());
        const std::string l = ReportOutcomeLine(ReportJob::Report, b);
        CHECK(!has(l, "report filed") && has(l, "may have been filed") && has(l, "before reporting again"));
    }

    // Refusals: the relay said no, nothing was filed.
    CHECK(line(ReportJob::Report, T::Ok, true, 429, "{\"error\":\"too many reports from you, try again later\"}") ==
          "Bug report not filed: too many reports from you, try again later.");                 // the relay's own words
    CHECK(line(ReportJob::Report, T::Ok, true, 429, "") ==
          "Bug report not filed: too many reports right now, try again in a few minutes.");    // none: the generic line
    CHECK(line(ReportJob::Report, T::Ok, true, 429, "<html>Too Many Requests</html>") ==
          "Bug report not filed: too many reports right now, try again in a few minutes.");
    CHECK(line(ReportJob::Report, T::Ok, true, 413, "") == "Bug report not filed: the report is too large.");
    CHECK(line(ReportJob::Report, T::Ok, true, 502, "{\"error\":\"the report could not be filed, try again later\"}") ==
          "Bug report not filed: the report could not be filed, try again later.");
    CHECK(line(ReportJob::Report, T::Ok, true, 502, "<html>Bad Gateway</html>") == "Bug report not filed: the report server answered 502.");
    CHECK(line(ReportJob::Report, T::Ok, true, 400, "{\"error\":\"empty description\"}") == "Bug report not filed: empty description.");
    CHECK(JudgeReportAnswer(ReportJob::Report, T::Ok, true, 503, "").outcome == O::Refused);

    // The request went out and no answer came: it may have been filed (a timeout, a broken connection, a proxy's 504).
    CHECK(line(ReportJob::Report, T::Timeout, true, 0, "") ==
          "No answer from the report server: your bug report may have been filed. Check the mod's GitHub issues before reporting again.");
    CHECK(JudgeReportAnswer(ReportJob::Report, T::Other, true, 0, "").outcome == O::MaybeDone);
    CHECK(JudgeReportAnswer(ReportJob::Report, T::Ok, true, 504, "").outcome == O::MaybeDone);
    CHECK(has(line(ReportJob::Crash, T::Timeout, true, 0, ""), "your crash report may have been filed"));

    // Never got out: not sent.
    CHECK(line(ReportJob::Report, T::Unreachable, false, 0, "") == "Bug report not sent: cannot reach the report server.");
    CHECK(line(ReportJob::Report, T::Certificate, false, 0, "") == "Bug report not sent: the report server's certificate is not valid.");
    CHECK(line(ReportJob::Report, T::Timeout, false, 0, "") == "Bug report not sent: the report server did not answer.");
    CHECK(line(ReportJob::Report, T::NoConnection, false, 0, "") == "Bug report not sent: no internet connection.");
    CHECK(JudgeReportAnswer(ReportJob::Report, T::Unavailable, false, 0, "").outcome == O::NotSent);
    // A certificate or name failure cannot have reached the relay, even if WinHTTP reported the send as begun.
    CHECK(JudgeReportAnswer(ReportJob::Report, T::Certificate, true, 0, "").outcome == O::NotSent);
    CHECK(JudgeReportAnswer(ReportJob::Report, T::Unreachable, true, 0, "").outcome == O::NotSent);

    // /sendlog and help_improve: the log's own wording.
    CHECK(line(ReportJob::Log, T::Ok, true, 200, "{\"ok\":true}") == "Your log was added to the bug report. Thanks!");
    CHECK(line(ReportJob::Log, T::Ok, true, 403, "{\"error\":\"bad token\"}") == "Your log was not added: bad token.");
    CHECK(line(ReportJob::Log, T::Unreachable, false, 0, "") == "Your log was not sent: cannot reach the report server.");
    CHECK(line(ReportJob::Log, T::Timeout, true, 0, "") == "No answer from the report server: your log may have been added.");

    // No outcome line ever claims success unless it was Done.
    for (T t : {T::Ok, T::Unavailable, T::NoConnection, T::Timeout, T::Certificate, T::Unreachable, T::Other})
        for (bool sent : {false, true})
            for (unsigned st : {0u, 200u, 201u, 204u, 400u, 403u, 404u, 413u, 429u, 500u, 502u, 503u, 504u})
                for (ReportJob j : {ReportJob::Report, ReportJob::Crash, ReportJob::Log}) {
                    const ReportVerdict r = JudgeReportAnswer(j, t, sent, st, "{}");
                    const std::string l = ReportOutcomeLine(j, r);
                    CHECK(!l.empty());
                    if (r.outcome != O::Done) CHECK(!has(l, "report filed") && !has(l, "Thanks") && !has(l, "was added to"));
                }
}

int main() {
    TestCommands(); TestRedaction(); TestTail(); TestJson(); TestIdsAndShare(); TestOffer(); TestCrashLine(); TestPendingShare(); TestOutcomes();
    if (g_fail) { std::printf("test_hsm_report: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_report: all passed\n");
    return 0;
}

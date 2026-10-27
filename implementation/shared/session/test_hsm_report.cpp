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
    for (const char* keep : {"7656119801234567", "765611980123456789", "12345678901234567", "x76561198012345678", "76561198012345678x"})
        CHECK(RedactLog(keep, salt) == keep);

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

int main() {
    TestCommands(); TestRedaction(); TestTail(); TestJson(); TestIdsAndShare(); TestOffer();
    if (g_fail) { std::printf("test_hsm_report: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_report: all passed\n");
    return 0;
}

// Unit tests for hsm_moderation.h. Run via implementation/shared/run_tests.sh test_hsm_moderation.
#include "session/hsm_moderation.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestKeys() {
    CHECK(SteamBanKey(76561198012345678ull) == "steam:76561198012345678" && ValidBanKey(SteamBanKey(76561198012345678ull)));
    CHECK(ValidBanKey("ip:10.0.0.5") && ValidBanKey("ip:255.255.255.255") && ValidBanKey("ip:0.0.0.0"));
    for (const char* bad : {"", "steam:", "steam:0", "steam:012", "steam:12a", "steam:18446744073709551616", "steam:123456789012345678901",
                            "ip:", "ip:1.2.3", "ip:1.2.3.4.5", "ip:256.1.1.1", "ip:01.2.3.4", "ip:1..2.3", "ip:1.2.3.4.", "ip:a.b.c.d",
                            "ip:1234.1.1.1", "76561198012345678", "steam:1\n", "ipx:1.2.3.4"})
        CHECK(!ValidBanKey(bad));
    CHECK(ValidBanKey("steam:18446744073709551615"));
    CHECK(BanName("Bob\tSmith\n") == "BobSmith" && BanName(std::string(40, 'x')).size() == kMaxBanName);
    std::string utf = std::string(31, 'a') + "\xC3\xA9";   // the cut would split the e-acute
    CHECK(BanName(utf) == std::string(31, 'a'));
}

static void TestDurations() {
    CHECK(ParseBanDuration("30s") == 30 && ParseBanDuration("10m") == 600 && ParseBanDuration("10") == 600);
    CHECK(ParseBanDuration("2H") == 7200 && ParseBanDuration("7d") == 7 * 86400 && ParseBanDuration("1w") == 7 * 86400);
    CHECK(ParseBanDuration("perm") == 0 && ParseBanDuration("Forever") == 0 && ParseBanDuration("0") == 0);
    CHECK(ParseBanDuration("9999999d") == 0);   // past ten years: permanent
    for (const char* bad : {"", "m", "-5m", "5x", "5mm", "1.5h", "Smith", "0m", "123456789"})
        CHECK(!ParseBanDuration(bad));
    CHECK(DurationText(0) == "permanently" && DurationText(30) == "for 30 s" && DurationText(900) == "for 15 min");
    CHECK(DurationText(7200) == "for 2 h" && DurationText(3 * 86400) == "for 3 days");
}

static void TestBanList() {
    BanList b;
    const int64_t now = 1'800'000'000;
    CHECK(b.Add("steam:111", "Alice", 0, false) && b.Add("ip:10.0.0.5", "Bob", now + 600, false));
    CHECK(!b.Add("nope", "x", 0, false));
    CHECK(b.Find("steam:111", now) && b.Find("steam:111", now + 999999999));          // permanent
    CHECK(b.Find("ip:10.0.0.5", now + 599) && !b.Find("ip:10.0.0.5", now + 600));     // expired at `until`
    CHECK(!b.Find("steam:222", now));

    CHECK(b.Add("steam:222", "Kicked", now + 60, true) && b.Find("steam:222", now)->kick);
    CHECK(b.Add("steam:111", "", now + 5, true) && b.Find("steam:111", now + 10) && !b.Find("steam:111", now)->kick);   // a kick never shortens a ban
    CHECK(b.Add("steam:222", "Kicked", 0, false) && !b.Find("steam:222", now)->kick);  // a ban replaces a kick
    CHECK(b.Bans().size() == 3);

    // Serialize writes bans only; Parse reads them back, keeping kicks and skipping junk and expired lines
    b.Add("steam:333", "K", now + 60, true);
    const std::string file = b.Serialize();
    CHECK(file.find("steam:333") == std::string::npos && file.find("steam:111\t0\tAlice\n") != std::string::npos);
    BanList c;
    c.Add("steam:444", "kick", now + 60, true);
    CHECK(c.Parse(file + "garbage\nsteam:5\tabc\tx\nsteam:6\t" + std::to_string(now - 1) + "\told\nip:1.2.3\t0\tx\r\nsteam:7\t0\r\n", now) == 4);
    CHECK(c.Find("steam:111", now) && c.Find("ip:10.0.0.5", now) && c.Find("steam:222", now) && c.Find("steam:7", now));
    CHECK(c.Find("steam:444", now) && !c.Find("steam:6", now) && !c.Find("steam:5", now));
    CHECK(c.Parse("", now) == 0 && c.Bans().empty() && c.Find("steam:444", now));      // a reload keeps kicks

    // Resolve for /unban
    CHECK(b.Resolve("#1") == "steam:111" && b.Resolve("#2") == "ip:10.0.0.5" && b.Resolve("#9").empty() && b.Resolve("#").empty());
    CHECK(b.Resolve("alice") == "steam:111" && b.Resolve("Bo") == "ip:10.0.0.5" && b.Resolve("10.0.0.5") == "ip:10.0.0.5");
    CHECK(b.Resolve("111") == "steam:111" && b.Resolve("steam:999") == "steam:999" && b.Resolve("zzz").empty());
    b.Add("steam:555", "Alicia", 0, false);
    CHECK(b.Resolve("ali").empty() && b.Resolve("Alice") == "steam:111");   // ambiguous prefix; exact still works

    CHECK(b.Remove("steam:111") && !b.Remove("steam:111") && !b.Find("steam:111", now));
    CHECK(b.Prune(now + 601) && !b.Find("ip:10.0.0.5", now) && !b.Prune(now + 601));

    BanList full;
    for (size_t i = 0; i < kMaxBans; ++i) CHECK(full.Add(SteamBanKey(1000 + i), "", 0, false));
    CHECK(!full.Add("steam:99", "", 0, false) && full.Add(SteamBanKey(1000), "again", 5, false));   // full: replacing still works
}

static void TestCommands() {
    ModCommand c = ParseModCommand("/kick Bob");
    CHECK(c.kind == ModCommandKind::Kick && c.target == "Bob");
    c = ParseModCommand("/BAN  Bob Smith  2h ");
    CHECK(c.kind == ModCommandKind::Ban && c.target == "Bob Smith" && c.seconds == 7200);
    c = ParseModCommand("/ban Bob Smith");
    CHECK(c.kind == ModCommandKind::Ban && c.target == "Bob Smith" && c.seconds == 0);   // no duration: permanent
    c = ParseModCommand("/ban #3 perm");
    CHECK(c.kind == ModCommandKind::Ban && c.target == "#3" && c.seconds == 0);
    c = ParseModCommand("/ban 30m");                                                       // a lone word is the target
    CHECK(c.kind == ModCommandKind::Ban && c.target == "30m");
    CHECK(ParseModCommand("/unban #1").kind == ModCommandKind::Unban && ParseModCommand("/bans").kind == ModCommandKind::Bans);
    CHECK(ParseModCommand("/vk bob").kind == ModCommandKind::VoteKick && ParseModCommand("/votekick #2").target == "#2");
    CHECK(ParseModCommand("/VoteBan bob").kind == ModCommandKind::VoteBan && ParseModCommand("/vb #3").target == "#3");
    CHECK(ParseModCommand("/voteban").kind == ModCommandKind::None);
    CHECK(ParseModCommand("voteban #1", true).kind == ModCommandKind::VoteBan);
    CHECK(ParseModCommand("/yes").kind == ModCommandKind::Yes && ParseModCommand("/N").kind == ModCommandKind::No);
    CHECK(ParseModCommand("/players").kind == ModCommandKind::Players);
    for (const char* chat : {"", "kick bob", "/kick", "/kick   ", "/ban", "/bans x", "/yes please", "/say hi", "/report x", "hello /kick bob"})
        CHECK(ParseModCommand(chat).kind == ModCommandKind::None);
    // the console: no slash needed, "say" works
    CHECK(ParseModCommand("kick bob\r\n", true).kind == ModCommandKind::Kick && ParseModCommand("  bans", true).kind == ModCommandKind::Bans);
    c = ParseModCommand("say  match starts soon ", true);
    CHECK(c.kind == ModCommandKind::Say && c.target == "match starts soon");
    CHECK(ParseModCommand("hello", true).kind == ModCommandKind::None);
}

static void TestTargets() {
    const std::vector<ModPlayer> ps = {{1, "Bob"}, {2, "Bobby"}, {3, "alice"}, {4, "Carl"}, {5, "carl"}};
    uint8_t s = 0;
    CHECK(FindTarget("#3", ps, s) == TargetMatch::Found && s == 3);
    CHECK(FindTarget("#9", ps, s) == TargetMatch::NoMatch && FindTarget("#", ps, s) == TargetMatch::NoMatch && FindTarget("#1x", ps, s) == TargetMatch::NoMatch);
    CHECK(FindTarget("bob", ps, s) == TargetMatch::Found && s == 1);      // exact beats prefix
    CHECK(FindTarget("bobb", ps, s) == TargetMatch::Found && s == 2);
    CHECK(FindTarget("AL", ps, s) == TargetMatch::Found && s == 3);
    CHECK(FindTarget("carl", ps, s) == TargetMatch::Ambiguous);          // two exact matches (case-insensitive)
    CHECK(FindTarget("b", ps, s) == TargetMatch::Ambiguous && FindTarget("z", ps, s) == TargetMatch::NoMatch && FindTarget("", ps, s) == TargetMatch::NoMatch);
}

static void TestVote() {
    VoteKick v;
    const std::vector<uint64_t> four = {10, 20, 30, 40};
    // 4 players, target 40: 3 voters, 2 yes needed
    CHECK(v.Begin(10, 10, four, 0) == VoteKick::Start::Self);
    CHECK(v.Begin(10, 40, {10, 40}, 0) == VoteKick::Start::TooFew);
    CHECK(v.Begin(10, 40, four, 0) == VoteKick::Start::Ok && v.Active() && v.Yes() == 1 && v.Needed() == 2);
    CHECK(v.Begin(20, 30, four, 1) == VoteKick::Start::Running);
    CHECK(v.Tick(100) == VoteKick::Outcome::Running && v.SecondsLeft(100) == 30);
    CHECK(!v.Cast(40, false) && !v.Cast(99, true));                     // the target and strangers have no vote
    CHECK(v.Cast(20, true) && v.Tick(200) == VoteKick::Outcome::Passed && !v.Active() && v.Tick(201) == VoteKick::Outcome::None);

    // cooldown per starter, across the tick wrap
    CHECK(v.Begin(10, 30, four, 1000) == VoteKick::Start::Cooldown);
    CHECK(v.Begin(10, 30, four, kVoteCooldownMs) == VoteKick::Start::Ok);
    CHECK(v.Cast(20, false) && v.Cast(40, false) && v.Tick(kVoteCooldownMs + 1) == VoteKick::Outcome::Failed);   // can't reach 2 of 3

    // timeout
    VoteKick t;
    CHECK(t.Begin(1, 2, {1, 2, 3, 4, 5}, 0xFFFFFF00u) == VoteKick::Start::Ok);
    CHECK(t.Tick(0xFFFFFF00u + kVoteKickMs - 1) == VoteKick::Outcome::Running);
    CHECK(t.Tick(0xFFFFFF00u + kVoteKickMs) == VoteKick::Outcome::Failed);

    // target leaves: cancelled; voters leaving shrink the electorate
    VoteKick l;
    CHECK(l.Begin(1, 2, {1, 2, 3, 4}, 0) == VoteKick::Start::Ok);
    l.Left(2); CHECK(!l.Active() && l.Tick(1) == VoteKick::Outcome::Cancelled && l.Tick(2) == VoteKick::Outcome::None);
    CHECK(l.Begin(3, 2, {1, 2, 3, 4, 5}, 0) == VoteKick::Start::Ok);   // voters 1,3,4,5: needs 3
    CHECK(l.Cast(1, true) && l.Tick(1) == VoteKick::Outcome::Running);
    l.Left(5); CHECK(l.Tick(2) == VoteKick::Outcome::Passed);          // 2 of 3 left is a majority
    CHECK(l.Begin(4, 1, {1, 3, 4}, 0) == VoteKick::Start::Ok);
    l.Left(3); CHECK(l.Tick(1) == VoteKick::Outcome::Cancelled);       // one voter left: too few

    // a voter may change their mind
    VoteKick m;
    CHECK(m.Begin(1, 9, {1, 2, 3, 4, 5, 9}, 0) == VoteKick::Start::Ok);   // 5 voters: needs 3
    CHECK(m.Cast(2, false) && m.Cast(3, false) && m.Tick(1) == VoteKick::Outcome::Running);
    CHECK(m.Cast(2, true) && m.Cast(3, true) && m.Tick(2) == VoteKick::Outcome::Passed);
    // the starter not in the player list still counts (it just joined the list late)
    VoteKick s;
    CHECK(s.Begin(7, 2, {2, 3}, 0) == VoteKick::Start::Ok && s.Needed() == 2);
}

static void TestAdmins() {
    const std::vector<uint64_t> a = ParseAdminList(" 76561198012345678, 76561198000000001;bad 0 76561198012345678");
    CHECK(a.size() == 3 && a[0] == 76561198012345678ull && a[1] == 76561198000000001ull);
    CHECK(ParseAdminList("").empty() && ParseAdminList(",,,").empty());
}

int main() {
    TestKeys(); TestDurations(); TestBanList(); TestCommands(); TestTargets(); TestVote(); TestAdmins();
    if (g_fail) { std::printf("test_hsm_moderation: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_moderation: all passed\n");
    return 0;
}

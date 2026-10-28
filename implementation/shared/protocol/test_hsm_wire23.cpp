// Protocol rev 23: VoteState (the running votekick, for the in-match menu) and the VoteKick accessors behind it.
#include "hsm_protocol.h"
#include "session/hsm_moderation.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestVoteStateWire() {
    VoteStateMsg m; m.target = 3; m.yes = 2; m.no = 1; m.needed = 3; m.voters = 4; m.secondsLeft = 20; m.mine = -1;
    VoteStateMsg o;
    auto b = Encode(m);
    CHECK(Decode(b.data(), b.size(), o) && o.target == 3 && o.yes == 2 && o.no == 1 && o.needed == 3 && o.voters == 4 && o.secondsLeft == 20 && o.mine == -1);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));
    b.push_back(0); CHECK(!Decode(b.data(), b.size(), o));                 // trailing byte

    const VoteStateMsg none{};
    b = Encode(none); CHECK(Decode(b.data(), b.size(), o) && o.target == kVoteNone && o.mine == kVoteNotVoter);

    auto bad = [](VoteStateMsg v) { auto e = Encode(v); VoteStateMsg x; return !Decode(e.data(), e.size(), x); };
    VoteStateMsg v = none; v.yes = 1; CHECK(bad(v));                       // no vote, but counts
    v = none; v.mine = 0; CHECK(bad(v));
    v = m; v.target = 0; CHECK(!bad(v));                                    // slot 0 (the host) is a valid slot on the wire
    v = m; v.target = kMaxPeerMap; CHECK(bad(v));
    v = m; v.yes = 4; v.no = 1; CHECK(bad(v));                              // more votes than voters
    v = m; v.needed = 5; CHECK(bad(v));
    v = m; v.voters = kMaxPeerMap + 1; CHECK(bad(v));
    v = m; v.secondsLeft = 61; CHECK(bad(v));
    v = m; v.mine = 2; CHECK(bad(v));
    v = m; v.mine = -3; CHECK(bad(v));
    v = m; v.mine = kVoteNotVoter; CHECK(!bad(v));

    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::VoteState);
}

static void TestVoteOf() {
    VoteKick k;
    CHECK(k.VoteOf(1) == -2 && k.Voters() == 0);
    // The host votes as key 1 (no Steam ID needed); players 10, 11, 12; target 13.
    CHECK(k.Begin(10, 13, {1, 10, 11, 12, 13}, 1000) == VoteKick::Start::Ok);
    CHECK(k.Voters() == 4);
    CHECK(k.VoteOf(10) == 1);                      // the starter votes yes
    CHECK(k.VoteOf(1) == 0 && k.VoteOf(11) == 0);
    CHECK(k.VoteOf(13) == -2);                     // the target is not a voter
    CHECK(k.VoteOf(99) == -2);
    CHECK(k.Cast(1, false) && k.VoteOf(1) == -1);
    CHECK(k.Cast(1, true) && k.VoteOf(1) == 1);    // a voter may change their mind
    CHECK(k.Yes() == 2 && k.Needed() == 3);
    CHECK(k.Tick(2000) == VoteKick::Outcome::Running);
    CHECK(k.Cast(11, true));
    CHECK(k.Tick(2100) == VoteKick::Outcome::Passed);
    CHECK(k.VoteOf(10) == -2);                     // over
    k.Left(12);
    CHECK(k.Voters() == 4);                        // no vote running: nothing changes
}

// A voteban needs two thirds of the voters (rounded up), a votekick more than half.
static void TestVoteBan() {
    VoteKick k;
    CHECK(k.Begin(10, 14, {10, 11, 12, 13, 14}, 0, true) == VoteKick::Start::Ok);   // 4 voters
    CHECK(k.Ban() && k.Needed() == 3);
    CHECK(k.Cast(11, true) && k.Tick(10) == VoteKick::Outcome::Running);          // 2 of 4: not yet (3 needed)
    CHECK(k.Cast(12, true) && k.Tick(20) == VoteKick::Outcome::Passed);
    VoteKick kick;
    CHECK(kick.Begin(10, 16, {10, 11, 12, 13, 14, 15, 16}, 0) == VoteKick::Start::Ok);   // 6 voters
    CHECK(!kick.Ban() && kick.Needed() == 4);
    VoteKick ban;
    CHECK(ban.Begin(10, 16, {10, 11, 12, 13, 14, 15, 16}, 0, true) == VoteKick::Start::Ok);
    CHECK(ban.Needed() == 4);                                                     // 6 voters: 4 either way
    CHECK(ban.Cast(11, true) && ban.Cast(12, true) && ban.Tick(5) == VoteKick::Outcome::Running);
    CHECK(ban.Cast(13, false) && ban.Cast(14, false) && ban.Tick(6) == VoteKick::Outcome::Running);   // 3 yes + 1 undecided
    CHECK(ban.Cast(15, false) && ban.Tick(7) == VoteKick::Outcome::Failed);                         // 3 yes, 3 no
    VoteKick five;                                                                // 5 voters: kick 3, ban 4
    CHECK(five.Begin(10, 15, {10, 11, 12, 13, 14, 15}, 0, true) == VoteKick::Start::Ok && five.Needed() == 4);
    CHECK(five.Cast(11, true) && five.Cast(12, true) && five.Tick(1) == VoteKick::Outcome::Running);
    CHECK(five.Cast(13, false) && five.Tick(2) == VoteKick::Outcome::Running);    // 3 yes + 1 undecided can still reach 4
    CHECK(five.Cast(14, false) && five.Tick(3) == VoteKick::Outcome::Failed);
    VoteKick fivek;
    CHECK(fivek.Begin(10, 15, {10, 11, 12, 13, 14, 15}, 0) == VoteKick::Start::Ok && fivek.Needed() == 3);
    CHECK(fivek.Cast(11, true) && fivek.Cast(12, true) && fivek.Tick(1) == VoteKick::Outcome::Passed);
    CHECK(kVoteBanOutSeconds == 7 * 86400 && DurationText(kVoteBanOutSeconds) == "for 7 days");

    // The wire carries the kind.
    VoteStateMsg m; m.target = 2; m.yes = 1; m.needed = 3; m.voters = 4; m.secondsLeft = 30; m.mine = 1; m.ban = 1;
    auto b = Encode(m); VoteStateMsg o;
    CHECK(Decode(b.data(), b.size(), o) && o.ban == 1);
    m.ban = 2; b = Encode(m); CHECK(!Decode(b.data(), b.size(), o));
    VoteStateMsg none; none.ban = 1; b = Encode(none); CHECK(!Decode(b.data(), b.size(), o));
}

int main() {
    TestVoteStateWire();
    TestVoteOf();
    TestVoteBan();
    if (g_fail) { std::printf("test_hsm_wire23: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_wire23: ok\n");
    return 0;
}

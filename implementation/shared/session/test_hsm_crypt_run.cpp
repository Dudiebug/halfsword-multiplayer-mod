// Unit tests for hsm_crypt_run.h (the shared Crypts run's end and results, protocol rev 50). Run via implementation/shared/run_tests.sh test_hsm_crypt_run.
#include "session/hsm_crypt_run.h"
#include "session/hsm_budget.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using namespace hsm::crypt;

static void TestWire() {
    CryptRunMsg k; k.kind = kRunKit; k.seq = 7; k.items = 12;
    auto b = Encode(k); CryptRunMsg y;
    CHECK(Decode(b.data(), b.size(), y) && y.kind == kRunKit && y.seq == 7 && y.items == 12);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, y));
    { auto x = b; x.push_back(0); CHECK(!Decode(x.data(), x.size(), y)); }
    { CryptRunMsg z = k; z.seq = 0; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptRunMsg z = k; z.kind = 2; auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }

    CryptRunMsg r; r.kind = kRunResults; r.seq = 9; r.reason = RunEnd::TimeUp;
    r.rows.push_back({1, Outcome::Out, 5, "Ann"}); r.rows.push_back({3, Outcome::Down, 0, "Bo"}); r.rows.push_back({4, Outcome::Inside, 2, ""});
    auto rb = Encode(r);
    CHECK(Decode(rb.data(), rb.size(), y) && y.kind == kRunResults && y.seq == 9 && y.reason == RunEnd::TimeUp && y.rows.size() == 3
          && y.rows[0].slot == 1 && y.rows[0].outcome == Outcome::Out && y.rows[0].items == 5 && y.rows[0].name == "Ann"
          && y.rows[1].outcome == Outcome::Down && y.rows[2].outcome == Outcome::Inside && y.rows[2].name.empty());
    for (size_t cut = 0; cut < rb.size(); ++cut) CHECK(!Decode(rb.data(), cut, y));
    { auto x = rb; x.push_back(0); CHECK(!Decode(x.data(), x.size(), y)); }
    { auto x = rb; x[4 + 1 + 2] = 0; CHECK(!Decode(x.data(), x.size(), y)); }   // reason 0
    { auto x = rb; x[4 + 1 + 2] = 4; CHECK(!Decode(x.data(), x.size(), y)); }   // reason 4
    { auto x = rb; x[4 + 1 + 2 + 1] = uint8_t(kMaxRunRows + 1); CHECK(!Decode(x.data(), x.size(), y)); }   // too many rows
    { CryptRunMsg z = r; z.rows[1].outcome = Outcome(0); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptRunMsg z = r; z.rows[1].outcome = Outcome(6); auto e = Encode(z); CHECK(!Decode(e.data(), e.size(), y)); }
    { CryptRunMsg z = r; z.rows.clear(); auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.rows.empty()); }
    { CryptRunMsg z = r; z.rows.assign(kMaxRunRows + 5, r.rows[0]); auto e = Encode(z); CHECK(Decode(e.data(), e.size(), y) && y.rows.size() == kMaxRunRows); }   // capped on the wire
    CHECK(BudgetClassOf(MsgType::CryptRun) == BudgetClass::Control && uint8_t(MsgType::CryptRun) == 77);
}

static RunBook Book() {
    RunBook b; b.Begin(4);
    b.Add(0, "Host"); b.Add(1, "Ann"); b.Add(2, "Bo"); b.Add(3, "Cy");
    return b;
}

static void TestBook() {
    RunBook b = Book();
    CHECK(b.Players() == 4 && !b.AllDone() && b.GotOut() == 0);   // nobody in yet: not over
    for (uint8_t s = 0; s < 4; ++s) b.OnNote(s, GatherNote::Loading);
    CHECK(b.Of(0) == Outcome::Absent);
    for (uint8_t s = 0; s < 4; ++s) b.OnNote(s, GatherNote::InCrypts);
    CHECK(b.Count(Outcome::Inside) == 4 && !b.AllDone());
    b.OnNote(1, GatherNote::Extracted); b.OnItems(1, 6);
    CHECK(b.Of(1) == Outcome::Out && b.GotOut() == 1 && !b.AllDone());
    b.OnNote(2, GatherNote::Died); b.OnNote(2, GatherNote::InCrypts);   // a late InCrypts does not undo a death
    CHECK(b.Of(2) == Outcome::Down);
    b.OnNote(1, GatherNote::Died);                                      // nor does a later note change an exit
    CHECK(b.Of(1) == Outcome::Out);
    b.OnNote(0, GatherNote::Extracted); b.OnLeft(3);
    CHECK(b.Of(3) == Outcome::Left && b.AllDone() && b.GotOut() == 2);   // the last one inside left: the run is over
    b.OnLeft(1); CHECK(b.Of(1) == Outcome::Out);                         // an exit stays after the player leaves
}

static void TestResults() {
    RunBook b = Book();
    for (uint8_t s = 0; s < 4; ++s) b.OnNote(s, GatherNote::InCrypts);
    b.OnNote(3, GatherNote::Extracted); b.OnItems(3, 1);
    b.OnNote(1, GatherNote::Died); b.OnItems(1, 9);
    b.OnNote(2, GatherNote::Extracted); b.OnItems(2, 3);
    const CryptRunMsg m = b.Results(RunEnd::TimeUp);
    CHECK(m.kind == kRunResults && m.seq == 4 && m.reason == RunEnd::TimeUp && m.rows.size() == 4);
    CHECK(m.rows[0].slot == 2 && m.rows[1].slot == 3 && m.rows[2].slot == 1 && m.rows[3].slot == 0);   // out by slot, then down, then still inside
    CHECK(m.rows[3].outcome == Outcome::Inside && GotOutOf(m) == 2);
    CHECK(RowText(m.rows[0]) == "Bo - got out - 3 items" && RowText(m.rows[1]) == "Cy - got out - 1 item");
    CHECK(RowText(m.rows[2]) == "Ann - went down - 9 items" && RowText(m.rows[3]) == "Host - still inside");
    CHECK(ResultsTitle(RunEnd::AllOut, 2) == "CRYPTS RESULTS - everyone is out or down: 2 got out");
    auto e = Encode(m); CryptRunMsg y;
    CHECK(Decode(e.data(), e.size(), y) && y.rows.size() == 4 && y.rows[0].name == "Bo");
    RunBook empty; empty.Begin(1);
    CHECK(!empty.AllDone() && empty.Results(RunEnd::HostLeft).rows.empty());
    CHECK(ResultsShowing(1000, 500) && !ResultsShowing(500 + kRunResultsMs, 500) && !ResultsShowing(1000, 0));
    for (uint8_t i = 1; i <= 3; ++i) CHECK(std::string(RunEndName(RunEnd(i))) != "?" && ValidRunEnd(i));
    for (uint8_t i = 1; i <= 5; ++i) CHECK(std::string(OutcomeName(Outcome(i))) != "?" && ValidOutcome(i));
}

static void TestCap() {
    RunBook b; b.Begin(1);
    for (int i = 0; i < 30; ++i) b.Add(uint8_t(i), "p");
    CHECK(b.Players() == int(kMaxRunRows));
    b.Add(2, "renamed"); CHECK(b.Players() == int(kMaxRunRows));
}

int main() {
    TestWire(); TestBook(); TestResults(); TestCap();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("hsm_crypt_run: ok\n");
    return 0;
}

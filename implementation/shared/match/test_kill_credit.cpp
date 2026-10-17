// FIX3: positive opposing damage -> death credit -> referee -> wire -> both HUDs.
#include "match/hsm_hud_text.h"
#include <cassert>
#include <limits>
#include <cstdio>
using namespace hsm;

static MatchInfo Wire(const Match& m, uint32_t now) {
    MatchInfo info; info.format = uint8_t(m.config().format); info.scoreLimit = m.config().scoreLimit;
    info.phase = uint8_t(m.phase()); info.winner = int16_t(m.winner()); info.timeLeftMs = m.TimeLeftMs(now);
    info.fighters = {{1, kNoTeam, int16_t(m.Kills(1)), int16_t(m.Deaths(1)), "Host"},
                     {2, kNoTeam, int16_t(m.Kills(2)), int16_t(m.Deaths(2)), "Client"}};
    for (const auto& [key, score] : m.scores()) info.scores.push_back({int16_t(key), int16_t(score)});
    auto bytes = Encode(info); MatchInfo received; assert(Decode(bytes.data(), bytes.size(), received));
    return received;
}
int main() {
    KillLog k;
    assert(k.KillerOf(1, 5000) == 0); // a fall without enemy damage
    assert(k.OnDamage(1, 2, 1000, 50, true));
    assert(!k.OnDamage(1, 0, 2000, 900, true)); // ground/prop cannot replace or refresh the fighter
    assert(!k.OnDamage(1, 1, 3000, 900, true));
    assert(!k.OnDamage(1, 3, 4000, 900, false)); // teammate even with friendly fire
    assert(!k.OnDamage(1, 3, 5000, 0, true)); // blocked/parried/no damage
    assert(!k.OnDamage(1, 3, 5000, -1, true));
    assert(!k.OnDamage(1, 3, 5000, std::numeric_limits<double>::quiet_NaN(), true));
    assert(!k.OnDamage(1, 3, 5000, std::numeric_limits<double>::infinity(), true));
    assert(k.KillerOf(1, 41000) == 2); // DUEL-3 delayed death, outside former 10 s window
    assert(k.KillerOf(1, 61000) == 2 && k.KillerOf(1, 61001) == 0);
    assert(k.OnDamage(1, 3, 62000, 1, true)); // latest opposing damage wins
    assert(k.KillerOf(1, 62001) == 3);
    k.Forget(1); assert(k.KillerOf(1, 62002) == 0); // new victim body has no prior credit
    k.OnDamage(1, 2, UINT32_MAX - 9, 1, true); assert(k.KillerOf(1, 10) == 2 && k.AgeMs(1, 10) == 20);
    k.Forget(1);

    DamageCredit c{1, 2}, decoded;
    auto b = Encode(c); assert(b.size() == 6 && Decode(b.data(), b.size(), decoded));
    assert(decoded.victim == 1 && decoded.attacker == 2);
    for (size_t n = 0; n < b.size(); ++n) assert(!Decode(b.data(), n, decoded));
    b.push_back(0); assert(!Decode(b.data(), b.size(), decoded));
    for (DamageCredit bad : {DamageCredit{0,2}, DamageCredit{1,0}, DamageCredit{1,1}, DamageCredit{17,2}, DamageCredit{1,17}}) {
        b = Encode(bad); assert(!Decode(b.data(), b.size(), decoded));
    }
    b = Encode(c); b[2] = 11; assert(!Decode(b.data(), b.size(), decoded)); // mixed DLLs fail closed

    Match m; MatchConfig cfg;
    for (uint8_t f = 0; f < kFormatCount; ++f) {
        cfg.format = Format(f); m.Start(cfg,{{1,kNoTeam,true},{2,kNoTeam,true}},0);
        assert(Wire(m,0).format == f); // every supported mode reaches clients
    }
    cfg.format = Format::FreeForAll; cfg.scoreLimit = 2;
    m.Start(cfg, {{1,kNoTeam,true},{2,kNoTeam,true}}, 0); m.Tick(Match::kCountdownMs);
    uint32_t t = 6000;
    for (int death = 0; death < 2; ++death) {
        k.OnDamage(1, 2, t, 800, true);
        k.OnDamage(1, 0, t + 40000, 1000, true);
        DeathReport report{1,k.KillerOf(1,t+40000)}, received;
        b = Encode(report); assert(Decode(b.data(), b.size(), received));
        m.OnDeath(received.victim, received.killer, t+40000);
        m.OnDeath(received.victim, received.killer, t+40001); // repeats do not score twice
        auto info = Wire(m,t+40001);
        assert(ScoreOf(info,2) == death+1 && m.Kills(2) == death+1);
        if (!death) {
            assert(StatusLine(info,2).find("You 1") != std::string::npos);
            assert(StatusLine(info,1).find("Client 1") != std::string::npos);
            auto ev=m.Tick(t+45000); assert(ev.respawn == std::vector<uint8_t>{1});
            m.MarkRespawned(1); k.Forget(1); assert(k.KillerOf(1,t+45001)==0);
        } else {
            assert(m.phase()==MatchPhase::Results && m.winner()==2 && m.Tick(t+40001).ended);
            assert(StatusLine(info,2).find("YOU WIN") != std::string::npos);
            assert(StatusLine(info,1).find("CLIENT WINS") != std::string::npos);
            assert(m.Tick(t+49999).respawn.empty());
            assert(m.Tick(t+50000).rematch && m.Score(2)==0 && m.Kills(2)==0 && m.Alive(1));
            assert(StatusLine(Wire(m,t+50000),1)=="Match starts in 5");
        }
        t += 50000;
    }
    // Actual Duel format: a timed tie ends on the next round, below the score limit.
    cfg.format=Format::Duel; cfg.scoreLimit=3; cfg.timeLimitS=1;
    m.Start(cfg,{{1,kNoTeam,true},{2,kNoTeam,true}},0); m.Tick(5000); m.Tick(6000);
    assert(m.phase()==MatchPhase::SuddenDeath);
    assert(StatusLine(Wire(m,6000),1)=="SUDDEN DEATH   next round wins");
    m.OnDeath(1,2,6100);
    assert(m.phase()==MatchPhase::Results && m.Score(2)==1 && m.winner()==2);
    assert(m.Tick(6101).ended && m.Tick(10100).respawn.empty());
    assert(m.Tick(16100).rematch && m.Alive(1) && m.Alive(2) && m.Score(2)==0);
    std::puts("kill credit / wire / HUD / rematch / duel sudden death: PASS");
}

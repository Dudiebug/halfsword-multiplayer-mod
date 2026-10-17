#include "match/hsm_test_match.h"
#include <cassert>
#include <cstdio>
using namespace hsm;
int main() {
    TestMatchConfig c; assert(!c.Enabled() && c.Apply({}).scoreLimit==5);
    c.SetFormat("duel"); assert(c.Enabled() && c.Apply({}).format==Format::Duel && c.Apply({}).scoreLimit==3);
    c.SetLimit("1"); assert(c.Apply({}).scoreLimit==1);
    c.SetFormat("ffa"); assert(c.Apply({}).format==Format::FreeForAll && c.Apply({}).scoreLimit==1);
    c.SetLimit(""); assert(c.Apply({}).scoreLimit==5);
    for (auto s:{"teams2","teams3","survival","zombies"}) { c.SetFormat(s);assert(c.Enabled() && c.Apply({}).scoreLimit==10); }
    c.SetFormat("bad"); assert(!c.Enabled());
    for (auto s:{"0","101","-1","1x","99999999999999999999"}) { c.SetLimit(s);assert(!c.Enabled()); }
    c.SetLimit("100"); assert(c.Enabled() && c.Apply({}).scoreLimit==100);
    std::puts("test match config: PASS");
}

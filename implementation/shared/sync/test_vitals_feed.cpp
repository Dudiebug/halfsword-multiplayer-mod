#include "sync/hsm_vitals_feed.h"
#include <cassert>
#include <map>
#include <string>

int main() {
    using namespace hsm;
    VitalsMsg v; v.entity=2; v.tsMs=100; v.consciousness=0; v.consciousnessLegs=3.3f;
    v.bloodRate=12.5f; v.partHealth[7]=44.5f; v.partHealth[8]=67.25f;
    std::map<std::string,float> fields;
    VisitVitalsFields(v,[&](const char* n,float value){fields[n]=value;});
    assert(fields.size()==15 && fields.at("Consciousness")==0);
    assert(fields.at("Consciousness 2 (Legs)")==3.3f && fields.at("Blood Rate")==12.5f);
    assert(fields.at("Neck Health")==44.5f && fields.at("Back Health")==67.25f);
    VitalsMsg captured;
    VisitVitalsFields(captured,[&](const char* n,float& value){value=fields.at(n);});
    assert(captured.partHealth==v.partHealth && captured.consciousnessLegs==v.consciousnessLegs);

    sync::VitalsFeed feed;
    assert(feed.Push(v) && !feed.Dead()); // zero consciousness is a knockout, not death
    auto old=v; old.tsMs=99; old.consciousness=100;
    assert(!feed.Push(old) && feed.Latest()->consciousness==0);
    assert(!feed.Push(v)); // duplicate
    v.tsMs=101; v.flags=1; assert(feed.Push(v) && feed.Dead());
    v.tsMs=102; v.flags=0; assert(feed.Push(v) && feed.Dead() && (feed.Latest()->flags&1));
    feed.NewBody(); assert(!feed.Latest() && !feed.Dead());
    v.tsMs=103; v.flags=1; assert(!feed.Push(v) && !feed.Dead()); // previous corpse still in flight
    v.tsMs=104; v.flags=0; assert(feed.Push(v) && !feed.Dead());
    v.tsMs=103; v.flags=1; assert(!feed.Push(v) && !feed.Dead()); // old death after first alive sample
    feed.ConfirmDeath(); assert(feed.Dead() && (feed.Latest()->flags&1));
    feed.NewBody(); feed.ConfirmDeath(); assert(feed.Dead()); // new body killed before first alive sample
    sync::VitalsFeed lateJoin; v.tsMs=1000; v.flags=1;
    assert(lateJoin.Push(v) && lateJoin.Dead()); // first-ever body can already be dead
    sync::VitalsFeed wrap; v.tsMs=0xfffffff0; v.flags=0; assert(wrap.Push(v));
    v.tsMs=20; assert(wrap.Push(v)); v.tsMs=0xfffffff5; assert(!wrap.Push(v));
}

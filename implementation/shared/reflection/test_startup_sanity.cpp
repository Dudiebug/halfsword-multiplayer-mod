#include "reflection/hsm_startup_sanity.h"
#include <cassert>
#include <cstring>
#include <cstdio>
using namespace hsm::uecheck;
int main() {
    std::vector<uint8_t> b(0x30000);uintptr_t pool=0x1000,data=0x10000;
    auto put=[&](size_t at,auto v) { std::memcpy(b.data()+at,&v,sizeof v); };
    put(pool+8,uint32_t(0));put(pool+12,uint32_t(512));put(pool+16,data);
    std::string base="Willie_BP_C";put(data,uint16_t(base.size()<<6));std::memcpy(b.data()+data+2,base.data(),base.size());
    auto read=[&](uintptr_t at,void* dst,size_t n) { if (at>b.size() || n>b.size()-at) return false;std::memcpy(dst,b.data()+at,n);return true; };
    std::string name;
    // Observed in DUEL-4 log: a valid FName instance number, not corruption or an invalid name-pool index.
    assert(PoolName(pool,0,2147482041,read,name) && name=="Willie_BP_C_2147482040");
    assert(PoolName(pool,0,0,read,name) && name==base);
    assert(!PoolName(pool,-1,0,read,name) && !PoolName(pool,0,-1,read,name));
    put(pool+12,uint32_t(2));assert(!PoolName(pool,0,0,read,name)); // incomplete entry at startup
    put(pool+12,uint32_t(512));assert(PoolName(pool,0,1,read,name)); // ready on a subsequent probe
    put(pool+16,uintptr_t(0x100000000));assert(!PoolName(pool,0,0,read,name)); // bad pointer is a failed read
    assert(ArrayShape(25165824,24410,384,1)); // reservation capacity is not live-object count
    assert(!ArrayShape(25165824,70000,384,1));
    assert(!ArrayShape(10,24410,384,1));assert(!ArrayShape(25165824,24410,1,1));
    std::puts("startup sanity: large valid FName / partial registration / unreadable pointer / array capacity PASS");
}

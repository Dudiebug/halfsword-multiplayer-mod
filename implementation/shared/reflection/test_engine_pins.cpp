#include "reflection/hsm_engine_pins.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
using namespace hsm::pins;
template<class T> void Put(std::vector<uint8_t>& b,size_t at,T value) { assert(at+sizeof(T)<=b.size());std::memcpy(b.data()+at,&value,sizeof(T)); }
static void Place(std::vector<uint8_t>& b,size_t at,const char* hex) {
    auto p=Pattern(hex);for (size_t i=0;i<p.size();++i) b[at+i]=p[i]<0?0:uint8_t(p[i]);
}
int main(int argc,char** argv) {
    std::setbuf(stdout,nullptr);
    if (argc==2) {
        std::ifstream in(argv[1],std::ios::binary);assert(in);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),{}),mapped;
        assert(MapFile(file,mapped)); Image im;assert(ParseImage(mapped,im));
        auto r=Scan(im);std::printf("image key=%016llx bytes=%zu error=%s\n",(unsigned long long)im.key,mapped.size(),r.error.c_str());
        for (size_t i=0;i<5;++i) std::printf("%s matches=%zu signature_rva=%08x\n",kSignatures[i].name,r.counts[i],r.hits[i]);
        assert(r.error.empty());
        std::printf("pins GObjects=%08x AppendString=%08x ProcessInternal=%08x ScriptExecutor=%08x execAdd_IntInt=%08x callSite=%x\n",
            r.pins.objects,r.pins.append,r.pins.internal,r.pins.executor,r.pins.add,r.pins.callSite);
        assert(r.pins.objects==0x08bfd240 && r.pins.append==0x012c1cb0 && r.pins.internal==0x014a4850 &&
            r.pins.executor==0x014a4950 && r.pins.add==0x036af990 && r.pins.callSite==0x6c);
        uint32_t nt=0;Read(std::span<const uint8_t>(mapped),60,nt);Put(mapped,nt+8,uint32_t(0x12345678));
        assert(ParseImage(mapped,im));r=Scan(im);assert(r.error.empty() && im.key!=0x8efa122909712000ull);
        std::puts("unknown-key copy: same five pins; PASS (no game execution)");return 0;
    }
    std::vector<uint8_t> b(0x4000); Put(b,0,uint16_t(0x5a4d));Put(b,60,uint32_t(0x80));
    Put(b,0x80,uint32_t(0x4550));Put(b,0x84,uint16_t(0x8664));Put(b,0x86,uint16_t(2));Put(b,0x94,uint16_t(0xf0));
    Put(b,0x98,uint16_t(0x20b));Put(b,0xd0,uint32_t(b.size()));Put(b,0xd4,uint32_t(0x400));
    for (int i=0;i<2;++i) {
        size_t at=0x188+size_t(i)*40;uint32_t rva=i?0x3000:0x1000,size=i?0x1000:0x2000;
        Put(b,at+8,size);Put(b,at+12,rva);Put(b,at+16,size);Put(b,at+20,rva);Put(b,at+36,i?kWritable:kExecutable);
    }
    uint32_t sites[]={0x1800,0x1100,0x1200,0x1400,0x1600};
    for (size_t i=0;i<5;++i) Place(b,sites[i],kSignatures[i].hex);
    Put(b,0x1803,int32_t(0x3100-0x1807)); b[0x126c]=0xe8;Put(b,0x126d,int32_t(0x1400-0x1271));
    Image im;assert(ParseImage(b,im));auto r=Scan(im);assert(r.error.empty() && r.pins.objects==0x3100 && r.pins.callSite==0x6c);
    for (auto n:r.counts) assert(n==1);
    std::vector<uint8_t> mapped;assert(MapFile(b,mapped) && mapped==b);
    for (size_t i=0;i<5;++i) {
        auto broken=b;broken[sites[i]]=0;assert(ParseImage(broken,im));assert(!Scan(im).error.empty());
        broken=b;Place(broken,0x1a00,kSignatures[i].hex);assert(ParseImage(broken,im));
        r=Scan(im);assert(!r.error.empty() && r.counts[i]==2);
    }
    auto broken=b;Put(broken,0x1803,int32_t(-0x2000));assert(ParseImage(broken,im));assert(!Scan(im).error.empty());
    broken=b;Put(broken,0x126d,int32_t(1));assert(ParseImage(broken,im));assert(!Scan(im).error.empty());
    broken=b;Put(broken,0x188+40+12,uint32_t(0x1100));assert(!ParseImage(broken,im));
    broken=b;Put(broken,0x188+20,uint32_t(0xffff));assert(!MapFile(broken,mapped));
    for (size_t n=0;n<0x200;++n) assert(!MapFile({b.data(),n},mapped));
    assert(!Matches(b,b.size(),Pattern("48 ??")));
    std::puts("engine pins: unique/missing/ambiguous/RIP/call/bounds/PE mapping PASS");
}

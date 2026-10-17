// UE 5.4 pin discovery. No OS/game calls; accepts a PE mapped as an image.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace hsm::pins {
constexpr uint32_t kExecutable = 0x20000000, kWritable = 0x80000000;
struct Section { uint32_t rva, size, flags; };
struct Image { std::span<const uint8_t> bytes; std::vector<Section> sections; uint64_t key = 0; };
template<class T> inline bool Read(std::span<const uint8_t> b, size_t at, T& v) {
    if (at > b.size() || sizeof(T) > b.size() - at) return false;
    std::memcpy(&v,b.data()+at,sizeof(T)); return true;
}
inline bool Header(std::span<const uint8_t> b, uint32_t& nt, uint16_t& count, uint16_t& opt, uint32_t& size, uint32_t& headers) {
    uint16_t mz=0, machine=0, magic=0; uint32_t pe=0;
    return Read(b,0,mz) && mz==0x5a4d && Read(b,60,nt) && nt>=64 && nt<=b.size() &&
        Read(b,size_t(nt),pe) && pe==0x4550 && Read(b,size_t(nt)+4,machine) && machine==0x8664 &&
        Read(b,size_t(nt)+6,count) && count>0 && count<=96 && Read(b,size_t(nt)+20,opt) && opt>=112 &&
        Read(b,size_t(nt)+24,magic) && magic==0x20b && Read(b,size_t(nt)+80,size) && size>=4096 && size<=512u*1024*1024 &&
        Read(b,size_t(nt)+84,headers) && headers<=size && headers<=b.size() && size_t(nt)+24+opt+size_t(count)*40<=headers;
}
inline bool ParseImage(std::span<const uint8_t> bytes, Image& image) {
    uint32_t nt=0,size=0,headers=0,stamp=0; uint16_t count=0,opt=0;
    if (!Header(bytes,nt,count,opt,size,headers) || size!=bytes.size()) return false;
    Image out; out.bytes=bytes; Read(bytes,size_t(nt)+8,stamp); out.key=uint64_t(stamp)<<32|size;
    for (size_t i=0;i<count;++i) {
        size_t at=size_t(nt)+24+opt+i*40; uint32_t vs=0,rva=0,raw=0,flags=0;
        Read(bytes,at+8,vs); Read(bytes,at+12,rva); Read(bytes,at+16,raw); Read(bytes,at+36,flags);
        uint32_t len=std::max(vs,raw);
        if (rva<headers || rva>size || len>size-rva) return false;
        for (const auto& s:out.sections) if (len && s.size && rva<s.rva+s.size && s.rva<rva+len) return false;
        out.sections.push_back({rva,len,flags});
    }
    image=std::move(out); return true;
}
inline bool MapFile(std::span<const uint8_t> file, std::vector<uint8_t>& mapped) {
    uint32_t nt=0,size=0,headers=0; uint16_t count=0,opt=0;
    if (!Header(file,nt,count,opt,size,headers)) return false;
    std::vector<uint8_t> out(size,0); std::copy_n(file.begin(),headers,out.begin());
    Image parsed; if (!ParseImage(out,parsed)) return false;
    for (size_t i=0;i<count;++i) {
        size_t at=size_t(nt)+24+opt+i*40; uint32_t rva=0,raw=0,offset=0;
        Read(file,at+12,rva); Read(file,at+16,raw); Read(file,at+20,offset);
        if (offset>file.size() || raw>file.size()-offset) return false;
        std::copy_n(file.begin()+offset,raw,out.begin()+rva);
    }
    mapped=std::move(out); return true;
}
inline bool InSection(const Image& im, uint32_t at, size_t size, uint32_t required, uint32_t forbidden=0) {
    for (const auto& s:im.sections) if ((s.flags&required)==required && !(s.flags&forbidden) &&
        at>=s.rva && at-s.rva<=s.size && size<=s.size-(at-s.rva)) return true;
    return false;
}
struct Signature { const char* name; const char* hex; bool rip; };
inline std::vector<int> Pattern(const char* hex) {
    std::vector<int> out;
    while (*hex) {
        if (*hex==' ') { ++hex; continue; }
        if (*hex=='?' && hex[1]=='?') { out.push_back(-1); hex+=2; continue; }
        auto digit=[](char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1; };
        int a=digit(hex[0]),b=hex[1]?digit(hex[1]):-1;
        if (a<0||b<0) return {};
        out.push_back(a*16+b); hex+=2;
    }
    return out;
}
inline bool Matches(std::span<const uint8_t> bytes, size_t at, const std::vector<int>& pat) {
    if (pat.empty() || at>bytes.size() || pat.size()>bytes.size()-at) return false;
    for (size_t i=0;i<pat.size();++i) if (pat[i]>=0 && bytes[at+i]!=pat[i]) return false;
    return true;
}
inline const std::array<Signature,5> kSignatures = {{
    {"GObjects", "48 8b 05 ?? ?? ?? ?? 48 8b 14 c8 4b 8d 0c 40 4c 8d 04 ca eb 03 4d 8b c7 41 8b 40 08 0f ba e0 15 72 18 8b c8 0f ba e9 15 f0 41 0f b1 48 08 74 0a", true},
    {"AppendString", "48 89 5c 24 10 48 89 74 24 18 57 48 83 ec 20 80 3d ?? ?? ?? ?? 00 48 8b f2 8b 19 48 8b f9 74 09 4c 8d 05 ?? ?? ?? ?? eb 16 48 8d 0d ?? ?? ?? ?? e8 ?? ?? ?? ?? 4c 8b c0 c6 05 ?? ?? ?? ?? 01", false},
    {"ProcessInternal", "48 89 5c 24 08 48 89 6c 24 10 48 89 74 24 18 57 41 56 41 57 48 83 ec 30 4c 8b 72 10 49 8b f8 48 8b 01 48 8b ea 45 33 c0 49 8b d6 48 8b d9 ff 90 70 02 00 00", false},
    {"ScriptExecutor", "48 89 5c 24 08 55 56 57 48 83 ec 70 48 8b 05 ?? ?? ?? ?? 48 33 c4 48 89 44 24 60 48 8b 4a 20 48 8d 2d ?? ?? ?? ?? 48 8b 72 10 49 8b f8 48 8b da 80 39 04", false},
    {"execAdd_IntInt", "48 89 5c 24 08 48 89 74 24 20 57 48 83 ec 20 33 ff 49 8b f0 89 7c 24 38 48 8b da e8 ?? ?? ?? ?? 48 8b cb 48 39 7b 20 74 10 48 8b 53 18 4c 8d 44 24 38 e8 ?? ?? ?? ?? eb 1c 4c 8b 83 88 00 00 00 48 8d 54 24 38 49 8b 40 18 48 89 83 88 00 00 00 e8 ?? ?? ?? ?? 89 7c 24 40 e8 ?? ?? ?? ?? 48 8b cb 48 39 7b 20 74 10 48 8b 53 18 4c 8d 44 24 40 e8 ?? ?? ?? ?? eb 1c 4c 8b 83 88 00 00 00 48 8d 54 24 40 49 8b 40 18 48 89 83 88 00 00 00 e8 ?? ?? ?? ?? 48 8b 43 20 8b 4c 24 38 48 85 c0 40 0f 95 c7 48 03 f8 03 4c 24 40 48 89 7b 20 48 8b 5c 24 30 89 0e 48 8b 74 24 48 48 83 c4 20 5f c3 cc 48 89 5c 24 08 48 89 74 24 20 57 48 83 ec 20 49 8b f0 48 8b da e8", false}
}};
struct Pins { uint32_t objects=0, append=0, internal=0, executor=0, add=0, callSite=0; };
struct Result { Pins pins; std::array<size_t,5> counts{}; std::array<uint32_t,5> hits{}; std::string error; };
inline bool Validate(const Image& im, const Pins& p, std::string& error) {
    const uint32_t addresses[]={p.objects,p.append,p.internal,p.executor,p.add};
    for (size_t i=0;i<5;++i) {
        bool ok=i==0 ? InSection(im,addresses[i],32,kWritable,kExecutable) :
            InSection(im,addresses[i],Pattern(kSignatures[i].hex).size(),kExecutable) &&
            Matches(im.bytes,addresses[i],Pattern(kSignatures[i].hex));
        if (!ok) { error=std::string(kSignatures[i].name)+": section/prologue mismatch"; return false; }
    }
    if (p.callSite>=256 || !InSection(im,p.internal, p.callSite+5,kExecutable) || im.bytes[p.internal+p.callSite]!=0xe8) {
        error="ProcessInternal: executor call missing"; return false;
    }
    int32_t disp=0; Read(im.bytes,size_t(p.internal)+p.callSite+1,disp);
    if (int64_t(p.internal)+p.callSite+5+disp!=p.executor) { error="ProcessInternal: executor call target mismatch"; return false; }
    return true;
}
inline Result Scan(const Image& im) {
    Result out; std::array<uint32_t,5> found{};
    for (size_t i=0;i<5;++i) {
        auto pat=Pattern(kSignatures[i].hex);
        for (const auto& s:im.sections) if ((s.flags&kExecutable) && pat.size()<=s.size) {
            const uint8_t* begin=im.bytes.data()+s.rva;
            const uint8_t* end=begin+s.size-pat.size()+1;
            while (begin<end) {
                begin=std::find(begin,end,uint8_t(pat[0])); if (begin==end) break;
                uint32_t at=uint32_t(begin-im.bytes.data());
                if (Matches(im.bytes,at,pat)) { ++out.counts[i]; out.hits[i]=at; }
                ++begin;
            }
        }
        if (out.counts[i]!=1) { out.error=std::string(kSignatures[i].name)+": expected 1 signature hit, got "+std::to_string(out.counts[i]); return out; }
        int64_t target=out.hits[i];
        if (kSignatures[i].rip) { int32_t disp=0; Read(im.bytes,target+3,disp); target+=7+disp; }
        if (target<0 || target>UINT32_MAX) { out.error=std::string(kSignatures[i].name)+": RIP target outside image"; return out; }
        found[i]=uint32_t(target);
        uint32_t* slots[]={&out.pins.objects,&out.pins.append,&out.pins.internal,&out.pins.executor,&out.pins.add};
        *slots[i]=found[i];
    }
    out.pins={found[0],found[1],found[2],found[3],found[4],0};
    size_t calls=0;
    for (uint32_t off=0;off<256;++off) {
        if (!InSection(im,out.pins.internal,size_t(off)+5,kExecutable)) break;
        if (im.bytes[out.pins.internal+off]!=0xe8) continue;
        int32_t disp=0; Read(im.bytes,size_t(out.pins.internal)+off+1,disp);
        if (int64_t(out.pins.internal)+off+5+disp==out.pins.executor) { ++calls;out.pins.callSite=off; }
    }
    if (calls!=1) { out.error="ProcessInternal: expected one call to executor";return out; }
    Validate(im,out.pins,out.error); return out;
}
} // namespace hsm::pins

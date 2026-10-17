// Read-only UE 5.4 startup checks, shared with offline regression fixtures.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace hsm::uecheck {
inline bool ArrayShape(int32_t max,int32_t count,int32_t maxChunks,int32_t chunks) {
    return count>=10000 && max>=count && chunks>0 && maxChunks>=chunks && maxChunks<=32768 &&
        int64_t(chunks)*65536>=count && int64_t(maxChunks)*65536>=max;
}
// Read callback: bool(uintptr_t address, void* destination, size_t bytes). Never executes engine code.
template<class Read> bool PoolName(uintptr_t pool,int32_t index,int32_t number,Read read,std::string& out) {
    out.clear(); if (!pool || index<0 || number<0) return false;
    uint32_t current=0,cursor=0,block=uint32_t(index)>>16,off=(uint32_t(index)&65535)*2;
    uintptr_t data=0;uint16_t header=0;
    if (!read(pool+8,&current,4) || current>=8192 || block>current || !read(pool+12,&cursor,4) || cursor>131072 ||
        !read(pool+16+size_t(block)*8,&data,8) || !data || data>UINTPTR_MAX-131072 || !read(data+off,&header,2)) return false;
    size_t len=header>>6,width=(header&1)?2:1,bytes=2+len*width;
    if (!len || len>=1024 || off+bytes>131072 || (block==current && off+bytes>cursor)) return false;
    std::vector<uint8_t> chars(len*width);
    if (!read(data+off+2,chars.data(),chars.size())) return false;
    for (size_t i=0;i<len;++i) {
        uint16_t c=chars[i*width];if (width==2) c|=uint16_t(chars[i*width+1])<<8;
        if (!c) return false;
        out.push_back(c<128?char(c):'?');
    }
    if (number) out+='_'+std::to_string(number-1);
    return out.size()<1024; // includes the numeric suffix: native AppendString must fit the same buffer
}
} // namespace hsm::uecheck

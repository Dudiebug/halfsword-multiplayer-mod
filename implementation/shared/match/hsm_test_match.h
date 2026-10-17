// Explicit direct-IP test overrides. Steam callers never parse or apply these settings.
#pragma once
#include "match/hsm_match.h"
#include <charconv>
#include <string_view>
namespace hsm {
struct TestMatchConfig {
    int format=-1; uint16_t limit=0;
    bool Enabled() const { return format>=0 || limit!=0; }
    void SetFormat(std::string_view value) {
        constexpr std::string_view names[]={"ffa","teams2","teams3","survival","duel","zombies"};
        format=-1;
        for (int i=0;i<int(kFormatCount);++i) if (value==names[i]) format=i;
    }
    void SetLimit(std::string_view value) {
        unsigned n=0; auto [end,ec]=std::from_chars(value.data(),value.data()+value.size(),n);
        limit=ec==std::errc{} && end==value.data()+value.size() && n>=1 && n<=100 ? uint16_t(n):0;
    }
    MatchConfig Apply(MatchConfig c) const {
        if (format>=0 && format<kFormatCount) { c.format=Format(format); c.scoreLimit=DefaultScoreLimit(c.format); }
        if (limit) c.scoreLimit=limit;
        return c;
    }
};
} // namespace hsm

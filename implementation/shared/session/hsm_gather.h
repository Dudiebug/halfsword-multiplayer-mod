// The Crypts gather's wire (protocol rev 47, match/hsm_crypts.h). Gather 71, reliable, both ways:
//   host -> client  GatherMsg: the gather's phase (GatherPhase: 0 over or called off, 1 gathering, 2 collecting the last
//                   kits), its time left in ms (at most the Gather time's 30 minutes) and its seq (never 0). Sent at the
//                   start, every kGatherClockEveryMs, to each joiner, at Collect and at the end.
//   client -> host  GatherNoteMsg: a note about the sender's own run (GatherNote) in the gather of that seq.
// The kits themselves go as LoadoutChange 61 chunks (session/hsm_loadout_change.h): while a gather runs, the host takes a
// client's whole passport as its gathered kit, after the same checks as any upload.
#pragma once
#include "match/hsm_crypts.h"
#include "protocol/hsm_protocol.h"
#include <cstdint>
#include <vector>

namespace hsm {

struct GatherMsg { GatherPhase phase = GatherPhase::Off; uint32_t leftMs = 0; uint16_t seq = 0; };
struct GatherNoteMsg { GatherNote note = GatherNote::Loading; uint16_t seq = 0; };

// The two share the type and differ in length: the host's is 8 bytes after the header, a note 3.
inline std::vector<uint8_t> Encode(const GatherMsg& m) {
    Writer w; w.header(MsgType::Gather); w.u8(uint8_t(m.phase)); w.u32(m.leftMs); w.u16(m.seq); w.u8(0); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, GatherMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Gather) return false;
    const uint8_t p = r.u8(); m.leftMs = r.u32(); m.seq = r.u16(); const uint8_t spare = r.u8();
    if (!r.done() || p >= kGatherPhaseCount || spare != 0 || m.seq == 0) return false;
    if (m.leftMs > uint32_t(kGatherMaxS) * 1000u) return false;
    m.phase = GatherPhase(p);
    return true;
}
inline std::vector<uint8_t> Encode(const GatherNoteMsg& m) {
    Writer w; w.header(MsgType::Gather); w.u8(uint8_t(m.note)); w.u16(m.seq); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, GatherNoteMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Gather) return false;
    const uint8_t k = r.u8(); m.seq = r.u16();
    if (!r.done() || !ValidGatherNote(k) || m.seq == 0) return false;
    m.note = GatherNote(k);
    return true;
}

}  // namespace hsm

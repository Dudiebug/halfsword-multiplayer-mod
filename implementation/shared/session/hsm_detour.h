// Detour routing, step R1 (0.7.0-pre6): a client passes another pair's Pose and Vitals on when that is faster than the
// direct link or the host (session/hsm_route.h plans it). Three roles:
// - The host (PairKeys, MakeRouteTable): a 16-byte key per client pair, sent only to the pair's two ends over their
//   reliable host links, and each client's RouteTable (protocol rev 23) whenever the plan or the lobby changes.
// - The sender A (DetourClient::MakeForward): for a peer B routed through C, wraps each Pose/Vitals in a Forward with
//   a SipHash-2-4 tag of (from, to, epoch, seq, payload) under the pair's key, and sends it to C. The direct copy
//   (mesh) still goes too; whichever arrives first is used, so a detour never makes a stream later.
// - The relay C (MayRelay): passes on only pairs its own table lists, from the pair's own ends, within a budget per
//   direction; it never sees the key, so it can drop or delay a Forward but not change one.
// - The receiver B (Accept): the tag, the relay its table names, the epoch (this table or the one before, as the two
//   ends get a new table at slightly different times) and a newer seq. Then the payload is checked as if A sent it.
// B's MeshHeard bit for A means "heard directly or through the detour": a detour that goes quiet for 2 s clears it,
// and the host relays again (session/hsm_mesh.h).
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_authority.h"
#include "session/hsm_route.h"
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace hsm {

static_assert(kRouteViaDirect == kRouteDirect && kRouteViaHost == kRouteHost, "RouteTable.via uses the planner's values");

// ---------------------------------------------------------------- SipHash-2-4 (Aumasson, Bernstein), 64-bit tag
using DetourKey = std::array<uint8_t, kRouteKeyBytes>;

inline uint64_t SipHash24(const DetourKey& key, const uint8_t* d, size_t n) {
    auto le64 = [](const uint8_t* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | p[i]; return v; };
    auto rotl = [](uint64_t x, int b) { return (x << b) | (x >> (64 - b)); };
    const uint64_t k0 = le64(key.data()), k1 = le64(key.data() + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotl(v1, 13); v1 ^= v0; v0 = rotl(v0, 32);
        v2 += v3; v3 = rotl(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl(v1, 17); v1 ^= v2; v2 = rotl(v2, 32);
    };
    const size_t whole = n - n % 8;
    for (size_t i = 0; i < whole; i += 8) {
        const uint64_t m = le64(d + i);
        v3 ^= m; round(); round(); v0 ^= m;
    }
    uint64_t last = uint64_t(n & 0xFF) << 56;
    for (size_t i = 0; i < n % 8; ++i) last |= uint64_t(d[whole + i]) << (8 * i);
    v3 ^= last; round(); round(); v0 ^= last;
    v2 ^= 0xFF;
    round(); round(); round(); round();
    return v0 ^ v1 ^ v2 ^ v3;
}

inline uint64_t ForwardTag(const DetourKey& key, uint8_t from, uint8_t to, uint8_t epoch, uint16_t seq, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    b.reserve(5 + payload.size());
    b.push_back(from); b.push_back(to); b.push_back(epoch); b.push_back(uint8_t(seq)); b.push_back(uint8_t(seq >> 8));
    b.insert(b.end(), payload.begin(), payload.end());
    return SipHash24(key, b.data(), b.size());
}

// ---------------------------------------------------------------- host
// One key per client pair, made when the pair is first needed and dropped when either end leaves (a returning player
// gets a new one). random: fills the key (the session uses the system's secure random source).
class PairKeys {
public:
    using Random = std::function<void(uint8_t*, size_t)>;
    const DetourKey& Get(uint8_t a, uint8_t b, const Random& random) {
        if (a > b) { const uint8_t t = a; a = b; b = t; }
        Slot& s = k_[a][b];
        if (!s.made) { random(s.key.data(), s.key.size()); s.made = true; }
        return s.key;
    }
    bool Has(uint8_t a, uint8_t b) const { if (a > b) { const uint8_t t = a; a = b; b = t; } return a < kMaxFighters && b < kMaxFighters && k_[a][b].made; }
    void Forget(uint8_t slot) {
        if (slot >= kMaxFighters) return;
        for (uint8_t o = 0; o < kMaxFighters; ++o) { k_[o][slot] = Slot{}; k_[slot][o] = Slot{}; }
    }
    void Clear() { k_ = {}; }
private:
    struct Slot { bool made = false; DetourKey key{}; };
    std::array<std::array<Slot, kMaxFighters>, kMaxFighters> k_{};
};

// Client `me`'s table: every other client in the lobby with the pair's route (and key, for a detour through a relay:
// the only routes that need one), and the pairs `me` relays for.
inline RouteTableMsg MakeRouteTable(uint8_t me, const std::vector<uint8_t>& clients, const RoutePlanner& plan, PairKeys& keys,
                                    uint8_t epoch, const PairKeys::Random& random) {
    RouteTableMsg m; m.epoch = epoch;
    std::array<bool, kMaxFighters> in{};
    for (uint8_t s : clients) if (s != kHostSlot && s < kMaxFighters) in[s] = true;
    if (me == kHostSlot || me >= kMaxFighters || !in[me]) return m;
    for (uint8_t p = 1; p < kMaxFighters; ++p) {
        if (!in[p] || p == me) continue;
        RouteKey k; k.peer = p;
        const uint8_t r = plan.RouteOf(me, p);
        k.via = (r == kRouteDirect || r == kRouteHost || (r < kMaxFighters && in[r])) ? r : kRouteHost;
        if (k.via != kRouteDirect && k.via != kRouteHost) k.key = keys.Get(me, p, random);
        m.keys.push_back(k);
    }
    for (uint8_t a = 1; a < kMaxFighters; ++a)
        for (uint8_t b = uint8_t(a + 1); b < kMaxFighters; ++b)
            if (in[a] && in[b] && a != me && b != me && plan.RouteOf(a, b) == me) m.relays.push_back(RoutePair{a, b});
    return m;
}

// ---------------------------------------------------------------- clients
constexpr uint32_t kDetourRelayPerSec = 120;   // Forwards one relay passes on per direction of a pair, a second
constexpr uint32_t kDetourRelayBurst = 40;     // ...with this much slack for a hitch

enum class DetourVerdict : uint8_t { Ok, NotForMe, Epoch, Relay, Tag, Seq };
inline const char* DetourVerdictName(DetourVerdict v) {
    static const char* const k[] = {"ok", "not for me", "epoch", "relay", "tag", "seq"};
    return k[size_t(v)];
}
enum class RelayVerdict : uint8_t { Ok, ForMe, NotListed, Sender, Budget };

class DetourClient {
public:
    // A new table from the host. me: this client's slot (Welcome). The table before stays valid for Forwards still on
    // their way with its epoch.
    void SetTable(uint8_t me, const RouteTableMsg& t) {
        me_ = me;
        prev_ = cur_; prevValid_ = curValid_;
        cur_ = Table{}; cur_.epoch = t.epoch; curValid_ = true;
        for (const RouteKey& k : t.keys) {
            if (k.peer >= kMaxFighters || k.peer == me) continue;
            cur_.via[k.peer] = k.via; cur_.key[k.peer] = k.key; cur_.listed[k.peer] = true;
        }
        for (const RoutePair& p : t.relays)
            if (p.a < kMaxFighters && p.b < kMaxFighters && p.a != me && p.b != me) cur_.relay[p.a][p.b] = true;
    }
    // A peer left: nothing goes to or through it until the next table (the host sends one at once).
    void Forget(uint8_t slot) {
        if (slot >= kMaxFighters) return;
        for (Table* t : {&cur_, &prev_}) {
            t->listed[slot] = false;
            for (uint8_t p = 0; p < kMaxFighters; ++p) if (t->via[p] == slot) t->via[p] = kRouteHost;
            for (uint8_t o = 0; o < kMaxFighters; ++o) { t->relay[o][slot] = false; t->relay[slot][o] = false; }
        }
        sendSeq_[slot] = 0; recv_[slot] = Recv{}; bucket_[slot] = {}; for (auto& row : bucket_) row[slot] = Bucket{};
    }
    void Clear() { *this = DetourClient{}; }

    // The relay this client sends `peer`'s copy through, or 0 (none: direct or the host only).
    uint8_t RelayFor(uint8_t peer) const {
        if (!curValid_ || peer >= kMaxFighters || !cur_.listed[peer]) return 0;
        const uint8_t v = cur_.via[peer];
        return v != kRouteDirect && v != kRouteHost && v < kMaxFighters && v != me_ && v != peer ? v : 0;
    }
    // Wraps one encoded Pose/Vitals for `peer`. False when no detour applies (RelayFor is 0).
    bool MakeForward(uint8_t peer, const std::vector<uint8_t>& payload, ForwardMsg& out) {
        if (!RelayFor(peer) || payload.empty() || payload.size() > kMaxForwardBytes) return false;
        out.from = me_; out.to = peer; out.epoch = cur_.epoch; out.seq = ++sendSeq_[peer]; out.payload = payload;
        out.tag = ForwardTag(cur_.key[peer], out.from, out.to, out.epoch, out.seq, out.payload);
        return true;
    }

    // As the relay: may this Forward, arrived from `senderSlot`, be passed on to m.to?
    RelayVerdict MayRelay(uint8_t senderSlot, const ForwardMsg& m, uint32_t nowMs) {
        if (m.to == me_) return RelayVerdict::ForMe;
        if (senderSlot != m.from || m.from >= kMaxFighters || m.to >= kMaxFighters) return RelayVerdict::Sender;
        const uint8_t a = m.from < m.to ? m.from : m.to, b = m.from < m.to ? m.to : m.from;
        const bool listed = (curValid_ && cur_.relay[a][b]) || (prevValid_ && prev_.relay[a][b]);
        if (!listed) return RelayVerdict::NotListed;
        if (!bucket_[m.from][m.to].Take(nowMs)) return RelayVerdict::Budget;
        return RelayVerdict::Ok;
    }

    // As the receiver: may this Forward, arrived from `senderSlot`, be taken as m.from's own message?
    DetourVerdict Accept(uint8_t senderSlot, const ForwardMsg& m) {
        if (m.to != me_ || m.from >= kMaxFighters || m.from == me_) return DetourVerdict::NotForMe;
        const Table* t = curValid_ && m.epoch == cur_.epoch ? &cur_ : prevValid_ && m.epoch == prev_.epoch ? &prev_ : nullptr;
        if (!t || !t->listed[m.from]) return DetourVerdict::Epoch;
        const uint8_t via = t->via[m.from];
        if (via == kRouteDirect || via == kRouteHost || via != senderSlot) return DetourVerdict::Relay;
        if (ForwardTag(t->key[m.from], m.from, m.to, m.epoch, m.seq, m.payload) != m.tag) return DetourVerdict::Tag;
        Recv& r = recv_[m.from];
        if (r.any && r.epoch == m.epoch && int16_t(uint16_t(m.seq - r.seq)) <= 0) return DetourVerdict::Seq;
        if (r.any && t == &prev_ && curValid_ && r.epoch == cur_.epoch && cur_.epoch != prev_.epoch) return DetourVerdict::Seq;   // the newer table's stream has begun
        r.any = true; r.epoch = m.epoch; r.seq = m.seq;
        return DetourVerdict::Ok;
    }

    uint8_t me() const { return me_; }
    bool HasTable() const { return curValid_; }
    uint8_t Epoch() const { return cur_.epoch; }
    // Pairs this client relays for (the current table).
    int RelayPairs() const {
        int n = 0;
        if (curValid_) for (const auto& row : cur_.relay) for (bool x : row) n += x ? 1 : 0;
        return n;
    }

private:
    struct Table {
        uint8_t epoch = 0;
        std::array<uint8_t, kMaxFighters> via{};
        std::array<bool, kMaxFighters> listed{};
        std::array<DetourKey, kMaxFighters> key{};
        std::array<std::array<bool, kMaxFighters>, kMaxFighters> relay{};   // [a][b], a < b
    };
    struct Recv { bool any = false; uint8_t epoch = 0; uint16_t seq = 0; };
    struct Bucket {
        uint32_t tokens = kDetourRelayBurst, at = 0; bool started = false;
        bool Take(uint32_t now) {
            if (!started) { started = true; at = now; }
            const uint32_t gained = (now - at) * kDetourRelayPerSec / 1000;
            if (gained > 0) { tokens = tokens + gained > kDetourRelayBurst ? kDetourRelayBurst : tokens + gained; at += gained * 1000 / kDetourRelayPerSec; }
            if (tokens == 0) return false;
            --tokens; return true;
        }
    };
    uint8_t me_ = 0;
    Table cur_, prev_;
    bool curValid_ = false, prevValid_ = false;
    std::array<uint16_t, kMaxFighters> sendSeq_{};
    std::array<Recv, kMaxFighters> recv_{};
    std::array<std::array<Bucket, kMaxFighters>, kMaxFighters> bucket_{};   // [from][to]
};

}  // namespace hsm

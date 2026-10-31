// The HSMMod recorder's file format (.hsmrec, version 1) and the events it derives from field edges.
// Pure: no engine types. The recorder (client/HSMMod/src/gameplay/recorder.cpp) fills these structs at 60 Hz and
// tools/hsmrec.py reads the file; docs/PLAN_RECORDER.md has the layout in prose.
//
// File: header, then chunks. Header: "HSMREC", u8 version, u8 reserved, u64 build key, str mod build, str level,
// u64 start wall time (unix ms), f64 start steady ms. Chunk: u8 kind, u32 len, payload[len], u32 crc32(payload).
// Little-endian, IEEE floats, strings are u8 length + bytes. A reader skips kinds it does not know; a short or bad
// last chunk is a crash's truncated tail, not a broken file.
#pragma once
#include "hsm_protocol.h"
#include "util/hsm_gzip.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hsm::rec {

constexpr char kMagic[6] = {'H', 'S', 'M', 'R', 'E', 'C'};
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxStr = 255;
constexpr size_t kMaxBones = 512;
constexpr size_t kMaxParts = 16;
constexpr size_t kMaxPropBodies = 64;
constexpr size_t kMaxAxes = 32;
constexpr uint32_t kMaxChunk = 1u << 20;
constexpr uint16_t kNoParent = 0xFFFF;
constexpr uint8_t kNoHand = 0xFF;

enum class Chunk : uint8_t { FighterTable = 1, FighterFrame = 2, Vitals = 3, PropTable = 4, PropFrame = 5, Event = 6, Input = 7 };

// Fighter flags (FighterTable.flags).
constexpr uint8_t kFighterLocal = 1, kFighterBot = 2, kFighterStandIn = 4;
// Vitals flags.
constexpr uint8_t kVitDead = 1, kVitFallen = 2, kVitKnockedOut = 4;
// PropFrame flags.
constexpr uint8_t kPropAtRest = 1;

// Hands: 0 right, 1 left (as WeaponSample.hand).
enum class Ev : uint8_t {
    GrabOn = 1, GrabOff, BeingGrabbedOn, BeingGrabbedOff, GripChange, PickUp, Drop, Draw, Sheathe, Thrust, Kick,
    ParryOn, ParryOff, Hit, Damage, Sever, StuckOn, StuckOff, Fall, GetUp, Death, Input, LevelStart, LevelEnd, Marker,
    Break,   // a geometry collection's root cluster broke (propId, text = the component)
};
constexpr uint8_t kEvLast = uint8_t(Ev::Break);
inline const char* EvName(Ev e) {
    static const char* const names[] = {"?", "GrabOn", "GrabOff", "BeingGrabbedOn", "BeingGrabbedOff", "GripChange",
                                        "PickUp", "Drop", "Draw", "Sheathe", "Thrust", "Kick", "ParryOn", "ParryOff",
                                        "Hit", "Damage", "Sever", "StuckOn", "StuckOff", "Fall", "GetUp", "Death",
                                        "Input", "LevelStart", "LevelEnd", "Marker", "Break"};
    static_assert(sizeof(names) / sizeof(names[0]) == kEvLast + 1, "one name per event kind");
    return uint8_t(e) <= kEvLast ? names[uint8_t(e)] : "?";
}

using F3 = std::array<float, 3>;

struct FileHeader {
    uint64_t buildKey = 0;
    std::string modBuild, level;
    uint64_t wallMs = 0;
    double steadyMs = 0;
};
struct BoneInfo { std::string name; uint16_t parent = kNoParent; bool hasBody = false; };
struct FighterTable {
    uint16_t recId = 0;   // 1.. per file; 0 means "none" in events
    uint8_t flags = 0;
    std::string name, cls;
    std::vector<BoneInfo> bones;
};
struct BoneFrame {
    F3 pos{};
    Quat16 rot;
    bool hasBody = false;   // the velocities below are only written for a simulating body
    F3 vel{}, angVelDeg{};
};
struct WeaponFrame {
    bool present = false;
    uint32_t classHash = 0;
    F3 pos{};
    Quat16 rot;
    F3 vel{}, angVelDeg{}, base{}, tip{};
};
struct FighterFrame {
    double ts = 0;   // steady ms
    float dt = 0;    // the game frame's delta, s
    uint16_t recId = 0;
    std::vector<BoneFrame> bones;   // in FighterTable.bones order
    std::array<WeaponFrame, 2> weapons{};
    uint16_t grip = 0;              // sync::PackGrip
};
struct Vitals {
    double ts = 0;
    uint16_t recId = 0;
    float health = 0, consciousness = 0, stamina = 0, bleeding = 0;
    std::vector<float> partHealth;
    uint8_t flags = 0;
    uint32_t stateMask = 0, severMask = 0, dislocMask = 0;
};
struct PropTable {
    uint64_t propId = 0;
    std::string name, cls;
    std::vector<std::string> bodies;
};
struct PropFrame {
    double ts = 0;
    uint64_t propId = 0;
    uint8_t body = 0, flags = 0;
    F3 pos{};
    Quat16 rot;
    F3 vel{}, angVelDeg{};
};
struct Event {
    double ts = 0;
    Ev kind = Ev::Marker;
    uint16_t subject = 0, other = 0;   // fighter recIds, 0 none
    uint64_t propId = 0;
    uint8_t hand = kNoHand;
    std::string text;                  // bone, input binding, level name or marker text
    std::array<float, 4> x{};          // kind-specific numbers (see EdgeEvents and recorder.cpp)
};

// The local player's input in one sample (the game's own input events, read where it gets them).
struct InputSample {
    double ts = 0;
    uint16_t recId = 0;
    std::vector<float> axes;    // hsm::Axis order: the mouse turn/look summed since the last sample, the rest the latest value
    uint32_t buttons = 0;       // bit i: hsm::Action i held
    float controlYaw = 0, controlPitch = 0;   // the camera (control rotation), degrees
};

// ---------------------------------------------------------------- encoding
namespace detail {
inline void F64(Writer& w, double v) { uint64_t u; std::memcpy(&u, &v, 8); w.u64(u); }
inline double F64(Reader& r, bool& ok) {
    uint64_t u = r.u64(); double v; std::memcpy(&v, &u, 8);
    if (!std::isfinite(v)) ok = false;
    return v;
}
inline void Put(Writer& w, const F3& v) { w.f32(v[0]); w.f32(v[1]); w.f32(v[2]); }
inline F3 GetF3(Reader& r) { F3 v; v[0] = r.f32(); v[1] = r.f32(); v[2] = r.f32(); return v; }
inline void Put(Writer& w, const Quat16& q) { w.i16(q.x); w.i16(q.y); w.i16(q.z); w.i16(q.w); }
inline Quat16 GetQ(Reader& r) { Quat16 q; q.x = r.i16(); q.y = r.i16(); q.z = r.i16(); q.w = r.i16(); return q; }
}  // namespace detail

inline std::vector<uint8_t> EncodeHeader(const FileHeader& h) {
    Writer w;
    for (char c : kMagic) w.u8(uint8_t(c));
    w.u8(kVersion); w.u8(0);
    w.u64(h.buildKey); w.str(h.modBuild, kMaxStr); w.str(h.level, kMaxStr); w.u64(h.wallMs); detail::F64(w, h.steadyMs);
    return std::move(w.buf);
}
// Returns the header's size in bytes, 0 if the data does not start with a version-1 header.
inline size_t DecodeHeader(const uint8_t* d, size_t n, FileHeader& h) {
    Reader r(d, n);
    for (char c : kMagic) if (r.u8() != uint8_t(c)) return 0;
    if (r.u8() != kVersion) return 0;
    r.u8();
    bool ok = true;
    h.buildKey = r.u64(); h.modBuild = r.str(kMaxStr); h.level = r.str(kMaxStr); h.wallMs = r.u64(); h.steadyMs = detail::F64(r, ok);
    if (!ok || !r.ok()) return 0;
    return 6 + 2 + 8 + 1 + h.modBuild.size() + 1 + h.level.size() + 8 + 8;
}

// Appends kind, len, payload, crc to `out`.
inline void AppendChunk(std::vector<uint8_t>& out, Chunk kind, const std::vector<uint8_t>& payload) {
    Writer w;
    w.u8(uint8_t(kind)); w.u32(uint32_t(payload.size()));
    out.insert(out.end(), w.buf.begin(), w.buf.end());
    out.insert(out.end(), payload.begin(), payload.end());
    Writer c; c.u32(util::Crc32(payload.data(), payload.size()));
    out.insert(out.end(), c.buf.begin(), c.buf.end());
}

struct ChunkView { Chunk kind{}; const uint8_t* data = nullptr; size_t size = 0; };
enum class NextResult { Ok, End, Truncated, Corrupt };
// Reads the chunk at `pos` and advances it. End at exactly the end of the data; Truncated when the data stops inside
// a chunk (a crash); Corrupt on a bad length or checksum.
inline NextResult NextChunk(const uint8_t* d, size_t n, size_t& pos, ChunkView& c) {
    if (pos == n) return NextResult::End;
    if (n - pos < 5) return NextResult::Truncated;
    Reader r(d + pos, 5);
    c.kind = Chunk(r.u8());
    const uint32_t len = r.u32();
    if (len > kMaxChunk) return NextResult::Corrupt;
    if (n - pos - 5 < size_t(len) + 4) return NextResult::Truncated;
    c.data = d + pos + 5; c.size = len;
    Reader cr(c.data + len, 4);
    if (cr.u32() != util::Crc32(c.data, len)) return NextResult::Corrupt;
    pos += 5 + size_t(len) + 4;
    return NextResult::Ok;
}

inline std::vector<uint8_t> Encode(const FighterTable& t) {
    Writer w;
    w.u16(t.recId); w.u8(t.flags); w.str(t.name, kMaxStr); w.str(t.cls, kMaxStr);
    w.u16(uint16_t(t.bones.size() < kMaxBones ? t.bones.size() : kMaxBones));
    for (size_t i = 0; i < t.bones.size() && i < kMaxBones; ++i) {
        w.str(t.bones[i].name, kMaxStr); w.u16(t.bones[i].parent); w.u8(t.bones[i].hasBody ? 1 : 0);
    }
    return std::move(w.buf);
}
inline bool Decode(const uint8_t* d, size_t n, FighterTable& t) {
    Reader r(d, n);
    t.recId = r.u16(); t.flags = r.u8(); t.name = r.str(kMaxStr); t.cls = r.str(kMaxStr);
    const size_t count = r.u16();
    if (count > kMaxBones) return false;
    t.bones.assign(count, {});
    for (auto& b : t.bones) {
        b.name = r.str(kMaxStr); b.parent = r.u16();
        const uint8_t body = r.u8();
        if (body > 1) return false;
        b.hasBody = body == 1;
    }
    if (!r.done()) return false;
    for (const auto& b : t.bones) if (b.parent != kNoParent && b.parent >= count) return false;
    return true;
}

inline void PutWeapon(Writer& w, const WeaponFrame& f) {
    w.u8(f.present ? 1 : 0);
    if (!f.present) return;
    w.u32(f.classHash); detail::Put(w, f.pos); detail::Put(w, f.rot); detail::Put(w, f.vel); detail::Put(w, f.angVelDeg);
    detail::Put(w, f.base); detail::Put(w, f.tip);
}
inline bool GetWeapon(Reader& r, WeaponFrame& f) {
    const uint8_t present = r.u8();
    if (present > 1) return false;
    f = {};
    f.present = present == 1;
    if (!f.present) return true;
    f.classHash = r.u32(); f.pos = detail::GetF3(r); f.rot = detail::GetQ(r); f.vel = detail::GetF3(r);
    f.angVelDeg = detail::GetF3(r); f.base = detail::GetF3(r); f.tip = detail::GetF3(r);
    return r.ok();
}

// Self-contained: each bone carries its own body flag, so a reader needs no table to walk a frame.
inline std::vector<uint8_t> Encode(const FighterFrame& f) {
    Writer w;
    detail::F64(w, f.ts); w.f32(f.dt); w.u16(f.recId);
    const size_t count = f.bones.size() < kMaxBones ? f.bones.size() : kMaxBones;
    w.u16(uint16_t(count));
    for (size_t i = 0; i < count; ++i) {
        const BoneFrame& b = f.bones[i];
        detail::Put(w, b.pos); detail::Put(w, b.rot); w.u8(b.hasBody ? 1 : 0);
        if (b.hasBody) { detail::Put(w, b.vel); detail::Put(w, b.angVelDeg); }
    }
    for (const auto& wf : f.weapons) PutWeapon(w, wf);
    w.u16(f.grip);
    return std::move(w.buf);
}
inline bool Decode(const uint8_t* d, size_t n, FighterFrame& f) {
    Reader r(d, n);
    bool ok = true;
    f.ts = detail::F64(r, ok); f.dt = r.f32(); f.recId = r.u16();
    const size_t count = r.u16();
    if (count > kMaxBones) return false;
    f.bones.assign(count, {});
    for (auto& b : f.bones) {
        b.pos = detail::GetF3(r); b.rot = detail::GetQ(r);
        const uint8_t body = r.u8();
        if (body > 1) return false;
        b.hasBody = body == 1;
        if (b.hasBody) { b.vel = detail::GetF3(r); b.angVelDeg = detail::GetF3(r); }
        if (!r.ok()) return false;
    }
    for (auto& wf : f.weapons) if (!GetWeapon(r, wf)) return false;
    f.grip = r.u16();
    return ok && r.done();
}

inline std::vector<uint8_t> Encode(const Vitals& v) {
    Writer w;
    detail::F64(w, v.ts); w.u16(v.recId);
    w.f32(v.health); w.f32(v.consciousness); w.f32(v.stamina); w.f32(v.bleeding);
    const size_t parts = v.partHealth.size() < kMaxParts ? v.partHealth.size() : kMaxParts;
    w.u8(uint8_t(parts));
    for (size_t i = 0; i < parts; ++i) w.f32(v.partHealth[i]);
    w.u8(v.flags); w.u32(v.stateMask); w.u32(v.severMask); w.u32(v.dislocMask);
    return std::move(w.buf);
}
inline bool Decode(const uint8_t* d, size_t n, Vitals& v) {
    Reader r(d, n);
    bool ok = true;
    v.ts = detail::F64(r, ok); v.recId = r.u16();
    v.health = r.f32(); v.consciousness = r.f32(); v.stamina = r.f32(); v.bleeding = r.f32();
    const size_t parts = r.u8();
    if (parts > kMaxParts) return false;
    v.partHealth.assign(parts, 0.f);
    for (auto& p : v.partHealth) p = r.f32();
    v.flags = r.u8(); v.stateMask = r.u32(); v.severMask = r.u32(); v.dislocMask = r.u32();
    return ok && r.done();
}

inline std::vector<uint8_t> Encode(const PropTable& t) {
    Writer w;
    w.u64(t.propId); w.str(t.name, kMaxStr); w.str(t.cls, kMaxStr);
    const size_t count = t.bodies.size() < kMaxPropBodies ? t.bodies.size() : kMaxPropBodies;
    w.u8(uint8_t(count));
    for (size_t i = 0; i < count; ++i) w.str(t.bodies[i], kMaxStr);
    return std::move(w.buf);
}
inline bool Decode(const uint8_t* d, size_t n, PropTable& t) {
    Reader r(d, n);
    t.propId = r.u64(); t.name = r.str(kMaxStr); t.cls = r.str(kMaxStr);
    const size_t count = r.u8();
    if (count > kMaxPropBodies) return false;
    t.bodies.assign(count, {});
    for (auto& b : t.bodies) b = r.str(kMaxStr);
    return r.done();
}

inline std::vector<uint8_t> Encode(const PropFrame& f) {
    Writer w;
    detail::F64(w, f.ts); w.u64(f.propId); w.u8(f.body); w.u8(f.flags);
    detail::Put(w, f.pos); detail::Put(w, f.rot); detail::Put(w, f.vel); detail::Put(w, f.angVelDeg);
    return std::move(w.buf);
}
inline bool Decode(const uint8_t* d, size_t n, PropFrame& f) {
    Reader r(d, n);
    bool ok = true;
    f.ts = detail::F64(r, ok); f.propId = r.u64(); f.body = r.u8(); f.flags = r.u8();
    f.pos = detail::GetF3(r); f.rot = detail::GetQ(r); f.vel = detail::GetF3(r); f.angVelDeg = detail::GetF3(r);
    return ok && r.done();
}

inline std::vector<uint8_t> Encode(const Event& e) {
    Writer w;
    detail::F64(w, e.ts); w.u8(uint8_t(e.kind)); w.u16(e.subject); w.u16(e.other); w.u64(e.propId); w.u8(e.hand);
    w.str(e.text, kMaxStr);
    for (float v : e.x) w.f32(v);
    return std::move(w.buf);
}
inline bool Decode(const uint8_t* d, size_t n, Event& e) {
    Reader r(d, n);
    bool ok = true;
    e.ts = detail::F64(r, ok);
    const uint8_t kind = r.u8();
    if (kind == 0 || kind > kEvLast) return false;
    e.kind = Ev(kind);
    e.subject = r.u16(); e.other = r.u16(); e.propId = r.u64(); e.hand = r.u8();
    if (e.hand > 1 && e.hand != kNoHand) return false;
    e.text = r.str(kMaxStr);
    for (auto& v : e.x) v = r.f32();
    return ok && r.done();
}

inline std::vector<uint8_t> Encode(const InputSample& s) {
    Writer w;
    detail::F64(w, s.ts); w.u16(s.recId);
    const size_t count = s.axes.size() < kMaxAxes ? s.axes.size() : kMaxAxes;
    w.u8(uint8_t(count));
    for (size_t i = 0; i < count; ++i) w.f32(s.axes[i]);
    w.u32(s.buttons); w.f32(s.controlYaw); w.f32(s.controlPitch);
    return std::move(w.buf);
}
inline bool Decode(const uint8_t* d, size_t n, InputSample& s) {
    Reader r(d, n);
    bool ok = true;
    s.ts = detail::F64(r, ok); s.recId = r.u16();
    const size_t count = r.u8();
    if (count > kMaxAxes) return false;
    s.axes.assign(count, 0.f);
    for (auto& v : s.axes) v = r.f32();
    s.buttons = r.u32(); s.controlYaw = r.f32(); s.controlPitch = r.f32();
    return ok && r.done();
}

template <class T> inline void Append(std::vector<uint8_t>& out, Chunk kind, const T& v) { AppendChunk(out, kind, Encode(v)); }

// ---------------------------------------------------------------- events from field edges
// One fighter's gameplay fields in one sample, as the recorder reads them from the Willie. Index 0 is the right hand,
// 1 the left. Ids: a fighter's recId, a prop's ObjectSync id; 0 when unknown or none.
constexpr size_t kSheathSlots = 5;   // Willie_BP "Weapon Slot R 1", "R 2", "Back", "L 1", "L 2"

struct FighterState {
    std::array<bool, 2> grabbing{};              // "Grabbed R/L"
    std::array<uint16_t, 2> grabbedFighter{};    // whose body that hand holds
    std::array<uint64_t, 2> grabbedProp{};       // or which prop
    std::array<std::string, 2> grabBone{};       // "Grab Bone R/L"
    bool beingGrabbed = false;                   // "Being Grabbed"
    uint16_t grabbedBy = 0;                      // "Grabbed By Willie"
    std::array<bool, 2> holding{};               // "Weapon R/L" set
    std::array<uint64_t, 2> weaponProp{};        // that weapon's prop id
    std::array<uint32_t, 2> weaponClass{};       // its class hash (tells two weapons apart when the prop id is 0)
    uint16_t grip = 0;                           // sync::PackGrip
    std::array<bool, 2> sheathing{}, drawing{};  // "R/L Weapon Sheathing", "Weapon UnSheathing R/L"
    std::array<uint64_t, kSheathSlots> sheathed{};   // the prop ids in the sheath slots
    std::array<bool, 2> thrusting{}, kicking{}, parrying{}, stuck{};   // "R_Thrusting"..., "Kicking  R/L", "Parrying R/L", "R/L Hand Stuck"
    bool fallen = false, dead = false;
};

// PickUp/Drop carry the weapon's class hash in x[0] (low 16 bits) and x[1] (high 16 bits): exact in a float.
inline void ClassToX(Event& e, uint32_t h) { e.x[0] = float(h & 0xFFFF); e.x[1] = float(h >> 16); }
inline uint32_t ClassFromX(const Event& e) { return uint32_t(e.x[0]) | uint32_t(e.x[1]) << 16; }

inline bool InSheath(const FighterState& s, uint64_t propId) {
    return propId && std::find(s.sheathed.begin(), s.sheathed.end(), propId) != s.sheathed.end();
}

// Appends the events between two successive samples of the fighter `recId` at time `ts`. The recorder does not call
// it on a fighter's first sample (there is no edge yet: a weapon held at spawn is not a pick-up).
inline void EdgeEvents(const FighterState& prev, const FighterState& cur, double ts, uint16_t recId, std::vector<Event>& out) {
    auto ev = [&](Ev kind, uint8_t hand) -> Event& {
        Event e; e.ts = ts; e.kind = kind; e.subject = recId; e.hand = hand;
        out.push_back(std::move(e));
        return out.back();
    };
    for (uint8_t h = 0; h < 2; ++h) {
        const bool sameGrab = prev.grabbing[h] && cur.grabbing[h] && prev.grabbedFighter[h] == cur.grabbedFighter[h] &&
                              prev.grabbedProp[h] == cur.grabbedProp[h] && prev.grabBone[h] == cur.grabBone[h];
        if (prev.grabbing[h] && !sameGrab) {
            Event& e = ev(Ev::GrabOff, h);
            e.other = prev.grabbedFighter[h]; e.propId = prev.grabbedProp[h]; e.text = prev.grabBone[h];
        }
        if (cur.grabbing[h] && !sameGrab) {
            Event& e = ev(Ev::GrabOn, h);
            e.other = cur.grabbedFighter[h]; e.propId = cur.grabbedProp[h]; e.text = cur.grabBone[h];
        }
        const bool sameWeapon = prev.holding[h] && cur.holding[h] && prev.weaponProp[h] == cur.weaponProp[h] &&
                                prev.weaponClass[h] == cur.weaponClass[h];
        // A weapon put in a sheath or drawn from one is a Sheathe or Draw, not a Drop or PickUp: it is in a slot, or the
        // hand's sheathe or draw is running.
        const bool sheathed = InSheath(cur, prev.weaponProp[h]) || prev.sheathing[h] || cur.sheathing[h];
        const bool drawn = InSheath(prev, cur.weaponProp[h]) || prev.drawing[h] || cur.drawing[h];
        if (prev.holding[h] && !sameWeapon && !sheathed) {
            Event& e = ev(Ev::Drop, h);
            e.propId = prev.weaponProp[h]; ClassToX(e, prev.weaponClass[h]);
        }
        if (cur.holding[h] && !sameWeapon && !drawn) {
            Event& e = ev(Ev::PickUp, h);
            e.propId = cur.weaponProp[h]; ClassToX(e, cur.weaponClass[h]);
        }
        if (cur.sheathing[h] && !prev.sheathing[h]) ev(Ev::Sheathe, h).propId = cur.weaponProp[h];
        if (cur.drawing[h] && !prev.drawing[h]) ev(Ev::Draw, h).propId = cur.weaponProp[h];
        if (cur.thrusting[h] && !prev.thrusting[h]) ev(Ev::Thrust, h);
        if (cur.kicking[h] && !prev.kicking[h]) ev(Ev::Kick, h);
        if (cur.parrying[h] != prev.parrying[h]) ev(cur.parrying[h] ? Ev::ParryOn : Ev::ParryOff, h);
        if (cur.stuck[h] != prev.stuck[h]) ev(cur.stuck[h] ? Ev::StuckOn : Ev::StuckOff, h).propId = cur.weaponProp[h] ? cur.weaponProp[h] : prev.weaponProp[h];
    }
    const bool sameGrabber = prev.beingGrabbed && cur.beingGrabbed && prev.grabbedBy == cur.grabbedBy;
    if (prev.beingGrabbed && !sameGrabber) ev(Ev::BeingGrabbedOff, kNoHand).other = prev.grabbedBy;
    if (cur.beingGrabbed && !sameGrabber) ev(Ev::BeingGrabbedOn, kNoHand).other = cur.grabbedBy;
    if (cur.grip != prev.grip) {
        // Exact in a float: the packed grip is 15 bits.
        Event& e = ev(Ev::GripChange, kNoHand);
        e.x[0] = float(prev.grip); e.x[1] = float(cur.grip);
    }
    if (cur.fallen != prev.fallen) ev(cur.fallen ? Ev::Fall : Ev::GetUp, kNoHand);
    if (cur.dead && !prev.dead) ev(Ev::Death, kNoHand);
}

// The seven part healths in Vitals order (protocol VitalsMsg::partHealth).
inline constexpr std::array<const char*, 7> kPartNames = {"head", "upper body", "lower body", "arm R", "arm L", "leg R", "leg L"};
inline constexpr float kDamageMin = 0.5f;   // smaller drops are regeneration noise, not a hit

// Damage and Sever between two successive Vitals of one fighter (15 Hz, so one Damage can sum a few hits). Damage: one per
// part that dropped by kDamageMin or more, text = the part, x = {drop, new value, part index}. Sever: one per newly set bit
// of severMask, x0 = the bit. A heal or a cleared bit (a respawn) is no event.
inline void VitalsEvents(const Vitals& prev, const Vitals& cur, std::vector<Event>& out) {
    auto ev = [&](Ev kind) -> Event& {
        Event e; e.ts = cur.ts; e.kind = kind; e.subject = cur.recId; e.hand = kNoHand;
        out.push_back(std::move(e));
        return out.back();
    };
    const size_t parts = std::min({prev.partHealth.size(), cur.partHealth.size(), kPartNames.size()});
    for (size_t i = 0; i < parts; ++i) {
        const float drop = prev.partHealth[i] - cur.partHealth[i];
        if (!(drop >= kDamageMin)) continue;
        Event& e = ev(Ev::Damage);
        e.text = kPartNames[i]; e.x[0] = drop; e.x[1] = cur.partHealth[i]; e.x[2] = float(i);
    }
    const uint32_t added = cur.severMask & ~prev.severMask;
    for (uint32_t b = 0; b < 32; ++b)
        if (added >> b & 1u) ev(Ev::Sever).x[0] = float(b);
}

}  // namespace hsm::rec

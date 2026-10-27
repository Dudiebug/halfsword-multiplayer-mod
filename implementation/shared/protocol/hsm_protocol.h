// HalfSwordMP wire protocol (revision 5). Game-independent: no Unreal or generated SDK types.
// All integers are little-endian; floats are IEEE-754 binary32. Every decoder validates sizes,
// counts and finiteness and returns false on malformed input instead of trusting the sender.
#pragma once
#include "match/hsm_match.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hsm {

constexpr uint16_t kMagic = 0x4D48;          // "HM"
constexpr uint8_t kProtocolRevision = 19;         // 19: Spawn.life and victimLife (HitClaim, SeverClaim, DeathReport), PeerMap and MeshHeard (direct client-to-client Pose/Vitals), Welcome.flags; 18: ReportShare (in-game bug reports); 17: Kill flags 16/32, a drawn Duel round (both down / no hits); the pose carries each weapon's flail head (pose flags bits 2, 3, codec still v2); Hello and Welcome carry the mod build (session/hsm_build.h); MatchInfo marks fighters down (alive byte bit 1); 16: GiveUp (hold G concedes a Duel round); 15: PaintFx kinds Voice, HitSound, Armor, ArmorOn (voice lines, hit sounds, armour off/on, strip requests); 14: the pose codec is version 2, its header carries the hands' grip state (GripState); 13: a HitClaim may be struck by a prop; MatchInfo carries gear, tier, the Duel round and gap, fighters' ping and alive; 12: SeverNotice, PropCensus; 11: MatchInfo accepts every match format (Duel, Zombies); 10: PaintFx, Equip
constexpr uint16_t kDefaultPort = 27777;
constexpr size_t kMaxPlayerName = 32;
constexpr size_t kMaxModBuild = 64;   // Hello/Welcome modBuild bytes (session/hsm_build.h)
constexpr size_t kMaxWillies = 16;           // 6 fighters + up to 10 bots
constexpr size_t kMaxBodies = 64;
constexpr size_t kMaxPacket = 16 * 1024;

// ENet channels: reliable-ordered control/events, unreliable-sequenced input/state.
enum Channel : uint8_t { kChanReliable = 0, kChanUnreliable = 1, kChanCount = 2 };

enum class MsgType : uint8_t {
    Hello = 1,       // client -> host
    Welcome = 2,     // host -> client
    Reject = 3,      // host -> client
    Input = 4,       // client -> host, unreliable
    Snapshot = 5,    // host -> client, unreliable
    Event = 6,       // host -> client, reliable
    Ping = 7,
    Pong = 8,
    Travel = 9,      // host -> client, reliable: follow the host to a level
    Ready = 10,      // client -> host, reliable: level loaded
    Spawn = 11,      // host -> client, reliable: an entity exists (fighter/bot) with its loadout blob
    Despawn = 12,    // host -> client, reliable
    BlobChunk = 13,  // both ways, reliable: one piece of a loadout blob
    MatchInfo = 14,  // host -> client, reliable: match settings, phase, scores, teams
    Kill = 15,       // host -> client, reliable: kill feed entry
    Chat = 16,        // both ways, reliable: chat line (protocol/hsm_chat.h); host relays
    WorldState = 17,  // host -> client, unreliable: loose world objects (session/hsm_world.h)
    CombatEvent = 18, // host -> client, reliable: dismemberment / blood (session/hsm_combat.h)
    RelayInput = 19,  // host -> client, unreliable: one fighter's input frame (unused since owner authority)
    OwnerState = 20,  // client -> host, unreliable: the sender's own fighter as it simulates it (it is the authority)
    DeathReport = 21, // client -> host, reliable: the sender's fighter died, and who it says killed it
    Pose = 22,        // owner -> host -> others, unreliable: an owned fighter's pose (protocol/hsm_owner.h)
    Vitals = 23,      // owner -> host -> others, unreliable: an owned fighter's health, parts, dead/fallen/KO
    HitClaim = 24,    // attacker -> host -> victim's owner, reliable: a blow on a stand-in, to replay natively
    HitFx = 25,       // host -> everyone else, reliable: the same blow, shown cosmetically on their stand-in
    SeverClaim = 26,  // attacker -> host -> everyone, reliable: a sever started on a stand-in (combat/hsm_sever.h)
    GrabState = 27,   // grabber's owner -> host -> others: what its hands hold (reliable on change, unreliable refresh; sync/hsm_grab.h)
    PropState = 28,   // lessee -> host -> others, unreliable: a leased or FREE prop's transform (session/hsm_lease.h)
    StuckBlade = 29,  // attacker -> host -> victim's owner, reliable: my blade stuck in / pulled out of a stand-in
    PropLease = 30,   // both ways, reliable: a lease request/release, or the host's grant (session/hsm_lease.h)
    PaintFx = 31,     // owner -> host -> others, reliable: blood or a wound painted on an owned fighter (combat/hsm_fx.h)
    Equip = 32,       // owner -> host -> others, reliable: which sheathed weapons an owned fighter has drawn (combat/hsm_equip.h)
    SeverNotice = 33, // owner -> host -> others, reliable: a sever the owner's own fighter took natively (combat/hsm_sever.h)
    PropCensus = 34,  // host -> client, reliable: the level's physics props and their indices (session/hsm_prop_census.h)
    GiveUp = 35,      // client -> host, reliable: the sender's fighter concedes the Duel round (held G)
    ReportShare = 36, // reporter -> host -> others, reliable: a bug report's id and attach token (session/hsm_report.h)
    PeerMap = 37,     // host -> client, reliable: the other clients' slots and SteamIDs (mesh, session/hsm_mesh.h)
    MeshHeard = 38,   // client -> host, reliable: which other slots this client hears directly (mesh fallback)
};

// Analog inputs, mirroring DefaultInput.ini axis mappings.
enum class Axis : uint8_t {
    MoveForward, MoveRight, TurnMouse, LookMouse, TurnGamepad, LookGamepad,
    RightArm, LeftArm, RightGuard, LeftGuard, Count
};
// Digital inputs: DefaultInput.ini action mappings plus the raw RMB/MMB key events Willie binds
// (held state; press/release edges are derived on the host).
enum class Action : uint8_t {
    GrabLeft, GrabRight, GuardLeft, GuardRight, Thrust, ThrustGamepad, Jump, CrouchKey, CrouchHold,
    Run, SwapHands, ToggleCameraLock, KeyForward, KeyBack, KeyLeft, KeyRight, Talk,
    RightMouse, MiddleMouse, Count
};
constexpr size_t kAxisCount = static_cast<size_t>(Axis::Count);
constexpr size_t kActionCount = static_cast<size_t>(Action::Count);
static_assert(kActionCount <= 32, "action bitmask is 32 bits");

enum class RejectReason : uint8_t { BadRevision = 1, BuildMismatch = 2, Full = 3, Malformed = 4 };

enum class EventKind : uint8_t {
    Spawned = 1,       // a = entity, b = slot
    Despawned = 2,     // a = entity
    Dismembered = 3,   // a = entity, b = part enum value
    Died = 4,          // a = entity
    RoundReset = 5,
    MapChange = 6,     // text = map path
};

struct Hello {
    uint8_t revision = kProtocolRevision;
    uint64_t buildKey = 0;   // PE TimeDateStamp << 32 | SizeOfImage of the game exe
    std::string name;
    std::string modBuild;    // rev 17: the client's mod build (session/hsm_build.h ModBuildString)
};
struct Welcome {
    uint32_t sessionId = 0;
    uint8_t slot = 0;        // local-player index the host assigned to this client
    uint8_t entity = 0;      // entity id of the client's Willie
    uint16_t snapshotHz = 30;
    std::string map;
    uint8_t syncMode = 0;    // session/hsm_authority.h SyncMode: 0 the host simulates every fighter, 1 each owner its own
    std::string modBuild;    // rev 17: the host's mod build
    uint8_t flags = 0;       // rev 19: kWelcomeNoHostFighter | kWelcomeMesh
};
constexpr uint8_t kWelcomeNoHostFighter = 1;   // the host plays no fighter of its own
constexpr uint8_t kWelcomeMesh = 2;            // mesh offered: clients send Pose/Vitals to each other too (PeerMap follows)
struct Reject { RejectReason reason = RejectReason::Malformed; };

struct InputFrame {
    uint32_t seq = 0;
    uint32_t clientTimeMs = 0;
    std::array<float, kAxisCount> axes{};   // each clamped to [-kAxisLimit, kAxisLimit]
    uint32_t buttons = 0;                   // bit i = Action i held
    float controlYaw = 0, controlPitch = 0; // degrees
};
constexpr float kAxisLimit = 64.f;          // mouse deltas can exceed 1; anything beyond is rejected

struct Quat16 { int16_t x = 0, y = 0, z = 0, w = 32767; };
struct BodyState { float px = 0, py = 0, pz = 0; Quat16 rot; };
struct WillieState {
    uint8_t entity = 0;
    uint8_t flags = 0;           // bit0 dead, bit1 fallen, bit2 headless
    float health = 0, consciousness = 0, stamina = 0, bleeding = 0;
    std::array<float, 7> partHealth{};   // head, upper body, lower body, arm R, arm L, leg R, leg L
    uint32_t dismembered = 0;    // bit per Enum_DismembermentPart value
    std::vector<BodyState> bodies;       // order agreed by bone list in Welcome-time config
};
struct Snapshot {
    uint32_t tick = 0;
    uint32_t ackInputSeq = 0;
    std::vector<WillieState> willies;
};
struct Event {
    uint32_t seq = 0;
    EventKind kind = EventKind::RoundReset;
    uint32_t a = 0, b = 0;
    std::string text;
};
struct Ping { uint32_t timeMs = 0; };

// One game setting copied from host to client before travel (e.g. a GI_Settings field by name).
// Values are the raw little-endian bytes of a plain (bool/byte/enum/int/float/double) property.
constexpr size_t kMaxSettingName = 64, kMaxSettingBytes = 16, kMaxSettings = 64, kMaxLevelName = 256;
struct Setting { std::string name; std::vector<uint8_t> value; };
struct Travel {
    uint32_t seq = 0;           // increments per host level change
    std::string level;          // level name/path as passed to OpenLevel
    bool absolute = true;
    std::string options;
    std::vector<Setting> settings;
};
struct Ready { uint32_t travelSeq = 0; std::string level; };

enum class EntityKind : uint8_t { Fighter = 1, Bot = 2 };
struct Spawn {
    uint8_t entity = 0;
    EntityKind kind = EntityKind::Fighter;
    uint8_t ownerSlot = 0;      // roster slot of the controlling player (0xFF for host-owned bots)
    int8_t team = -1;           // -1 = free-for-all
    uint32_t blobId = 0;        // loadout blob carrying this entity's passport (0 = none)
    float x = 0, y = 0, z = 0, yaw = 0;   // spawn location (cm, degrees)
    uint8_t life = 0;           // rev 19: the host's count of this entity's bodies; a claim names the life it was made against (0 = unknown)
};
struct Despawn { uint8_t entity = 0; };

// Loadout blobs (SaveGame bytes holding one Str_Passport_Character1) travel as reliable chunks.
constexpr size_t kMaxBlobBytes = 256 * 1024, kBlobChunkBytes = 8 * 1024;
struct BlobChunk {
    uint32_t blobId = 0;
    uint32_t total = 0;          // whole blob size in bytes
    uint16_t index = 0, count = 0;
    uint32_t crc = 0;            // CRC-32 of the whole blob
    std::vector<uint8_t> data;
};

// Match (rev 5). Field ranges mirror hsm::Sanitize in match/hsm_match.h.
struct ScoreEntry { int16_t key = 0; int16_t score = 0; };          // key: team, or entity in free-for-all
struct FighterEntry {
    uint8_t entity = 0; int8_t team = -1; int16_t kills = 0, deaths = 0; std::string name;
    uint16_t pingMs = 0;                // round trip to the host, 0 = unknown (the host itself, bots)
    bool alive = true;
    bool down = false;                  // rev 17: Duel, the host's draw check has it down (match/hsm_draw.h DrawCheck::Down)
};
struct MatchInfo {
    uint8_t format = 0;                 // hsm::Format
    uint16_t scoreLimit = 5, timeLimitS = 0, respawnDelayS = 5;
    bool friendlyFire = false;
    uint8_t phase = 0;                  // hsm::MatchPhase
    uint32_t timeLeftMs = 0;
    int16_t winner = -1000;             // score key, -1000 = undecided
    uint8_t gear = 0, tier = 2;         // hsm::Gear, Enum_Ranks tier
    uint16_t round = 0;                 // Duel: the round being fought, or just won in the gap; 0 in other formats
    uint32_t roundGapLeftMs = 0;        // Duel: time before the next round, 0 outside the gap
    uint8_t roundWinner = 0;            // Duel's gap: the entity that took the round, 0 = drawn
    std::vector<ScoreEntry> scores;
    std::vector<FighterEntry> fighters;
};
struct Kill { uint8_t killer = 0, victim = 0; uint8_t flags = 0; };   // flags: 1 teamkill, 2 suicide, 4 environment, 8 gave up (Duel); 16 the round drawn with both down, 32 drawn with no hits (Duel: victim and killer are the two fighters)

// ---------------------------------------------------------------- byte stream helpers
class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
    void i16(int16_t v) { u16(uint16_t(v)); }
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
    void str(const std::string& s, size_t max) { size_t n = s.size() < max ? s.size() : max; u8(uint8_t(n)); buf.insert(buf.end(), s.begin(), s.begin() + long(n)); }
    void header(MsgType t) { u16(kMagic); u8(kProtocolRevision); u8(uint8_t(t)); }
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool ok() const { return ok_; }
    bool done() const { return ok_ && p_ == n_; }
    uint8_t u8() { if (!need(1)) return 0; return d_[p_++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = uint16_t(d_[p_] | d_[p_ + 1] << 8); p_ += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 3; i >= 0; --i) v = v << 8 | d_[p_ + size_t(i)]; p_ += 4; return v; }
    uint64_t u64() { uint64_t lo = u32(); uint64_t hi = u32(); return hi << 32 | lo; }
    int16_t i16() { return int16_t(u16()); }
    float f32() { uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); if (!std::isfinite(v)) ok_ = false; return v; }
    std::string str(size_t max) { size_t len = u8(); if (len > max || !need(len)) { ok_ = false; return {}; } std::string s(reinterpret_cast<const char*>(d_ + p_), len); p_ += len; return s; }
    bool header(MsgType& t) { if (u16() != kMagic) ok_ = false; if (u8() != kProtocolRevision) ok_ = false; t = MsgType(u8()); return ok_; }
private:
    bool need(size_t k) { if (!ok_ || n_ - p_ < k) { ok_ = false; return false; } return true; }
    const uint8_t* d_; size_t n_; size_t p_ = 0; bool ok_ = true;
};

// Peek the message type without decoding the body. Returns false on bad magic/revision.
inline bool PeekType(const uint8_t* d, size_t n, MsgType& t) { Reader r(d, n); return r.header(t); }

// ---------------------------------------------------------------- encoders / decoders
inline std::vector<uint8_t> Encode(const Hello& m) { Writer w; w.header(MsgType::Hello); w.u8(m.revision); w.u64(m.buildKey); w.str(m.name, kMaxPlayerName); w.str(m.modBuild, kMaxModBuild); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Hello& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Hello) return false;
    m.revision = r.u8(); m.buildKey = r.u64(); m.name = r.str(kMaxPlayerName); m.modBuild = r.str(kMaxModBuild); return r.done();
}

inline std::vector<uint8_t> Encode(const Welcome& m) { Writer w; w.header(MsgType::Welcome); w.u32(m.sessionId); w.u8(m.slot); w.u8(m.entity); w.u16(m.snapshotHz); w.str(m.map, 200); w.u8(m.syncMode); w.str(m.modBuild, kMaxModBuild); w.u8(m.flags); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Welcome& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Welcome) return false;
    m.sessionId = r.u32(); m.slot = r.u8(); m.entity = r.u8(); m.snapshotHz = r.u16(); m.map = r.str(200); m.syncMode = r.u8(); m.modBuild = r.str(kMaxModBuild); m.flags = r.u8();
    return r.done() && m.snapshotHz >= 1 && m.snapshotHz <= 120 && m.syncMode <= 1 && (m.flags & ~(kWelcomeNoHostFighter | kWelcomeMesh)) == 0;
}

inline std::vector<uint8_t> Encode(const Reject& m) { Writer w; w.header(MsgType::Reject); w.u8(uint8_t(m.reason)); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Reject& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Reject) return false;
    uint8_t v = r.u8(); if (v < 1 || v > 4) return false; m.reason = RejectReason(v); return r.done();
}

inline void PutInputBody(Writer& w, const InputFrame& m) {
    w.u32(m.seq); w.u32(m.clientTimeMs);
    for (float a : m.axes) w.f32(a);
    w.u32(m.buttons); w.f32(m.controlYaw); w.f32(m.controlPitch);
}
inline bool GetInputBody(Reader& r, InputFrame& m) {
    m.seq = r.u32(); m.clientTimeMs = r.u32();
    for (float& a : m.axes) { a = r.f32(); if (std::fabs(a) > kAxisLimit) return false; }
    m.buttons = r.u32(); m.controlYaw = r.f32(); m.controlPitch = r.f32();
    if (m.buttons >> kActionCount) return false;
    if (std::fabs(m.controlYaw) > 720.f || std::fabs(m.controlPitch) > 720.f) return false;
    return r.done();
}
inline std::vector<uint8_t> Encode(const InputFrame& m) { Writer w; w.header(MsgType::Input); PutInputBody(w, m); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, InputFrame& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Input) return false;
    return GetInputBody(r, m);
}

struct RelayInput { uint8_t entity = 0; InputFrame frame; };
inline std::vector<uint8_t> Encode(const RelayInput& m) { Writer w; w.header(MsgType::RelayInput); w.u8(m.entity); PutInputBody(w, m.frame); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, RelayInput& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::RelayInput) return false;
    m.entity = r.u8(); if (m.entity == 0 || m.entity > kMaxWillies) return false;
    return GetInputBody(r, m.frame);
}


inline void PutWillie(Writer& w, const WillieState& s) {
    w.u8(s.entity); w.u8(s.flags); w.f32(s.health); w.f32(s.consciousness); w.f32(s.stamina); w.f32(s.bleeding);
    for (float h : s.partHealth) w.f32(h);
    w.u32(s.dismembered); w.u8(uint8_t(s.bodies.size()));
    for (const BodyState& b : s.bodies) { w.f32(b.px); w.f32(b.py); w.f32(b.pz); w.i16(b.rot.x); w.i16(b.rot.y); w.i16(b.rot.z); w.i16(b.rot.w); }
}
inline bool GetWillie(Reader& r, WillieState& s) {
    s.entity = r.u8(); s.flags = r.u8(); s.health = r.f32(); s.consciousness = r.f32(); s.stamina = r.f32(); s.bleeding = r.f32();
    for (float& h : s.partHealth) h = r.f32();
    s.dismembered = r.u32(); size_t nb = r.u8(); if (nb > kMaxBodies) return false;
    s.bodies.resize(nb);
    for (BodyState& b : s.bodies) { b.px = r.f32(); b.py = r.f32(); b.pz = r.f32(); b.rot.x = r.i16(); b.rot.y = r.i16(); b.rot.z = r.i16(); b.rot.w = r.i16(); }
    return r.ok();
}

struct OwnerState { WillieState state; };
inline std::vector<uint8_t> Encode(const OwnerState& m) { Writer w; w.header(MsgType::OwnerState); PutWillie(w, m.state); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, OwnerState& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::OwnerState) return false;
    if (!GetWillie(r, m.state) || !r.done()) return false;
    return m.state.entity >= 1 && m.state.entity <= kMaxWillies;
}

struct DeathReport { uint8_t victim = 0, killer = 0; uint8_t victimLife = 0; };   // killer 0 = environment / unknown; victimLife: the Spawn.life of the body that died (rev 19)
inline std::vector<uint8_t> Encode(const DeathReport& m) { Writer w; w.header(MsgType::DeathReport); w.u8(m.victim); w.u8(m.killer); w.u8(m.victimLife); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, DeathReport& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::DeathReport) return false;
    m.victim = r.u8(); m.killer = r.u8(); m.victimLife = r.u8();
    return r.done() && m.victim >= 1 && m.victim <= kMaxWillies && m.killer <= kMaxWillies;
}
struct GiveUpMsg { uint8_t entity = 0; };
inline std::vector<uint8_t> Encode(const GiveUpMsg& m) { Writer w; w.header(MsgType::GiveUp); w.u8(m.entity); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, GiveUpMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::GiveUp) return false;
    m.entity = r.u8();
    return r.done() && m.entity >= 1 && m.entity <= kMaxWillies;
}
inline std::vector<uint8_t> Encode(const Snapshot& m) {
    Writer w; w.header(MsgType::Snapshot); w.u32(m.tick); w.u32(m.ackInputSeq); w.u8(uint8_t(m.willies.size()));
    for (const WillieState& s : m.willies) PutWillie(w, s);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, Snapshot& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Snapshot) return false;
    m.tick = r.u32(); m.ackInputSeq = r.u32(); size_t nw = r.u8(); if (nw > kMaxWillies) return false;
    m.willies.resize(nw);
    for (WillieState& s : m.willies) if (!GetWillie(r, s)) return false;
    return r.done();
}

inline std::vector<uint8_t> Encode(const Event& m) { Writer w; w.header(MsgType::Event); w.u32(m.seq); w.u8(uint8_t(m.kind)); w.u32(m.a); w.u32(m.b); w.str(m.text, 200); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Event& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Event) return false;
    m.seq = r.u32(); uint8_t k = r.u8(); if (k < 1 || k > 6) return false; m.kind = EventKind(k);
    m.a = r.u32(); m.b = r.u32(); m.text = r.str(200); return r.done();
}

inline std::vector<uint8_t> EncodePing(uint32_t ms, bool pong) { Writer w; w.header(pong ? MsgType::Pong : MsgType::Ping); w.u32(ms); return w.buf; }
inline bool DecodePing(const uint8_t* d, size_t n, Ping& m, bool& pong) {
    Reader r(d, n); MsgType t; if (!r.header(t) || (t != MsgType::Ping && t != MsgType::Pong)) return false;
    pong = t == MsgType::Pong; m.timeMs = r.u32(); return r.done();
}

inline std::vector<uint8_t> Encode(const Travel& m) {
    Writer w; w.header(MsgType::Travel); w.u32(m.seq); w.str(m.level, 255); w.u8(m.absolute ? 1 : 0); w.str(m.options, 255);
    size_t n = m.settings.size() < kMaxSettings ? m.settings.size() : kMaxSettings; w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) {
        const Setting& st = m.settings[i]; w.str(st.name, kMaxSettingName);
        size_t vb = st.value.size() < kMaxSettingBytes ? st.value.size() : kMaxSettingBytes; w.u8(uint8_t(vb));
        w.buf.insert(w.buf.end(), st.value.begin(), st.value.begin() + long(vb));
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, Travel& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Travel) return false;
    m.seq = r.u32(); m.level = r.str(255); uint8_t abs = r.u8(); if (abs > 1) return false; m.absolute = abs == 1;
    m.options = r.str(255); size_t cnt = r.u8(); if (cnt > kMaxSettings || m.level.empty()) return false;
    m.settings.resize(cnt);
    for (Setting& st : m.settings) {
        st.name = r.str(kMaxSettingName); size_t vb = r.u8(); if (vb > kMaxSettingBytes || st.name.empty()) return false;
        st.value.resize(vb); for (uint8_t& b : st.value) b = r.u8();
    }
    return r.done();
}
inline std::vector<uint8_t> Encode(const Ready& m) { Writer w; w.header(MsgType::Ready); w.u32(m.travelSeq); w.str(m.level, 255); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Ready& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Ready) return false;
    m.travelSeq = r.u32(); m.level = r.str(255); return r.done();
}

inline std::vector<uint8_t> Encode(const Spawn& m) {
    Writer w; w.header(MsgType::Spawn); w.u8(m.entity); w.u8(uint8_t(m.kind)); w.u8(m.ownerSlot); w.u8(uint8_t(m.team)); w.u32(m.blobId);
    w.f32(m.x); w.f32(m.y); w.f32(m.z); w.f32(m.yaw); w.u8(m.life); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, Spawn& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Spawn) return false;
    m.entity = r.u8(); uint8_t k = r.u8(); m.ownerSlot = r.u8(); m.team = int8_t(r.u8()); m.blobId = r.u32();
    m.x = r.f32(); m.y = r.f32(); m.z = r.f32(); m.yaw = r.f32(); m.life = r.u8();
    if (k < 1 || k > 2 || m.entity == 0 || m.team < -1 || m.team > 7) return false;
    if (std::fabs(m.x) > 1e7f || std::fabs(m.y) > 1e7f || std::fabs(m.z) > 1e7f || std::fabs(m.yaw) > 720.f) return false;
    m.kind = EntityKind(k); return r.done();
}
inline std::vector<uint8_t> Encode(const Despawn& m) { Writer w; w.header(MsgType::Despawn); w.u8(m.entity); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Despawn& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Despawn) return false;
    m.entity = r.u8(); return r.done() && m.entity != 0;
}
inline std::vector<uint8_t> Encode(const BlobChunk& m) {
    Writer w; w.header(MsgType::BlobChunk); w.u32(m.blobId); w.u32(m.total); w.u16(m.index); w.u16(m.count); w.u32(m.crc);
    w.u16(uint16_t(m.data.size())); w.buf.insert(w.buf.end(), m.data.begin(), m.data.end()); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, BlobChunk& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::BlobChunk) return false;
    m.blobId = r.u32(); m.total = r.u32(); m.index = r.u16(); m.count = r.u16(); m.crc = r.u32();
    size_t len = r.u16();
    if (m.blobId == 0 || m.total == 0 || m.total > kMaxBlobBytes || len == 0 || len > kBlobChunkBytes) return false;
    if (m.count != (m.total + kBlobChunkBytes - 1) / kBlobChunkBytes || m.index >= m.count) return false;
    size_t expect = m.index + 1u == m.count ? m.total - size_t(m.index) * kBlobChunkBytes : kBlobChunkBytes;
    if (len != expect) return false;
    m.data.resize(len); for (uint8_t& b : m.data) b = r.u8();
    return r.done();
}

inline std::vector<uint8_t> Encode(const MatchInfo& m) {
    Writer w; w.header(MsgType::MatchInfo);
    w.u8(m.format); w.u16(m.scoreLimit); w.u16(m.timeLimitS); w.u16(m.respawnDelayS); w.u8(m.friendlyFire ? 1 : 0);
    w.u8(m.phase); w.u32(m.timeLeftMs); w.i16(m.winner);
    w.u8(m.gear); w.u8(m.tier); w.u16(m.round); w.u32(m.roundGapLeftMs); w.u8(m.roundWinner);
    size_t ns = m.scores.size() < 255 ? m.scores.size() : 255; w.u8(uint8_t(ns));
    for (size_t i = 0; i < ns; ++i) { w.i16(m.scores[i].key); w.i16(m.scores[i].score); }
    size_t nf = m.fighters.size() < 255 ? m.fighters.size() : 255; w.u8(uint8_t(nf));
    for (size_t i = 0; i < nf; ++i) {
        const FighterEntry& f = m.fighters[i];
        w.u8(f.entity); w.u8(uint8_t(f.team)); w.i16(f.kills); w.i16(f.deaths); w.str(f.name, kMaxPlayerName); w.u16(f.pingMs);
        w.u8(uint8_t((f.alive ? 1 : 0) | (f.down ? 2 : 0)));   // bit 0 alive, bit 1 down
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, MatchInfo& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::MatchInfo) return false;
    m.format = r.u8(); m.scoreLimit = r.u16(); m.timeLimitS = r.u16(); m.respawnDelayS = r.u16();
    uint8_t ff = r.u8(); m.phase = r.u8(); m.timeLeftMs = r.u32(); m.winner = r.i16();
    m.gear = r.u8(); m.tier = r.u8(); m.round = r.u16(); m.roundGapLeftMs = r.u32(); m.roundWinner = r.u8();
    if (m.format >= kFormatCount || ff > 1 || m.phase > 4 || m.scoreLimit < 1 || m.scoreLimit > 100 || m.timeLimitS > 3600 || m.respawnDelayS > 60) return false;
    if (m.gear >= kGearCount || m.tier > 7) return false;
    m.friendlyFire = ff == 1;
    size_t ns = r.u8(); if (ns > kMaxWillies) return false;
    m.scores.resize(ns); for (ScoreEntry& e : m.scores) { e.key = r.i16(); e.score = r.i16(); }
    size_t nf = r.u8(); if (nf > kMaxWillies) return false;
    m.fighters.resize(nf);
    for (FighterEntry& f : m.fighters) {
        f.entity = r.u8(); f.team = int8_t(r.u8()); f.kills = r.i16(); f.deaths = r.i16(); f.name = r.str(kMaxPlayerName);
        f.pingMs = r.u16(); uint8_t alive = r.u8(); f.alive = (alive & 1) != 0; f.down = (alive & 2) != 0;
        if (f.entity == 0 || f.team < -1 || f.team > 7) return false;
        if (alive > 3) return false;
    }
    return r.done();
}
inline std::vector<uint8_t> Encode(const Kill& m) { Writer w; w.header(MsgType::Kill); w.u8(m.killer); w.u8(m.victim); w.u8(m.flags); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Kill& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Kill) return false;
    m.killer = r.u8(); m.victim = r.u8(); m.flags = r.u8();
    return r.done() && m.victim != 0 && m.flags < 64;
}

// Mesh (rev 19). The host tells each client who else is in the session so clients can stream Pose/Vitals to each other;
// a client tells the host which slots it hears directly so the host stops relaying those to it.
constexpr size_t kMaxPeerMap = 6;   // == kMaxFighters (session/hsm_roster.h; static_assert in session/hsm_authority.h)
struct PeerMapEntry { uint8_t slot = 0; uint64_t steamId = 0; };
struct PeerMapMsg { std::vector<PeerMapEntry> peers; };
inline std::vector<uint8_t> Encode(const PeerMapMsg& m) {
    Writer w; w.header(MsgType::PeerMap);
    const size_t n = m.peers.size() < kMaxPeerMap ? m.peers.size() : kMaxPeerMap; w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) { w.u8(m.peers[i].slot); w.u64(m.peers[i].steamId); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, PeerMapMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::PeerMap) return false;
    const size_t cnt = r.u8(); if (cnt > kMaxPeerMap) return false;
    m.peers.assign(cnt, PeerMapEntry{});
    uint32_t seen = 0;
    for (PeerMapEntry& e : m.peers) {
        e.slot = r.u8(); e.steamId = r.u64();
        if (e.slot >= kMaxPeerMap || e.steamId == 0 || (seen >> e.slot & 1)) return false;
        seen |= 1u << e.slot;
    }
    return r.done();
}
struct MeshHeardMsg { uint8_t mask = 0; };   // bit s = this client received slot s's Pose or Vitals directly lately
inline std::vector<uint8_t> Encode(const MeshHeardMsg& m) { Writer w; w.header(MsgType::MeshHeard); w.u8(m.mask); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, MeshHeardMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::MeshHeard) return false;
    m.mask = r.u8(); return r.done() && (m.mask >> kMaxPeerMap) == 0;
}

// ---------------------------------------------------------------- quantization
inline Quat16 PackQuat(float x, float y, float z, float w) {
    float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(len > 1e-6f) || !std::isfinite(len)) return {};
    if (w < 0) { x = -x; y = -y; z = -z; w = -w; }   // canonical hemisphere
    auto q = [len](float v) { float s = v / len * 32767.f; return int16_t(s > 32767.f ? 32767 : s < -32767.f ? -32767 : std::lround(s)); };
    return { q(x), q(y), q(z), q(w) };
}
inline void UnpackQuat(const Quat16& p, float& x, float& y, float& z, float& w) {
    x = p.x / 32767.f; y = p.y / 32767.f; z = p.z / 32767.f; w = p.w / 32767.f;
    float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 1e-6f) { x /= len; y /= len; z /= len; w /= len; } else { x = y = z = 0; w = 1; }
}

// ---------------------------------------------------------------- sequence helpers
// True if a is newer than b with 32-bit wraparound.
inline bool SeqNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

}  // namespace hsm

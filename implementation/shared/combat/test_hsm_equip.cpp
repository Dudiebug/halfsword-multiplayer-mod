// Unit tests for hsm_equip.h. Run via implementation/shared/run_tests.sh test_hsm_equip.
//
// Wire layout is written out independently below (Wire()), in the order hsm_equip.h documents:
//   header(Equip) entity:u8 seq:u16 handR:u8 handL:u8 sheathed:u8
#include "combat/hsm_equip.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;
using hsm::MsgType;

namespace {

EquipStateMsg State(uint8_t r, uint8_t l, uint8_t sheathed) {
    EquipStateMsg m;
    m.entity = 4; m.seq = 513; m.hand = {r, l}; m.sheathed = sheathed;
    return m;
}

std::vector<uint8_t> Wire(uint8_t entity, uint16_t seq, uint8_t r, uint8_t l, uint8_t sheathed, MsgType type = MsgType::Equip) {
    hsm::Writer w;
    w.header(type);
    w.u8(entity); w.u16(seq); w.u8(r); w.u8(l); w.u8(sheathed);
    return w.buf;
}

bool Dec(const std::vector<uint8_t>& b, EquipStateMsg& out) { return Decode(b.data(), b.size(), out); }
bool Dec(const std::vector<uint8_t>& b) { EquipStateMsg o; return Dec(b, o); }

void TestRoundTrip() {
    const EquipStateMsg ms[] = {State(1, 4, 0b10110), State(0, 0, 0), State(5, 3, 0b01011), State(3, 0, 0b11011)};
    for (const EquipStateMsg& m : ms) {
        std::vector<uint8_t> b = Encode(m);
        CHECK(b == Wire(m.entity, m.seq, m.hand[0], m.hand[1], m.sheathed));
        EquipStateMsg o;
        CHECK(Dec(b, o));
        CHECK(o.entity == m.entity && o.seq == m.seq && o.hand == m.hand && o.sheathed == m.sheathed);
    }
}

void TestFraming() {
    std::vector<uint8_t> b = Wire(4, 1, 1, 0, 0);
    CHECK(Dec(b));
    CHECK(!Dec(Wire(4, 1, 1, 0, 0, MsgType::PaintFx)));   // same body, another type: negative control
    std::vector<uint8_t> shortB(b.begin(), b.end() - 1);
    CHECK(!Dec(shortB));
    std::vector<uint8_t> longB = b; longB.push_back(0);
    CHECK(!Dec(longB));
    EquipStateMsg o;
    CHECK(!Decode(b.data(), 0, o));
}

void TestRanges() {
    CHECK(!Dec(Wire(0, 1, 0, 0, 0)));
    CHECK(Dec(Wire(1, 1, 0, 0, 0)));
    CHECK(Dec(Wire(16, 1, 0, 0, 0)));
    CHECK(!Dec(Wire(17, 1, 0, 0, 0)));
    CHECK(Dec(Wire(2, 1, 5, 0, 0)));
    CHECK(!Dec(Wire(2, 1, 6, 0, 0)));                      // tokens are 0..5
    CHECK(Dec(Wire(2, 1, 0, 5, 0)));
    CHECK(!Dec(Wire(2, 1, 0, 6, 0)));
    CHECK(!Dec(Wire(2, 1, 2, 2, 0)));                      // one slot's weapon in both hands
    CHECK(Dec(Wire(2, 1, 2, 3, 0)));
    CHECK(Dec(Wire(2, 1, 0, 0, 31)));
    CHECK(!Dec(Wire(2, 1, 0, 0, 32)));                     // five slots
    CHECK(!Dec(Wire(2, 1, 1, 0, 0b00001)));                // drawn from slot 0 and still sheathed there
    CHECK(!Dec(Wire(2, 1, 0, 5, 0b10000)));
    CHECK(Dec(Wire(2, 1, 1, 5, 0b01110)));
    // A rejected decode leaves the output untouched.
    EquipStateMsg out = State(3, 0, 0b00001);
    CHECK(!Dec(Wire(2, 9, 6, 0, 0), out));
    CHECK(out.entity == 4 && out.seq == 513 && out.hand[0] == 3 && out.sheathed == 1);
}

bool Tok(const EquipStateMsg& m, uint8_t r, uint8_t l, uint8_t sheathed) { return m.hand[0] == r && m.hand[1] == l && m.sheathed == sheathed; }

void TestTracker() {
    int a, b, c, d, e, f;   // actor identities
    const std::array<bool, kEquipSlots> all{true, true, true, true, true};
    EquipTracker t;
    EquipSnapshot s;
    s.slots = {&a, &b, &c, &d, &e};
    s.constrained = all;
    EquipStateMsg m = t.Read(s);
    CHECK(Tok(m, 0, 0, 0b11111));
    CHECK(m.entity == 0 && m.seq == 0);
    // A draw of c (slot 2) into the right hand: grabbed while still sheathed (mid-draw: other, still sheathed) ...
    s.hands = {&c, nullptr};
    CHECK(Tok(t.Read(s), 0, 0, 0b11111));
    // ... then pulled out: constraint and slot variable gone, the tracker remembers where it came from.
    s.slots = {&a, &b, nullptr, &d, &e};
    s.constrained = {true, true, false, true, true};
    CHECK(Tok(t.Read(s), 3, 0, 0b11011));
    CHECK(Tok(t.Read(s), 3, 0, 0b11011));                  // stays so
    // d (slot 3) into the left hand the same way.
    s.hands = {&c, &d};
    s.slots = {&a, &b, nullptr, nullptr, &e};
    s.constrained = {true, true, false, false, true};
    CHECK(Tok(t.Read(s), 3, 4, 0b10011));
    // c put back into slot 2: sheathed again while still in hand (mid-sheathe: other), then let go.
    s.slots = {&a, &b, &c, nullptr, &e};
    s.constrained = {true, true, true, false, true};
    CHECK(Tok(t.Read(s), 0, 4, 0b10111));
    s.hands = {nullptr, &d};
    CHECK(Tok(t.Read(s), 0, 4, 0b10111));
    // ... and drawn again, this time into the left hand after d was dropped (d is nowhere: forgotten).
    s.hands = {nullptr, nullptr};
    CHECK(Tok(t.Read(s), 0, 0, 0b10111));
    s.hands = {&d, nullptr};                               // d picked up again: other
    CHECK(Tok(t.Read(s), 0, 0, 0b10111));
    s.hands = {&d, &c};
    s.slots = {&a, &b, nullptr, nullptr, &e};
    s.constrained = {true, true, false, false, true};
    CHECK(Tok(t.Read(s), 0, 3, 0b10011));
    // A slot refilled with another sheathed weapon is no longer the origin of what came out of it.
    s.slots = {&a, &b, &f, nullptr, &e};
    s.constrained = {true, true, true, false, true};
    CHECK(Tok(t.Read(s), 0, 0, 0b10111));
    s.slots = {&a, &b, nullptr, nullptr, &e};               // f taken out again: c stays other
    s.constrained = {true, true, false, false, true};
    CHECK(Tok(t.Read(s), 0, 0, 0b10011));

    // Without history: an actor in a slot variable without its constraint counts as drawn from it.
    EquipTracker u;
    s.hands = {&c, &d};
    s.slots = {&a, &b, &c, &d, &e};
    s.constrained = {true, true, true, false, true};
    CHECK(Tok(u.Read(s), 0, 4, 0b10111));
    // Something never sheathed (fists, a pickup) is other; an empty slot is not sheathed even with a constraint flag.
    EquipTracker v;
    s.hands = {&f, nullptr};
    s.slots = {&a, nullptr, &c, &d, &e};
    s.constrained = {true, true, true, true, false};
    CHECK(Tok(v.Read(s), 0, 0, 0b01101));
    // A null hand never matches a null slot.
    s.hands = {nullptr, nullptr};
    s.constrained = {true, false, true, true, true};
    CHECK(Tok(v.Read(s), 0, 0, 0b11101));
    // One weapon in both hands counts for the right.
    EquipTracker w;
    s.slots = {&a, &b, &c, &d, &e};
    s.constrained = all;
    w.Read(s);
    s.hands = {&e, &e};
    s.slots = {&a, &b, &c, &d, nullptr};
    s.constrained = {true, true, true, true, false};
    CHECK(Tok(w.Read(s), 5, 0, 0b01111));
    s.hands = {nullptr, &e};                               // the left hand alone
    CHECK(Tok(w.Read(s), 0, 5, 0b01111));
    // An actor sheathed in two slots counts as the lower one once drawn.
    EquipTracker x;
    s.hands = {nullptr, nullptr};
    s.slots = {&a, &e, &c, &d, &e};
    s.constrained = all;
    CHECK(Tok(x.Read(s), 0, 0, 0b11111));
    s.hands = {nullptr, &e};
    s.slots = {&a, nullptr, &c, &d, nullptr};
    s.constrained = {true, false, true, true, false};
    CHECK(Tok(x.Read(s), 0, 2, 0b01101));
    // Two hands never share a token: a drawn weapon's origin slot later shown (without constraint) holding another.
    EquipTracker y;
    s.hands = {nullptr, nullptr};
    s.slots = {&a, &b, &c, &d, &e};
    s.constrained = all;
    y.Read(s);
    s.hands = {&a, nullptr};
    s.slots = {nullptr, &b, &c, &d, &e};
    s.constrained = {false, true, true, true, true};
    CHECK(Tok(y.Read(s), 1, 0, 0b11110));
    s.hands = {&a, &f};
    s.slots = {&f, &b, &c, &d, &e};
    m = y.Read(s);
    CHECK(Tok(m, 1, 0, 0b11110));
    m.entity = 1;
    CHECK(Dec(Encode(m)));
    // Clear forgets every origin.
    s.hands = {nullptr, nullptr};
    s.slots = {&a, &b, &c, &d, &e};
    s.constrained = all;
    t.Read(s);
    t.Clear();
    s.hands = {nullptr, &c};
    s.slots = {&a, &b, nullptr, nullptr, &e};
    s.constrained = {true, true, false, false, true};
    CHECK(Tok(t.Read(s), 0, 0, 0b10011));
}

bool Is(const EquipStep& s, EquipStep::Kind k, int hand, int slot) { return s.kind == k && s.hand == hand && s.slot == slot; }
constexpr auto kSheathe = EquipStep::Kind::Sheathe;
constexpr auto kDraw = EquipStep::Kind::Draw;
const std::array<bool, kEquipSlots> kAll{true, true, true, true, true};

void TestPlan() {
    // Matching states: nothing to do.
    CHECK(PlanEquip(State(1, 4, 0b10110), State(1, 4, 0b10110), kAll).empty());
    CHECK(PlanEquip(State(0, 0, 0b11111), State(0, 0, 0b11111), kAll).empty());
    // The owner drew slot R 2 into the right hand.
    std::vector<EquipStep> p = PlanEquip(State(2, 0, 0b11101), State(0, 0, 0b11111), kAll);
    CHECK(p.size() == 1 && Is(p[0], kDraw, 0, 1));
    // ... and the back slot into the left.
    p = PlanEquip(State(0, 3, 0b11011), State(0, 0, 0b11111), kAll);
    CHECK(p.size() == 1 && Is(p[0], kDraw, 1, 2));
    // The owner put it back.
    p = PlanEquip(State(0, 0, 0b11111), State(2, 0, 0b11101), kAll);
    CHECK(p.size() == 1 && Is(p[0], kSheathe, 0, 1));
    // Hand empty on the owner but the slot not sheathed (dropped): the stand-in keeps it for the prop path.
    CHECK(PlanEquip(State(0, 0, 0b11101), State(2, 0, 0b11101), kAll).empty());
    // A swap: sheathe the old one first, then draw the new one; right hand before left.
    p = PlanEquip(State(4, 1, 0b00110), State(1, 4, 0b00110), kAll);
    CHECK(p.empty());   // both drawn, but the owner sheathed neither: no Sheathe, and each Draw finds the other hand holding it
    p = PlanEquip(State(3, 2, 0b11001), State(2, 3, 0b11001), kAll);
    CHECK(p.empty());
    p = PlanEquip(State(5, 2, 0b00101), State(1, 3, 0b11010), kAll);
    CHECK(p.size() == 4);
    CHECK(p.size() == 4 && Is(p[0], kSheathe, 0, 0) && Is(p[1], kSheathe, 1, 2) && Is(p[2], kDraw, 0, 4) && Is(p[3], kDraw, 1, 1));
    // Draw needs the stand-in to have that slot's actor.
    std::array<bool, kEquipSlots> noR2 = kAll; noR2[1] = false;
    CHECK(PlanEquip(State(2, 0, 0b11101), State(0, 0, 0b11101), noR2).empty());
    p = PlanEquip(State(2, 4, 0b10101), State(0, 0, 0b11101), noR2);
    CHECK(p.size() == 1 && Is(p[0], kDraw, 1, 3));
    // Sheathe needs the owner to have it sheathed; the stand-in's own sheathed bits do not matter.
    p = PlanEquip(State(0, 0, 0b00001), State(1, 0, 0b11110), kAll);
    CHECK(p.size() == 1 && Is(p[0], kSheathe, 0, 0));
    // The owner holds something else (other) in that hand and has slot 0 sheathed: put back, no draw.
    p = PlanEquip(State(0, 0, 0b00001), State(1, 0, 0), kAll);
    CHECK(p.size() == 1 && Is(p[0], kSheathe, 0, 0));
    // Left hand Sheathe.
    p = PlanEquip(State(0, 0, 0b10000), State(0, 5, 0), kAll);
    CHECK(p.size() == 1 && Is(p[0], kSheathe, 1, 4));
}

void TestSender() {
    EquipSender s;
    EquipStateMsg a = State(1, 0, 0b11110);
    CHECK(s.Due(a, 1000));                          // first
    CHECK(!s.Due(a, 1001));
    CHECK(!s.Due(a, 1000 + kEquipResendMs - 1));
    CHECK(s.Due(a, 1000 + kEquipResendMs));         // periodic resend
    CHECK(!s.Due(a, 1000 + kEquipResendMs + 1));
    EquipStateMsg b = a; b.hand[1] = 4;
    CHECK(s.Due(b, 3001));                          // changed
    CHECK(!s.Due(b, 3002));
    EquipStateMsg c = b; c.sheathed = 0b10110;
    CHECK(s.Due(c, 3003));
    EquipStateMsg d = c; d.hand[0] = 0;
    CHECK(s.Due(d, 3004));
    EquipStateMsg e = d; e.seq = 9;
    CHECK(!s.Due(e, 3005));                         // seq alone is not a change
    EquipStateMsg other = d; other.entity = 5;
    CHECK(s.Due(other, 3006));                      // per fighter
    CHECK(!s.Due(d, 3007));
    s.Forget(4);
    CHECK(s.Due(d, 3008));                          // forgotten: first again
    CHECK(!s.Due(other, 3009));                     // 5 kept
}

}  // namespace

int main() {
    TestRoundTrip();
    TestFraming();
    TestRanges();
    TestTracker();
    TestPlan();
    TestSender();
    if (g_fail) { std::printf("test_hsm_equip: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_equip: ok\n");
    return 0;
}

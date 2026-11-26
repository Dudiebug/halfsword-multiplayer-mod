// Unit tests for hsm_relay.h. Run via implementation/shared/run_tests.sh test_hsm_relay.
#include "session/hsm_relay.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;

static bool IsStart(size_t off) {
    for (size_t s : kRelayInstructionStarts) if (s == off) return true;
    return false;
}

// A hooked function as MinHook leaves it: `E9 rel32` at the function's start, the relay 0x40 bytes past it.
struct Hooked {
    static constexpr uint64_t kBase = 0x7FF600000000ULL;
    static constexpr uint64_t kFn = kBase + 0x100, kRelay = kBase + 0x140;
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x200, 0x90);
    explicit Hooked(const RelayBytes& relay) {
        const int32_t rel = int32_t(kRelay - (kFn + 5));
        mem[0x100] = 0xE9;
        for (int i = 0; i < 4; ++i) mem[0x101 + size_t(i)] = uint8_t(uint32_t(rel) >> (8 * i));
        for (size_t i = 0; i < relay.size(); ++i) mem[0x140 + i] = relay[i];
    }
    uint64_t Resolve(uint64_t from) const { return Ue4ssResolve(mem.data(), mem.size(), kBase, from); }
};

int main() {
    const uint64_t detours[] = {0x00007FF612345678ULL, 0x0000000140E9EB25ULL, 0x00007FF6000000E9ULL, 0xFFFFFFFFFFFFFFFFULL, 0};

    for (uint64_t d : detours) {
        const RelayBytes r = MakeRelay(d);
        CHECK(r.size() == 14 && sizeof(r) == HSM_RELAY_SIZE);
        // mov rax, imm64 ; jmp rax ; int3 ; int3
        CHECK(r[0] == 0x48 && r[1] == 0xB8);
        uint64_t imm = 0;
        for (int i = 0; i < 8; ++i) imm |= uint64_t(r[2 + size_t(i)]) << (8 * i);
        CHECK(imm == d);
        CHECK(r[10] == 0xFF && r[11] == 0xE0 && r[12] == 0xCC && r[13] == 0xCC);

        // The reported failure: the stock relay is resolved to its operand, relay+6, the middle of the address.
        const RelayBytes old = MakeLegacyRelay(d);
        CHECK(old[0] == 0xFF && old[1] == 0x25 && old[2] == 0 && old[3] == 0 && old[4] == 0 && old[5] == 0);
        uint64_t oldImm = 0;
        for (int i = 0; i < 8; ++i) oldImm |= uint64_t(old[6 + size_t(i)]) << (8 * i);
        CHECK(oldImm == d);
        const Hooked oldHook(old);
        CHECK(oldHook.Resolve(Hooked::kFn) == Hooked::kRelay + 6);
        CHECK(oldHook.Resolve(Hooked::kRelay) == Hooked::kRelay + 6);
        CHECK(!IsStart(size_t(oldHook.Resolve(Hooked::kFn) - Hooked::kRelay)));

        // The new relay: UE4SS ends on the relay's first instruction, code that goes to the detour.
        const Hooked hook(r);
        const uint64_t at = hook.Resolve(Hooked::kFn);
        CHECK(at >= Hooked::kRelay && at < Hooked::kRelay + HSM_RELAY_SIZE);
        CHECK(IsStart(size_t(at - Hooked::kRelay)));
        CHECK(at != Hooked::kRelay + 6);
        CHECK(at == Hooked::kRelay);
        CHECK(hook.Resolve(Hooked::kRelay) == Hooked::kRelay);
    }

    // The resolver's own rules.
    {
        std::vector<uint8_t> m(0x40, 0x90);
        const uint64_t b = 0x1000;
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b + 4) == b + 4);                      // a nop: stop
        m[0] = 0xEB; m[1] = 0x0A;                                                        // jmp short +10 -> 0x0C
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b) == b + 0x0C);
        m[0] = 0xEB; m[1] = 0xFE;                                                        // jmp to itself: bounded loop
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b) == b);
        m[0] = 0xE9; m[1] = 0xFB; m[2] = 0xFF; m[3] = 0xFF; m[4] = 0xFF;                 // jmp to itself via rel32
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b) == b);
        m[0] = 0xFF; m[1] = 0x25; m[2] = 0x10; m[3] = 0; m[4] = 0; m[5] = 0;             // jmp [rip+0x10]: operand address
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b) == b + 6 + 0x10);
        m[0] = 0xE9; m[1] = 0x00; m[2] = 0x10; m[3] = 0; m[4] = 0;                       // a jump out of the buffer: returned as is
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b) == b + 5 + 0x1000);
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b + 0x100) == b + 0x100);              // not in the buffer
        m[0x3F] = 0xE9;                                                                  // truncated jump at the end: stop
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b + 0x3F) == b + 0x3F);
        m[0x3E] = 0xFF; m[0x3F] = 0x25;
        CHECK(Ue4ssResolve(m.data(), m.size(), b, b + 0x3E) == b + 0x3E);
    }

    if (g_fail) { std::printf("%d failed\n", g_fail); return 1; }
    std::printf("hsm_relay: ok\n");
    return 0;
}

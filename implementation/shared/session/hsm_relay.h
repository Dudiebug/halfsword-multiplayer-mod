// The relay MinHook puts between a hooked function's jump and HSMMod's detour, and the rule that decides its form.
//
// A hooked x64 function starts with E9 rel32 to the relay (within 2 GB), and the relay jumps on to the detour. MinHook's
// stock relay is `FF 25 00000000 <imm64>` (jmp [rip+0], the address stored behind it). UE4SS, loaded after HSMMod's hooks
// (ue4ss_order=after), follows jumps from a function's start to find "the real function" before it hooks or calls it:
// RC::ASM::resolve_function_address_from_potential_jmp. For `jmp [rip+disp]` it returns the operand's address
// (relay+6, where the imm64 sits) instead of reading the pointer, so UE4SS then calls those eight bytes of address as
// code (public #3172: faults just past relay+6, `?+0x7FF...0FDA` and up) or puts its own jump over them, and the next
// call through the relay jumps to whatever that left there (a fault at the relay itself, `0FD4` or `0F53`). The relay below is `mov rax, imm64 ; jmp rax ; int3 ; int3`: the resolver stops at the
// mov, so UE4SS uses the relay itself, real code that leads to the detour. rax is volatile and holds no argument at a
// function's first instruction (Windows x64), and every function HSMMod hooks is entered at its first instruction.
//
// The header is plain C as well as C++20: MinHook's trampoline.c writes the relay with hsm_relay_write, so the bytes the
// test checks are the bytes that ship. hook_relay=legacy in [mod] brings the stock relay back.
#pragma once
#include <stdint.h>
#include <stddef.h>

#define HSM_RELAY_SIZE 14   // both forms: the room MinHook reserves behind a trampoline (sizeof JMP_ABS)

// Stock MinHook relay: FF 25 00 00 00 00 <imm64>.
static inline void hsm_relay_write_legacy(uint8_t* out, uint64_t detour) {
    out[0] = 0xFF; out[1] = 0x25; out[2] = out[3] = out[4] = out[5] = 0;
    for (int i = 0; i < 8; ++i) out[6 + i] = (uint8_t)(detour >> (8 * i));
}

// 48 B8 <imm64> ; FF E0 ; CC CC.
static inline void hsm_relay_write(uint8_t* out, uint64_t detour) {
    out[0] = 0x48; out[1] = 0xB8;
    for (int i = 0; i < 8; ++i) out[2 + i] = (uint8_t)(detour >> (8 * i));
    out[10] = 0xFF; out[11] = 0xE0; out[12] = 0xCC; out[13] = 0xCC;
}

#ifdef __cplusplus
#include <array>

namespace hsm {

using RelayBytes = std::array<uint8_t, HSM_RELAY_SIZE>;

// Offsets at which an instruction of the relay starts (and its end): mov rax,imm64 | jmp rax | int3 | int3.
inline constexpr size_t kRelayInstructionStarts[] = {0, 10, 12, 13, 14};

inline RelayBytes MakeRelay(uint64_t detour) {
    RelayBytes r{};
    hsm_relay_write(r.data(), detour);
    return r;
}

inline RelayBytes MakeLegacyRelay(uint64_t detour) {
    RelayBytes r{};
    hsm_relay_write_legacy(r.data(), detour);
    return r;
}

// Where UE4SS's address resolver ends up when handed `addr`, with `code` holding the process's bytes from `base` on.
// It follows E9 rel32 and EB rel8; for FF 25 (jmp [rip+disp32]) it returns the operand's address, not the pointer
// stored there (the bug); at any other instruction it stops, returning that instruction's address. A jump that leaves
// `code` is returned as is: nothing is known about the bytes there.
inline uint64_t Ue4ssResolve(const uint8_t* code, size_t len, uint64_t base, uint64_t addr) {
    for (int step = 0; step < 16; ++step) {
        if (addr < base || addr - base >= len) return addr;
        const size_t off = size_t(addr - base);
        const size_t left = len - off;
        if (code[off] == 0xE9 && left >= 5) {
            int32_t rel = 0;
            for (int i = 0; i < 4; ++i) rel = int32_t(uint32_t(rel) | uint32_t(code[off + 1 + size_t(i)]) << (8 * i));
            addr = addr + 5 + uint64_t(int64_t(rel));
        } else if (code[off] == 0xEB && left >= 2) {
            addr = addr + 2 + uint64_t(int64_t(int8_t(code[off + 1])));
        } else if (code[off] == 0xFF && left >= 6 && code[off + 1] == 0x25) {
            int32_t disp = 0;
            for (int i = 0; i < 4; ++i) disp = int32_t(uint32_t(disp) | uint32_t(code[off + 2 + size_t(i)]) << (8 * i));
            return addr + 6 + uint64_t(int64_t(disp));
        } else {
            return addr;
        }
    }
    return addr;
}

}  // namespace hsm
#endif

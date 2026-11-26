// Rules for telling whether a pointer the mod holds still names the engine object it was taken for.
//
// The mod keeps UObject pointers as cache keys (a function's hooks, a hit function's parameter offsets, a hooked
// function's cost) and reads object pointers out of engine memory (a hit event's OtherActor, a fighter's slot
// variables). Both go wrong when the engine frees an object and reuses its address: a key then names a different
// object, and a pointer read from a variable can be anything. The crash reports of 0.8.0-pre52 to pre56 showed the
// second kind as packed integers and ASCII text read as an object's class, and the first as a name read through a
// freed function (public #3343, #3417, #3973, #3979, #3297, #3315, #3558, #4005, #3925).
//
// Pure rules, no engine access: the engine layer (ue/engine.cpp, ue/hook.cpp) supplies the reads.
#pragma once
#include <cstddef>
#include <cstdint>

namespace hsm::util {

// An address the engine could have allocated an object at: 8-byte aligned and inside user space. The first megabyte is
// never an engine allocation (the recorded junk values 0x10000, 0x6C and the packed 32-bit pairs sit there or below it).
// It cannot tell a mapped address from an unmapped one: 0x6C0000 passes, and only a read of the object array slot (or
// the vtable check in the engine layer) tells that it holds no object.
constexpr uint64_t kObjectAddressMin = 0x100000;
constexpr uint64_t kObjectAddressMax = 0x800000000000ull;   // 47 bits, the top of x64 user space
constexpr bool PlausibleObjectAddress(uint64_t a) { return (a & 7) == 0 && a >= kObjectAddressMin && a < kObjectAddressMax; }
inline bool PlausibleObjectAddress(const void* p) { return PlausibleObjectAddress(uint64_t(reinterpret_cast<uintptr_t>(p))); }

// What an object is in the engine apart from its address: its slot in the object array (UObjectBase::InternalIndex) and
// its FName, as index | number << 32. A freed object's address comes back as another object with another slot or name.
struct ObjectIdent {
    int32_t index = -1;
    uint64_t name = 0;
    bool operator==(const ObjectIdent& o) const { return index == o.index && name == o.name; }
    bool operator!=(const ObjectIdent& o) const { return !(*this == o); }
};
constexpr uint64_t NameKey(int32_t nameIndex, int32_t number) { return uint64_t(uint32_t(nameIndex)) | uint64_t(uint32_t(number)) << 32; }

// Whether a stored identity still describes `key`. Two steps, in this order, so a freed object's memory is never read:
// the object array must still hold `key` at the stored slot (slotHolds = what it holds there), and only then is the
// object's own identity read (readNow, a callable returning ObjectIdent) and compared. A key the engine just passed in
// (the function of a ProcessEvent call) is live by definition: compare its identity with the stored one directly.
template <class ReadNow>
bool CacheEntryCurrent(const ObjectIdent& stored, const void* key, const void* slotHolds, ReadNow&& readNow) {
    if (!key || stored.index < 0 || slotHolds != key) return false;
    return readNow() == stored;
}

// A diagnostic line that must not flood: the first `first` times, then every `every`th.
constexpr bool ReportDue(uint64_t count, uint64_t first = 5, uint64_t every = 1000) {
    return count >= 1 && (count <= first || count % every == 0);
}

}  // namespace hsm::util

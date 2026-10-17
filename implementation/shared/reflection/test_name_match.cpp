#include "reflection/hsm_name_match.h"
#include <cassert>

int main() {
    using hsm::ReflectedNameEqual;
    assert(ReflectedNameEqual("Bone", "bone"));
    assert(ReflectedNameEqual("BONE", "Bone"));
    assert(ReflectedNameEqual("Hit Bone", "hit bone"));
    assert(ReflectedNameEqual("Damage Raw Scaled", "Damage Raw Scaled"));
    assert(ReflectedNameEqual("", ""));
    assert(!ReflectedNameEqual("Bone", "Hit Bone"));
    assert(!ReflectedNameEqual("Bone", "Bone_0"));
    assert(!ReflectedNameEqual("Bone", "Bones"));
    assert(!ReflectedNameEqual("Bone", "Bane"));
    assert(!ReflectedNameEqual("\xC0", "\xE0"));
}

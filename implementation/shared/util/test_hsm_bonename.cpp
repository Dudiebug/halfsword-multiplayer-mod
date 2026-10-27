#include "hsm_validate.h"
#include <cassert>
#include <string>

using namespace hsm::util;

int main() {
    // Valid names
    assert(ValidBoneName("Armature"));
    assert(ValidBoneName("bone_001"));
    assert(ValidBoneName("ik_hand_r"));
    assert(ValidBoneName("A"));
    assert(ValidBoneName("_underscore"));
    assert(ValidBoneName("123"));

    // Invalid: empty
    assert(!ValidBoneName(""));

    // Invalid: too long (>64)
    std::string long_name(65, 'a');
    assert(!ValidBoneName(long_name));

    // Invalid: special characters
    assert(!ValidBoneName("bone-name"));
    assert(!ValidBoneName("bone.name"));
    assert(!ValidBoneName("bone name"));
    assert(!ValidBoneName("bone@name"));
    assert(!ValidBoneName("bone#1"));

    // Invalid: non-ASCII
    assert(!ValidBoneName("bön€"));
    assert(!ValidBoneName("骨"));

    // Boundary: 64 chars is OK
    std::string boundary(64, 'a');
    assert(ValidBoneName(boundary));

    return 0;
}

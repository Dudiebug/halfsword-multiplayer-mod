#include "reflection/hsm_class_match.h"
#include <cassert>

struct Class { const char* name; const Class* super; };
int main() {
    const Class actor{"Actor", nullptr};
    const Class weapon{"ModularWeaponBP_C", &actor};
    const Class axe{"ModularWeaponBP_Axe_C", &weapon};
    const Class variant{"AxeVariant_C", &axe};
    const Class unrelated{"ModularWeaponBP_Fake_C", &actor};
    auto matches = [](const Class* cls) {
        return hsm::ClassChainNamed(cls, "ModularWeaponBP_C",
            [](const Class* c) { return std::string_view(c->name); },
            [](const Class* c) { return c->super; });
    };
    assert(matches(&weapon));
    assert(matches(&axe));
    assert(matches(&variant));
    assert(!matches(&unrelated));
    assert(!matches(&actor));
    assert(!matches(nullptr));
}

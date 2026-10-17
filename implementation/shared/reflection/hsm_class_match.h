#pragma once
#include <string_view>

namespace hsm {
// Match the class itself or an ancestor, never a name prefix that could match an unrelated class.
template<class Class, class GetName, class GetSuper>
bool ClassChainNamed(const Class* cls, std::string_view name, GetName getName, GetSuper getSuper) {
    for (; cls; cls = getSuper(cls)) if (getName(cls) == name) return true;
    return false;
}
}  // namespace hsm

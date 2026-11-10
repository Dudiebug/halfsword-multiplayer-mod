// The UE4SS builds the mod accepts: the one it carries is among them, and a build of someone's own isn't.
#include "hsm_dll_rules.h"
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace hsm;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    CHECK(IsKnownUe4ss(kBundledUe4ss));
    CHECK(IsKnownUe4ss("94c2ebd3ca9f4c4212898d727df5482937f586771c67502635ebd4367422052a"));   // the v3.0.1 Beta many players have
    CHECK(!IsKnownUe4ss("") && !IsKnownUe4ss("00") && !IsKnownUe4ss(std::string(kBundledUe4ss).substr(1)));
    CHECK(!IsKnownUe4ss("AAB30033E336DF1FEAE7EE5F175A7DBDBDC0630C9C3D70F3086D212F25009F8B"));   // hashes are compared as written: lower case
    std::printf("ok\n");
    return 0;
}

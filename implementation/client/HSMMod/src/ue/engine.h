// Minimal runtime reflection over the retail Half Sword process (UE 5.4.4, build-gated).
// Objects, functions and properties are resolved by name at runtime, so the mod needs no generated SDK
// and tolerates Blueprint layout changes; only core engine layouts are fixed per supported build.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace hsm::ue {

struct UObject;   // opaque engine types
struct UClass;
struct UFunction;
struct FProperty;
struct FName { int32_t index = 0; int32_t number = 0; };

struct FVector { double x = 0, y = 0, z = 0; };
struct FRotator { double pitch = 0, yaw = 0, roll = 0; };
struct FQuat { double x = 0, y = 0, z = 0, w = 1; };
struct FTransform { FQuat rotation; FVector translation; double pad0 = 0; FVector scale{1, 1, 1}; double pad1 = 0; };
static_assert(sizeof(FTransform) == 0x60, "UE 5.4 FTransform (double, SIMD-aligned)");

// Lifecycle ---------------------------------------------------------------
bool Init();                       // build gate + engine globals; false = unsupported, stay vanilla
bool SelfTest();                   // reflection layout checks; call once Kismet classes are loaded
uint64_t BuildKey();               // PE TimeDateStamp << 32 | SizeOfImage
void* ProcessEventAddress();

// Names and objects --------------------------------------------------------
std::string NameToString(FName n);
std::string ObjectName(const UObject* o);
std::string FullName(const UObject* o);          // "Class /Outer.Outer.Name"
FName ObjectFName(const UObject* o);
UClass* ClassOf(const UObject* o);
UObject* OuterOf(const UObject* o);
bool IsA(const UObject* o, const UClass* c);
bool IsChildOf(const UClass* c, const UClass* base);   // c == base or derives from it
bool IsDefaultObject(const UObject* o);
int32_t ObjectCount();
int32_t ObjectIndex(const UObject* o);          // GUObjectArray slot (UObjectBase::InternalIndex)
UObject* ObjectAt(int32_t index);
UObject* FindObject(const std::string& name, const char* className = nullptr);   // by object name
std::vector<UObject*> FindInstances(const char* className);                      // non-CDO instances
UObject* DefaultObject(const UClass* c);

// Reflection -----------------------------------------------------------------
UFunction* FindFunction(const UClass* c, const std::string& name);              // walks super classes
std::vector<UFunction*> FunctionsWithPrefix(const UClass* c, const std::string& prefix);
FProperty* FindProperty(const void* ustruct, const std::string& name);         // walks super structs
FProperty* FindPropertyPrefix(const void* ustruct, const std::string& prefix); // user-defined struct members carry _N_GUID suffixes
int32_t PropertyOffset(const FProperty* p);
int32_t PropertySize(const FProperty* p);           // ElementSize
int32_t PropertyArrayDim(const FProperty* p);
uint64_t PropertyFlags(const FProperty* p);         // EPropertyFlags
std::string PropertyName(const FProperty* p);
std::string PropertyType(const FProperty* p);
int32_t ParamsSize(const UFunction* f);
uint32_t FunctionFlags(const UFunction* f);
std::vector<FProperty*> Params(const UFunction* f);
void* FunctionNative(const UFunction* f);           // UFunction::Func: the native thunk, or ProcessInternal for script functions
bool IsScriptFunction(const UFunction* f);          // Func is this build's ProcessInternal (a Blueprint body run by the VM)
// This build's ProcessLocalScriptFunction (UObject*, FFrame&, RESULT_DECL), the one executor every Blueprint function body
// runs through, after checking Func's offset, ProcessInternal's call into it and both prologues. nullptr = not verified.
void* ScriptExecutorAddress();

// Calling and field access -----------------------------------------------------
void ProcessEvent(UObject* obj, UFunction* fn, void* params);

// Typed member access by reflected property name. Returns nullptr when the property is missing.
void* FieldPtr(UObject* obj, const std::string& property);
template <class T> T* Field(UObject* obj, const std::string& property) { return static_cast<T*>(FieldPtr(obj, property)); }
bool GetBool(UObject* obj, const std::string& property, bool& out);
bool SetBool(UObject* obj, const std::string& property, bool value);   // handles bitfield bools
// A BoolProperty's value at its value address (container + offset), bitfield bools included. False for other types.
bool GetBoolValue(const FProperty* p, const void* value, bool& out);
bool SetBoolValue(const FProperty* p, void* value, bool v);

// Parameter buffer for ProcessEvent, laid out by the function's reflected properties.
class Call {
public:
    Call(UObject* target, UFunction* fn);
    Call(UObject* target, const std::string& functionName);   // resolves on target's class
    bool valid() const { return target_ && fn_; }
    template <class T> Call& set(const std::string& param, const T& v) { if (void* p = slot(param, sizeof(T))) *static_cast<T*>(p) = v; return *this; }
    Call& setName(const std::string& param, FName v) { return set(param, v); }
    // A float or double parameter, whichever the function declares (a native C++ float stays 4 bytes in UE5; the
    // reflection index does not record which).
    Call& setReal(const std::string& param, double v);
    // Deep-copies a reflected value of the parameter's own type (structs with TMap/TArray) from src.
    Call& setFrom(const std::string& param, const void* src);
    void* paramPtr(const std::string& param);        // raw slot (e.g. to read an out struct in place)
    bool invoke();
    template <class T> T get(const std::string& param) { T v{}; if (void* p = slot(param, sizeof(T))) v = *static_cast<T*>(p); return v; }
    UFunction* function() const { return fn_; }
private:
    void* slot(const std::string& param, size_t size);
    UObject* target_ = nullptr;
    UFunction* fn_ = nullptr;
    std::vector<uint8_t> buf_;
};

// A call resolved once for per-frame use: the UFunction and each listed parameter's offset are looked up at
// construction (FindFunction/FindProperty walk names; Call repeats that on every use), so a call is a few stores and one
// ProcessEvent. Parameters are addressed by their index in the constructor's list; put/take require the exact
// reflected size (so a float/double or FVector3f/FVector mix-up reads nothing instead of garbage). The one buffer is
// reused across calls: an out or return value that owns memory (TArray, FString inside a struct) is released through
// the engine's own ClearValue before the next call writes it. Prepare against the target's own class (overrides
// resolve there); invoke refuses a target that is not of that class. Re-prepare after a level change.
class PreparedCall {
public:
    PreparedCall() = default;
    PreparedCall(const UClass* cls, const std::string& function, std::initializer_list<const char*> params);
    // Move-only: a copy would share engine-owned output values. A moved-from call is invalid. Whatever an owning
    // output still holds when the call is replaced or destroyed is left to leak (the function may be unloaded by then).
    PreparedCall(const PreparedCall&) = delete;
    PreparedCall& operator=(const PreparedCall&) = delete;
    PreparedCall(PreparedCall&& o) noexcept { *this = static_cast<PreparedCall&&>(o); }
    PreparedCall& operator=(PreparedCall&& o) noexcept;
    bool valid() const { return fn_ != nullptr; }
    template <class T> PreparedCall& put(size_t i, const T& v) { if (void* p = slot(i, sizeof(T))) *static_cast<T*>(p) = v; return *this; }
    template <class T> T take(size_t i) const { T v{}; if (const void* p = slot(i, sizeof(T))) v = *static_cast<const T*>(p); return v; }
    bool invoke(UObject* target);
    UFunction* function() const { return fn_; }
private:
    void* slot(size_t i, size_t size);
    const void* slot(size_t i, size_t size) const;
    const UClass* cls_ = nullptr;
    UFunction* fn_ = nullptr;
    std::vector<int32_t> off_, size_;
    std::vector<FProperty*> owning_;   // out/return parameters that need a destructor
    bool ran_ = false;
    std::vector<uint8_t> buf_;
};

FName MakeName(const std::string& s);   // via KismetStringLibrary.Conv_StringToName (game thread only)

// FString/FText as they appear in parameter buffers. FStringArg points at caller-owned storage the engine only reads.
struct FStringArg { const wchar_t* data = nullptr; int32_t num = 0, max = 0; };
struct FStringArgHolder {
    explicit FStringArgHolder(const std::string& s) : w(s.begin(), s.end()) { arg = { w.c_str(), int32_t(w.size() + 1), int32_t(w.size() + 1) }; }
    std::wstring w; FStringArg arg;
};
// TArray<uint8> header as it appears in parameter buffers. Engine-filled arrays are copied out and left to the engine
// allocator (never freed by the mod); caller-filled arrays point at mod-owned bytes the engine only reads.
struct FByteArray { uint8_t* data = nullptr; int32_t num = 0, max = 0; };
struct FText { uint8_t opaque[0x10] = {}; };          // engine-owned (16 bytes in this build, measured live); pass through only
FText MakeText(const std::string& s);                  // KismetTextLibrary.Conv_StringToText (game thread only)
std::string ReadFString(const void* fstring);          // copy an engine FString into UTF-8

// Engine-side helpers --------------------------------------------------------
uintptr_t ModuleBase();
UClass* FindClass(const std::string& name);            // native or Blueprint class by object name
UObject* SpawnObject(UClass* cls, UObject* outer);     // GameplayStatics.SpawnObject (UObjects incl. UMG widgets)
// Deep copy of one reflected value (structs with TMap/TArray included) via FProperty::CopyValuesInternal.
void CopyPropertyValue(const FProperty* prop, void* dest, const void* src);
// FProperty::ClearValue over all ArrayDim elements: memzero for POD, ClearValueInternal otherwise.
void ClearPropertyValue(const FProperty* prop, void* value);

}  // namespace hsm::ue

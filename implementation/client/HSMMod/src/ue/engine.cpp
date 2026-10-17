#include "diagnostics/stall.h"
#include "ue/engine.h"
#include "reflection/hsm_class_match.h"
#include "reflection/hsm_engine_pins.h"
#include "reflection/hsm_startup_sanity.h"
#include "reflection/hsm_name_match.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace hsm::ue {
namespace {

// ---- per-build constants (RVA), verified against the shipping exe (see docs/REVERSE_ENGINEERING.md)
struct ScriptVm {
    uintptr_t processInternal;     // UObject::ProcessInternal, Func of every script UFunction (UFunction::Bind)
    uintptr_t executor;            // ProcessLocalScriptFunction(UObject*, FFrame&, RESULT_DECL)
    uint32_t executorCallSite;     // offset of ProcessInternal's E8 call to the executor
    uintptr_t execAddIntInt;       // UKismetMathLibrary::execAdd_IntInt, checks UFunction::Func's offset
};
struct BuildInfo { uint64_t key; const char* label; uintptr_t gobjects, appendString; uint32_t processEventIndex; ScriptVm vm; };
// key = PE TimeDateStamp << 32 | SizeOfImage. Unknown keys use unique signatures, then the same layout checks.
constexpr BuildInfo kBuilds[] = {
    {0x8EFA122909712000ull, "steam-24185754 (UE 5.4.4 CL-2705)", 0x08BFD240, 0x012C1CB0, 0x4D,
     {0x014A4850, 0x014A4950, 0x6C, 0x036AF990}},
};
// Prologues of the two VM functions in the builds above (MSVC, /GS): checked before the executor is detoured.
constexpr uint8_t kExecutorPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x70};
constexpr uint8_t kProcessInternalPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
// ---- UE 5.4 core layouts for this build
constexpr size_t kObjFlags = 0x08, kObjIndex = 0x0C, kObjClass = 0x10, kObjName = 0x18, kObjOuter = 0x20;
constexpr size_t kFieldNext = 0x28;
constexpr size_t kStructSuper = 0x40, kStructChildren = 0x48, kStructChildProps = 0x50, kStructSize = 0x58;
constexpr size_t kFuncFlags = 0xB0, kFuncNative = 0xD8;
constexpr size_t kClassCDO = 0x110;
constexpr size_t kFFieldClass = 0x08, kFFieldNext = 0x18, kFFieldName = 0x20;
constexpr size_t kPropArrayDim = 0x30, kPropElementSize = 0x34, kPropFlags = 0x38, kPropOffset = 0x44;
constexpr size_t kBoolByteOffset = 0x71, kBoolByteMask = 0x72;
constexpr size_t kPropVtCopyValues = 0xF0;   // FProperty::CopyValuesInternal(Dest, Src, Count)
constexpr size_t kPropVtClearValue = 0x130;   // FProperty::ClearValueInternal(Data), as ProcessLocalScriptFunction's abort path calls it
constexpr uint32_t kRF_ClassDefaultObject = 0x10;
constexpr uint64_t kCPF_Parm = 0x80, kCPF_OutParm = 0x100, kCPF_ZeroConstructor = 0x200, kCPF_ReturnParm = 0x400,
                   kCPF_NoDestructor = 0x1000000000ull;
constexpr int32_t kItemsPerChunk = 0x10000, kItemSize = 0x18;

struct TUObjectArray { uint8_t** objects; uint8_t* pre; int32_t maxElements, numElements, maxChunks, numChunks; };
struct FString { wchar_t* data; int32_t num, max; };

BuildInfo g_scanned{};
const BuildInfo* g_build = nullptr;
uintptr_t g_base = 0;
TUObjectArray* g_objects = nullptr;
using AppendStringFn = void (*)(const FName*, FString*);
AppendStringFn g_appendString = nullptr;
uint64_t g_key = 0;
uintptr_t g_namePool = 0;

std::mutex g_nameMu;
std::unordered_map<uint64_t, std::string> g_nameCache;

template <class T> T Read(const void* base, size_t off) { T v; std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T)); return v; }

uint64_t ComputeBuildKey() {
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    return uint64_t(nt->FileHeader.TimeDateStamp) << 32 | nt->OptionalHeader.SizeOfImage;
}

bool IsName(const UObject* o, const char* n) { return o && ObjectName(o) == n; }
std::string ClassName(const UObject* o) { UClass* c = ClassOf(o); return c ? ObjectName(reinterpret_cast<UObject*>(c)) : std::string(); }
}  // namespace

uint64_t BuildKey() { return g_key; }

namespace {
bool Readable(const void* ptr, size_t size) {
    uintptr_t at=reinterpret_cast<uintptr_t>(ptr);
    if (!at || size>UINTPTR_MAX-at) return false;
    uintptr_t end=at+size;
    while (at<end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(at),&info,sizeof info) || info.State!=MEM_COMMIT ||
            (info.Protect&(PAGE_NOACCESS|PAGE_GUARD)) || !(info.Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))) return false;
        uintptr_t next=reinterpret_cast<uintptr_t>(info.BaseAddress)+info.RegionSize;
        if (next<=at) return false;
        at=next;
    }
    return true;
}
template<class T> bool Checked(const void* ptr, size_t off, T& value) {
    uintptr_t at=reinterpret_cast<uintptr_t>(ptr);
    if (!at || off>UINTPTR_MAX-at) return false;
    SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at+off),&value,sizeof value,&got) && got==sizeof value;
}
bool PoolName(FName n, std::string& out) {
    return uecheck::PoolName(g_namePool,n.index,n.number,[](uintptr_t at,void* dst,size_t size) {
        SIZE_T got=0;return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),dst,size,&got) && got==size;
    },out);
}
bool NameEntrySane(FName n) { std::string name; return PoolName(n,name); }
bool NameChecked(const void* object, size_t offset, std::string& out) {
    FName n{};
    return Checked(object,offset,n) && PoolName(n,out); // no AppendString call on the startup worker
}
bool ArraySane(const TUObjectArray& a) {
    return uecheck::ArrayShape(a.maxElements,a.numElements,a.maxChunks,a.numChunks) &&
        Readable(a.objects,size_t(a.numChunks)*sizeof(void*));
}
UObject* CheckedObject(int32_t index) {
    if (index<0 || index>=g_objects->numElements) return nullptr;
    uint8_t* chunk=nullptr; UObject* obj=nullptr;
    if (!Checked(g_objects->objects,size_t(index/kItemsPerChunk)*sizeof(void*),chunk) ||
        !Checked(chunk,size_t(index%kItemsPerChunk)*kItemSize,obj)) return nullptr;
    return obj;
}
bool LayoutSane(const pins::Image& image, std::string& error) {
    auto fail=[&](const std::string& check) { error=check; return false; };
    // Before normal unguarded reflection: bounded reads of object/class/outer/struct/property/function layouts.
    int sampled=0;
    for (int32_t i=0;i<g_objects->numElements && sampled<64;++i) {
        UObject* o=CheckedObject(i); int32_t index=-1; UClass* cls=nullptr;
        if (!o) continue; // unused/collected object slots are legal
        ++sampled;
        if (!Checked(o,kObjIndex,index) || index!=i || !Checked(o,kObjClass,cls) || !Readable(cls,kClassCDO+8))
            return fail("object["+std::to_string(i)+"] index/class (index="+std::to_string(index)+")");
        const void* outer=o; size_t depth=0;
        while (outer && depth++<32) {
            std::string name; if (!NameChecked(outer,kObjName,name) || !Checked(outer,kObjOuter,outer)) return fail("object["+std::to_string(i)+"] name/outer read");
        }
        if (outer) return fail("object outer-chain depth");
    }
    if (sampled<64) return fail("fewer than 64 live object headers");
    std::string first; if (!NameChecked(CheckedObject(0),kObjName,first) || first!="/Script/CoreUObject") return fail("object[0] name: "+first);
    void** vt=nullptr; void* pe=nullptr;
    if (!Checked(CheckedObject(0),0,vt) || !Checked(vt,g_build->processEventIndex*sizeof(void*),pe) ||
        reinterpret_cast<uintptr_t>(pe)<g_base || reinterpret_cast<uintptr_t>(pe)-g_base>UINT32_MAX ||
        !pins::InSection(image,uint32_t(reinterpret_cast<uintptr_t>(pe)-g_base),1,pins::kExecutable)) return fail("ProcessEvent vtable slot 0x4D");
    UObject* vector=nullptr; UObject* math=nullptr;
    for (int32_t i=0;i<g_objects->numElements && (!vector || !math);++i) {
        UObject* o=CheckedObject(i); if (!o) continue;
        std::string name; if (!NameChecked(o,kObjName,name)) return fail("class lookup object["+std::to_string(i)+"] name");
        if (name!="Vector" && name!="KismetMathLibrary") continue;
        void* cls=nullptr; std::string type;
        if (!Checked(o,kObjClass,cls) || !NameChecked(cls,kObjName,type)) return fail(name+" class name");
        if (name=="Vector" && type=="ScriptStruct") vector=o;
        if (name=="KismetMathLibrary" && type=="Class") math=o;
    }
    int32_t structSize=0; void* prop=nullptr;
    if (!vector) return fail("Core Vector not registered yet");
    if (!Checked(vector,kStructSize,structSize) || structSize!=24 || !Checked(vector,kStructChildProps,prop)) return fail("Vector size/fields (size="+std::to_string(structSize)+")");
    for (int i=0;i<3;++i) {
        int32_t offset=-1,size=0,dim=0; void* type=nullptr; std::string name,typeName;
        if (!prop || !NameChecked(prop,kFFieldName,name) || name!=std::string(1,char('X'+i)) ||
            !Checked(prop,kPropOffset,offset) || offset!=i*8 || !Checked(prop,kPropElementSize,size) || size!=8 ||
            !Checked(prop,kPropArrayDim,dim) || dim!=1 || !Checked(prop,kFFieldClass,type) ||
            !NameChecked(type,0,typeName) || typeName!="DoubleProperty" || !Checked(prop,kFFieldNext,prop)) return fail("Vector field "+std::to_string(i)+" name="+name+" type="+typeName+" offset="+std::to_string(offset)+" size="+std::to_string(size)+" dim="+std::to_string(dim));
    }
    if (prop) return fail("Vector extra field");
    UObject* cdo=nullptr; UClass* cdoClass=nullptr; void* super=nullptr;
    if (!math) return fail("KismetMathLibrary not registered yet");
    if (!Checked(math,kStructSuper,super) || !Readable(super,kStructSize+4) ||
        !Checked(math,kClassCDO,cdo) || !Checked(cdo,kObjClass,cdoClass) || reinterpret_cast<void*>(cdoClass)!=math) return fail("KismetMathLibrary superclass/CDO readiness");
    UObject* fn=nullptr; if (!Checked(math,kStructChildren,fn)) return fail("KismetMathLibrary child-function pointer");
    for (size_t count=0;fn && count<4096;++count) {
        std::string name; if (!NameChecked(fn,kObjName,name)) return fail("KismetMathLibrary child-function name");
        if (name=="Add_IntInt") {
            void* native=nullptr; uint32_t flags=0; int32_t size=0;
            if (!Checked(fn,kFuncNative,native) || native!=reinterpret_cast<void*>(g_base+g_build->vm.execAddIntInt) ||
                !Checked(fn,kFuncFlags,flags) || !(flags&0x400) || !Checked(fn,kStructSize,size) || size!=12) return fail("Add_IntInt native/flags/parameter size (size="+std::to_string(size)+" flags="+std::to_string(flags)+")");
            return true;
        }
        if (!Checked(fn,kFieldNext,fn)) return fail("KismetMathLibrary child-function next");
    }
    return fail("Add_IntInt not registered or child chain too long");
}
void LogPins(const pins::Pins& p,const BuildInfo* known) {
    const uint32_t resolved[]={p.objects,p.append,p.internal,p.executor,p.add};
    const uintptr_t expected[]={known?known->gobjects:0,known?known->appendString:0,known?known->vm.processInternal:0,
        known?known->vm.executor:0,known?known->vm.execAddIntInt:0};
    for (size_t i=0;i<5;++i) HSM_LOG("engine: pin %s resolved=%p RVA=%08x table=%p RVA=%08x known=%d",
        pins::kSignatures[i].name,resolved[i]?reinterpret_cast<void*>(g_base+resolved[i]):nullptr,resolved[i],
        known?reinterpret_cast<void*>(g_base+expected[i]):nullptr,unsigned(expected[i]),known?1:0);
}
void ResetPins() {
    g_build=nullptr;g_objects=nullptr;g_appendString=nullptr;g_namePool=0;
    std::lock_guard<std::mutex> lock(g_nameMu);g_nameCache.clear();
}
} // namespace

bool Init(bool forceScan) {
    ResetPins();
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_key = ComputeBuildKey();
    const BuildInfo* known=nullptr;
    for (const BuildInfo& b:kBuilds) if (b.key==g_key) { known=&b;break; }
    pins::Image image;
    // PE headers are the OS-loaded main image; executable sections must be readable before scanning.
    if (!pins::ParseImage({reinterpret_cast<const uint8_t*>(g_base),uint32_t(g_key)},image)) {
        LogPins({},known);HSM_LOG("engine: pin failed: invalid PE image"); return false;
    }
    for (const auto& section:image.sections) if ((section.flags&pins::kExecutable) &&
        !Readable(reinterpret_cast<void*>(g_base+section.rva),section.size)) {
        LogPins({},known);HSM_LOG("engine: pin failed: unreadable executable section RVA=%08x size=%u",section.rva,section.size); return false;
    }
    if (forceScan) HSM_LOG("engine: build table skipped by test request (force_scan=1)");
    else g_build=known;
    bool scanned=!g_build;
    if (scanned) {
        auto result=pins::Scan(image);
        if (!result.error.empty()) { LogPins(result.pins,known); HSM_LOG("engine: pin failed for %016llx: %s",(unsigned long long)g_key,result.error.c_str());return false; }
        const auto& p=result.pins;
        g_scanned={g_key,"signature scan (UE 5.4 layouts)",p.objects,p.append,0x4D,{p.internal,p.executor,p.callSite,p.add}};
        g_build=&g_scanned;
        for (size_t i=0;i<result.counts.size();++i) HSM_LOG("engine: scan %s matches=%zu hit RVA=%08x",pins::kSignatures[i].name,result.counts[i],result.hits[i]);
    }
    pins::Pins p{uint32_t(g_build->gobjects),uint32_t(g_build->appendString),uint32_t(g_build->vm.processInternal),
        uint32_t(g_build->vm.executor),uint32_t(g_build->vm.execAddIntInt),g_build->vm.executorCallSite};
    LogPins(p,known);
    std::string error;
    if (!pins::Validate(image,p,error)) { HSM_LOG("engine: pin failed: %s",error.c_str());ResetPins();return false; }
    g_objects = reinterpret_cast<TUObjectArray*>(g_base + g_build->gobjects);
    g_appendString = reinterpret_cast<AppendStringFn>(g_base + g_build->appendString);
    // AppendString's two RIP-relative LEAs must agree on its allocator; no extra absolute pin.
    int32_t poolDisp1=0,poolDisp2=0;
    pins::Read(image.bytes,size_t(p.append)+35,poolDisp1); pins::Read(image.bytes,size_t(p.append)+44,poolDisp2);
    int64_t pool1=int64_t(p.append)+39+poolDisp1,pool2=int64_t(p.append)+48+poolDisp2;
    if (pool1!=pool2 || pool1<0 || pool1>UINT32_MAX || !pins::InSection(image,uint32_t(pool1),0x18,pins::kWritable,pins::kExecutable)) {
        HSM_LOG("engine: pin failed: AppendString name allocator mismatch");ResetPins();return false;
    }
    g_namePool=g_base+uintptr_t(pool1);
    // A population threshold is not a readiness barrier: native class registration and CDO creation can lag it.
    TUObjectArray array{}; bool ready=false;std::string previous;
    const uint64_t deadline=GetTickCount64()+60000;
    do {
        error.clear();
        if (!Checked(g_objects,0,array)) error="GObjects header unreadable";
        else if (!ArraySane(array)) error="GObjects shape/chunks num="+std::to_string(array.numElements)+
            " max="+std::to_string(array.maxElements)+" chunks="+std::to_string(array.numChunks)+" maxChunks="+std::to_string(array.maxChunks);
        else if (LayoutSane(image,error)) { ready=true;break; }
        if (error!=previous) { HSM_LOG("engine: waiting for startup sanity: %s",error.c_str());previous=error; }
        Sleep(100);
    } while (GetTickCount64()<deadline);
    if (!ready) {
        HSM_LOG("engine: pin failed: startup sanity timeout: %s; no hooks installed",error.c_str());
        ResetPins();return false;
    }
    HSM_LOG("engine: build pinned by %s key=%016llx GObjects=%p AppendString=%p ProcessInternal=%p ScriptExecutor=%p execAdd_IntInt=%p",
        scanned?"scan":"table",(unsigned long long)g_key,reinterpret_cast<void*>(g_base+p.objects),reinterpret_cast<void*>(g_base+p.append),
        reinterpret_cast<void*>(g_base+p.internal),reinterpret_cast<void*>(g_base+p.executor),reinterpret_cast<void*>(g_base+p.add));
    HSM_LOG("engine: UE 5.4 layout sanity passed; %d objects",g_objects->numElements);
    return true;
}

void* ProcessEventAddress() {
    UObject* o = ObjectAt(0);
    if (!o) return nullptr;
    void** vtable = Read<void**>(o, 0);
    return vtable[g_build->processEventIndex];
}

// ---------------------------------------------------------------- names / objects
std::string NameToString(FName n) {
    uint64_t key = uint64_t(uint32_t(n.index)) << 32 | uint32_t(n.number);
    {
        std::lock_guard<std::mutex> lock(g_nameMu);
        auto it = g_nameCache.find(key);
        if (it != g_nameCache.end()) return it->second;
    }
    // AppendString writes into our FString; the buffer is large enough that the engine never reallocates it.
    if (!NameEntrySane(n)) return {};
    wchar_t buf[1024]; FString s{buf, 0, 1024};
    g_appendString(&n, &s);
    std::string out;
    if (s.data != buf || s.num < 0 || s.num > 1024 || s.max != 1024) return out;
    for (int i = 0; i < s.num && buf[i]; ++i) out.push_back(buf[i] < 128 ? char(buf[i]) : '?');
    std::lock_guard<std::mutex> lock(g_nameMu);
    g_nameCache.emplace(key, out);
    return out;
}

FName ObjectFName(const UObject* o) { return o ? Read<FName>(o, kObjName) : FName{}; }
std::string ObjectName(const UObject* o) { return o ? NameToString(Read<FName>(o, kObjName)) : std::string(); }
UClass* ClassOf(const UObject* o) { return o ? Read<UClass*>(o, kObjClass) : nullptr; }
UObject* OuterOf(const UObject* o) { return o ? Read<UObject*>(o, kObjOuter) : nullptr; }
bool IsDefaultObject(const UObject* o) { return o && (Read<uint32_t>(o, kObjFlags) & kRF_ClassDefaultObject); }

std::string FullName(const UObject* o) {
    if (!o) return "None";
    std::string path = ObjectName(o);
    for (UObject* outer = OuterOf(o); outer; outer = OuterOf(outer)) path = ObjectName(outer) + "." + path;
    return ClassName(o) + " " + path;
}

bool IsA(const UObject* o, const UClass* c) { return IsChildOf(ClassOf(o), c); }

bool IsAByName(const UObject* o, const char* className) {
    return className && ClassChainNamed(ClassOf(o), className,
        [](const UClass* c) { return ObjectName(reinterpret_cast<const UObject*>(c)); },
        [](const UClass* c) { return Read<UClass*>(c, kStructSuper); });
}

bool IsChildOf(const UClass* c, const UClass* base) {
    for (const void* s = c; s; s = Read<void*>(s, kStructSuper)) if (s == base) return true;
    return false;
}

int32_t ObjectIndex(const UObject* o) { return o ? Read<int32_t>(o, kObjIndex) : -1; }
int32_t ObjectCount() { return g_objects ? g_objects->numElements : 0; }

UObject* ObjectAt(int32_t index) {
    if (!g_objects || index < 0 || index >= g_objects->numElements) return nullptr;
    uint8_t* chunk = g_objects->objects[index / kItemsPerChunk];
    if (!chunk) return nullptr;
    return *reinterpret_cast<UObject**>(chunk + size_t(index % kItemsPerChunk) * kItemSize);
}

UObject* FindObject(const std::string& name, const char* className) {
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (!o || ObjectName(o) != name) continue;
        if (className && ClassName(o) != className) continue;
        return o;
    }
    return nullptr;
}

std::vector<UObject*> FindInstances(const char* className, bool includeDerived) {
    std::vector<UObject*> out;
    for (int32_t i = 0, n = ObjectCount(); i < n; ++i) {
        UObject* o = ObjectAt(i);
        if (o && !IsDefaultObject(o) && (includeDerived ? IsAByName(o, className) : ClassName(o) == className)) out.push_back(o);
    }
    return out;
}

UObject* DefaultObject(const UClass* c) { return c ? Read<UObject*>(c, kClassCDO) : nullptr; }

// ---------------------------------------------------------------- reflection
UFunction* FindFunction(const UClass* c, const std::string& name) {
    for (const void* s = c; s; s = Read<void*>(s, kStructSuper))
        for (UObject* f = Read<UObject*>(s, kStructChildren); f; f = Read<UObject*>(f, kFieldNext))
            if (ObjectName(f) == name) return reinterpret_cast<UFunction*>(f);
    return nullptr;
}

std::vector<UFunction*> FunctionsWithPrefix(const UClass* c, const std::string& prefix) {
    std::vector<UFunction*> out;
    for (UObject* f = Read<UObject*>(c, kStructChildren); f; f = Read<UObject*>(f, kFieldNext))
        if (ObjectName(f).rfind(prefix, 0) == 0) out.push_back(reinterpret_cast<UFunction*>(f));
    return out;
}

FProperty* FindProperty(const void* ustruct, const std::string& name) {
    for (const void* s = ustruct; s; s = Read<void*>(s, kStructSuper))
        for (const void* p = Read<void*>(s, kStructChildProps); p; p = Read<void*>(p, kFFieldNext))
            if (ReflectedNameEqual(NameToString(Read<FName>(p, kFFieldName)), name)) return static_cast<FProperty*>(const_cast<void*>(p));
    return nullptr;
}

FProperty* FindPropertyPrefix(const void* ustruct, const std::string& prefix) {
    for (const void* s = ustruct; s; s = Read<void*>(s, kStructSuper))
        for (const void* p = Read<void*>(s, kStructChildProps); p; p = Read<void*>(p, kFFieldNext))
            if (NameToString(Read<FName>(p, kFFieldName)).rfind(prefix, 0) == 0) return static_cast<FProperty*>(const_cast<void*>(p));
    return nullptr;
}

int32_t PropertyOffset(const FProperty* p) { return Read<int32_t>(p, kPropOffset); }
int32_t PropertySize(const FProperty* p) { return Read<int32_t>(p, kPropElementSize); }
int32_t PropertyArrayDim(const FProperty* p) { return Read<int32_t>(p, kPropArrayDim); }
uint64_t PropertyFlags(const FProperty* p) { return Read<uint64_t>(p, kPropFlags); }
std::string PropertyName(const FProperty* p) { return NameToString(Read<FName>(p, kFFieldName)); }
std::string PropertyType(const FProperty* p) { const void* fc = Read<void*>(p, kFFieldClass); return fc ? NameToString(Read<FName>(fc, 0)) : std::string(); }
int32_t ParamsSize(const UFunction* f) { return Read<int32_t>(f, kStructSize); }
uint32_t FunctionFlags(const UFunction* f) { return Read<uint32_t>(f, kFuncFlags); }

void* FunctionNative(const UFunction* f) { return f ? Read<void*>(f, kFuncNative) : nullptr; }
bool IsScriptFunction(const UFunction* f) { return g_build && FunctionNative(f) == reinterpret_cast<void*>(g_base + g_build->vm.processInternal); }

void* ScriptExecutorAddress() {
    if (!g_build) return nullptr;
    const ScriptVm& vm = g_build->vm;
    auto* pi = reinterpret_cast<const uint8_t*>(g_base + vm.processInternal);
    auto* exec = reinterpret_cast<const uint8_t*>(g_base + vm.executor);
    UObject* math = FindObject("KismetMathLibrary", "Class");
    UFunction* add = math ? FindFunction(reinterpret_cast<UClass*>(math), "Add_IntInt") : nullptr;
    bool funcOk = add && FunctionNative(add) == reinterpret_cast<void*>(g_base + vm.execAddIntInt);
    bool prologues = std::memcmp(pi, kProcessInternalPrologue, sizeof kProcessInternalPrologue) == 0 &&
                     std::memcmp(exec, kExecutorPrologue, sizeof kExecutorPrologue) == 0;
    const uint8_t* call = pi + vm.executorCallSite;
    bool callOk = call[0] == 0xE8 && call + 5 + Read<int32_t>(call, 1) == exec;
    HSM_LOG("engine: script VM Func@0x%zx %s, prologues %s, ProcessInternal->executor call %s", kFuncNative,
            funcOk ? "ok" : "MISMATCH", prologues ? "ok" : "MISMATCH", callOk ? "ok" : "MISMATCH");
    return funcOk && prologues && callOk ? const_cast<uint8_t*>(exec) : nullptr;
}

std::vector<FProperty*> Params(const UFunction* f) {
    std::vector<FProperty*> out;
    for (const void* p = Read<void*>(f, kStructChildProps); p; p = Read<void*>(p, kFFieldNext))
        if (Read<uint64_t>(p, kPropFlags) & kCPF_Parm) out.push_back(static_cast<FProperty*>(const_cast<void*>(p)));
    return out;
}

// ---------------------------------------------------------------- calls / fields
void ProcessEvent(UObject* obj, UFunction* fn, void* params) {
    using Fn = void (*)(UObject*, UFunction*, void*);
    void** vtable = Read<void**>(obj, 0);
    reinterpret_cast<Fn>(vtable[g_build->processEventIndex])(obj, fn, params);
}

void* FieldPtr(UObject* obj, const std::string& property) {
    if (!obj) return nullptr;
    FProperty* p = FindProperty(ClassOf(obj), property);
    return p ? reinterpret_cast<uint8_t*>(obj) + PropertyOffset(p) : nullptr;
}

bool GetBoolValue(const FProperty* p, const void* value, bool& out) {
    if (!p || !value || PropertyType(p) != "BoolProperty") return false;
    const uint8_t* byte = static_cast<const uint8_t*>(value) + Read<uint8_t>(p, kBoolByteOffset);
    out = (*byte & Read<uint8_t>(p, kBoolByteMask)) != 0;
    return true;
}

bool SetBoolValue(const FProperty* p, void* value, bool v) {
    if (!p || !value || PropertyType(p) != "BoolProperty") return false;
    uint8_t mask = Read<uint8_t>(p, kBoolByteMask);
    uint8_t* byte = static_cast<uint8_t*>(value) + Read<uint8_t>(p, kBoolByteOffset);
    *byte = v ? uint8_t(*byte | mask) : uint8_t(*byte & ~mask);
    return true;
}

bool GetBool(UObject* obj, const std::string& property, bool& out) {
    FProperty* p = obj ? FindProperty(ClassOf(obj), property) : nullptr;
    return p && GetBoolValue(p, reinterpret_cast<uint8_t*>(obj) + PropertyOffset(p), out);
}

bool SetBool(UObject* obj, const std::string& property, bool value) {
    FProperty* p = obj ? FindProperty(ClassOf(obj), property) : nullptr;
    return p && SetBoolValue(p, reinterpret_cast<uint8_t*>(obj) + PropertyOffset(p), value);
}

Call::Call(UObject* target, UFunction* fn) : target_(target), fn_(fn) {
    if (fn_) buf_.assign(size_t(ParamsSize(fn_)) + 16, 0);
}
Call::Call(UObject* target, const std::string& name) : Call(target, target ? FindFunction(ClassOf(target), name) : nullptr) {}

void* Call::slot(const std::string& param, size_t size) {
    if (!fn_) return nullptr;
    FProperty* p = FindProperty(fn_, param);
    if (!p) { HSM_LOG("call: %s has no param %s", ObjectName(reinterpret_cast<UObject*>(fn_)).c_str(), param.c_str()); return nullptr; }
    if (size_t(PropertyOffset(p)) + size > buf_.size() || size_t(PropertySize(p)) < size) {
        HSM_LOG("call: param %s size mismatch (%d vs %zu)", param.c_str(), PropertySize(p), size); return nullptr;
    }
    return buf_.data() + PropertyOffset(p);
}

Call& Call::setReal(const std::string& param, double v) {
    FProperty* p = fn_ ? FindProperty(fn_, param) : nullptr;
    if (p && PropertySize(p) == int32_t(sizeof(double))) return set<double>(param, v);
    return set<float>(param, float(v));             // a missing or odd-sized param is logged there
}

Call& Call::setFrom(const std::string& param, const void* src) {
    FProperty* p = fn_ ? FindProperty(fn_, param) : nullptr;
    if (!p || !src) { HSM_LOG("call: setFrom %s failed", param.c_str()); return *this; }
    CopyPropertyValue(p, buf_.data() + PropertyOffset(p), src);
    return *this;
}

void* Call::paramPtr(const std::string& param) {
    FProperty* p = fn_ ? FindProperty(fn_, param) : nullptr;
    return p ? buf_.data() + PropertyOffset(p) : nullptr;
}

bool Call::invoke() {
    diagnostics::Scope trace("Call::invoke", reinterpret_cast<uintptr_t>(fn_));
    if (!valid()) return false;
    ProcessEvent(target_, fn_, buf_.data());
    return true;
}

PreparedCall::PreparedCall(const UClass* cls, const std::string& name, std::initializer_list<const char*> params) {
    UFunction* fn = cls ? FindFunction(cls, name) : nullptr;
    if (!fn) { HSM_LOG("prepare: %s has no function %s", cls ? ObjectName(reinterpret_cast<const UObject*>(cls)).c_str() : "(null class)", name.c_str()); return; }
    for (FProperty* p : Params(fn))
        if ((PropertyFlags(p) & (kCPF_OutParm | kCPF_ReturnParm)) && !(PropertyFlags(p) & kCPF_NoDestructor)) owning_.push_back(p);
    for (const char* param : params) {
        FProperty* p = FindProperty(fn, param);
        if (!p || !(PropertyFlags(p) & kCPF_Parm)) { HSM_LOG("prepare: %s has no param %s", name.c_str(), param); return; }
        off_.push_back(PropertyOffset(p));
        size_.push_back(PropertySize(p) * PropertyArrayDim(p));
    }
    cls_ = cls;
    fn_ = fn;
    buf_.assign(size_t(ParamsSize(fn)) + 16, 0);
}

PreparedCall& PreparedCall::operator=(PreparedCall&& o) noexcept {
    if (this == &o) return *this;
    cls_ = o.cls_; fn_ = o.fn_; ran_ = o.ran_;
    off_.swap(o.off_); size_.swap(o.size_); owning_.swap(o.owning_); buf_.swap(o.buf_);
    o.cls_ = nullptr; o.fn_ = nullptr; o.ran_ = false;
    o.off_.clear(); o.size_.clear(); o.owning_.clear(); o.buf_.clear();
    return *this;
}

void* PreparedCall::slot(size_t i, size_t size) {
    return const_cast<void*>(static_cast<const PreparedCall*>(this)->slot(i, size));
}

const void* PreparedCall::slot(size_t i, size_t size) const {
    if (!fn_ || i >= off_.size()) return nullptr;
    if (size_t(size_[i]) != size || size_t(off_[i]) + size > buf_.size()) {
        static int logged = 0;
        if (logged++ < 8) HSM_LOG("prepare: %s param #%zu is %d bytes, accessed as %zu", ObjectName(reinterpret_cast<UObject*>(fn_)).c_str(), i, size_[i], size);
        return nullptr;
    }
    return buf_.data() + off_[i];
}

bool PreparedCall::invoke(UObject* target) {
    diagnostics::Scope trace("PreparedCall validate/clear outputs", reinterpret_cast<uintptr_t>(fn_));
    if (!fn_ || !target || !IsA(target, cls_)) return false;
    if (ran_) for (FProperty* p : owning_) ClearPropertyValue(p, buf_.data() + PropertyOffset(p));
    trace.Stage("PreparedCall ProcessEvent");
    ProcessEvent(target, fn_, buf_.data());
    ran_ = true;
    return true;
}

FName MakeName(const std::string& s) {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* cls = FindObject("KismetStringLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(cls));
    std::wstring w(s.begin(), s.end());
    FString str{w.data(), int32_t(w.size() + 1), int32_t(w.size() + 1)};
    Call c(lib, "Conv_StringToName");
    c.set("InString", str);
    c.invoke();
    return c.get<FName>("ReturnValue");
}

uintptr_t ModuleBase() { return g_base; }

UClass* FindClass(const std::string& name) {
    for (const char* kind : {"Class", "BlueprintGeneratedClass", "WidgetBlueprintGeneratedClass", "AnimBlueprintGeneratedClass"})
        if (UObject* o = FindObject(name, kind)) return reinterpret_cast<UClass*>(o);
    return nullptr;
}

UObject* SpawnObject(UClass* cls, UObject* outer) {
    static UObject* statics = nullptr;
    if (!statics) if (UObject* c = FindObject("GameplayStatics", "Class")) statics = DefaultObject(reinterpret_cast<UClass*>(c));
    if (!cls || !outer || !statics) return nullptr;
    Call call(statics, "SpawnObject");
    call.set<UClass*>("ObjectClass", cls).set<UObject*>("Outer", outer);
    if (!call.invoke()) return nullptr;
    return call.get<UObject*>("ReturnValue");
}

void CopyPropertyValue(const FProperty* prop, void* dest, const void* src) {
    using CopyFn = void (*)(const FProperty*, void*, const void*, int32_t);
    void** vtable = Read<void**>(prop, 0);
    reinterpret_cast<CopyFn>(vtable[kPropVtCopyValues / sizeof(void*)])(prop, dest, src, 1);
}

void ClearPropertyValue(const FProperty* prop, void* value) {
    using ClearFn = void (*)(const FProperty*, void*);
    bool pod = (PropertyFlags(prop) & (kCPF_ZeroConstructor | kCPF_NoDestructor)) == (kCPF_ZeroConstructor | kCPF_NoDestructor);
    void** vtable = Read<void**>(prop, 0);
    auto* v = static_cast<uint8_t*>(value);
    for (int32_t i = 0, n = PropertyArrayDim(prop), size = PropertySize(prop); i < n; ++i, v += size) {
        if (pod) std::memset(v, 0, size_t(size));
        else reinterpret_cast<ClearFn>(vtable[kPropVtClearValue / sizeof(void*)])(prop, v);
    }
}

FText MakeText(const std::string& s) {
    static UObject* lib = nullptr;
    if (!lib) if (UObject* cls = FindObject("KismetTextLibrary", "Class")) lib = DefaultObject(reinterpret_cast<UClass*>(cls));
    FStringArgHolder str(s);
    Call c(lib, "Conv_StringToText");
    c.set("InString", str.arg);
    c.invoke();
    return c.get<FText>("ReturnValue");
}

std::string ReadFString(const void* fstring) {
    const FString* s = static_cast<const FString*>(fstring);
    std::string out;
    if (!s || !s->data) return out;
    for (int32_t i = 0; i < s->num && s->data[i]; ++i) out.push_back(s->data[i] < 128 ? char(s->data[i]) : '?');
    return out;
}

bool SelfTest() {
    UObject* math = FindObject("KismetMathLibrary", "Class");
    UFunction* add = math ? FindFunction(reinterpret_cast<UClass*>(math), "Add_IntInt") : nullptr;
    if (!add) { HSM_LOG("selftest: KismetMathLibrary.Add_IntInt not found"); return false; }
    FProperty* a = FindProperty(add, "A"); FProperty* b = FindProperty(add, "B"); FProperty* r = FindProperty(add, "ReturnValue");
    bool layout = a && b && r && PropertyOffset(a) == 0 && PropertyOffset(b) == 4 && PropertyOffset(r) == 8 && PropertySize(a) == 4 && ParamsSize(add) == 12;
    HSM_LOG("selftest: Add_IntInt params %s (size %d, flags %08x)", layout ? "ok" : "MISMATCH", ParamsSize(add), FunctionFlags(add));
    if (!layout) return false;
    bool indexOk = ObjectAt(ObjectIndex(math)) == math;
    HSM_LOG("selftest: object index round trip %s", indexOk ? "ok" : "MISMATCH");
    if (!indexOk) return false;
    Call c(DefaultObject(reinterpret_cast<UClass*>(math)), add);
    c.set<int32_t>("A", 2).set<int32_t>("B", 3).invoke();
    int32_t sum = c.get<int32_t>("ReturnValue");
    HSM_LOG("selftest: ProcessEvent Add_IntInt(2,3) = %d", sum);
    return sum == 5 && IsName(math, "KismetMathLibrary");
}

}  // namespace hsm::ue

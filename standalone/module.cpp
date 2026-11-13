#include "protocol.hpp"
#include "core.hpp"
#include "reference.hpp"
#include "MinHook.h"
#include <array>
#include <atomic>
#include <utility>

namespace {
using Byte = uint8_t;
template<class T> T& At(void* p, size_t offset) { return *reinterpret_cast<T*>(static_cast<Byte*>(p) + offset); }
template<class T> T VFunction(void* p, size_t index) { return reinterpret_cast<T>((*reinterpret_cast<void***>(p))[index]); }
using Tick = bool(__fastcall*)(void*);
using Update = void(__fastcall*)(void*);
using GetType = int(__fastcall*)(void*);
using GetModel = uint8_t(__fastcall*)(void*);
using Transforms = void(__fastcall*)(void*, bool);
using GPose = bool(*)();
HMODULE self;
volatile LONG started;
HANDLE mapping;
HANDLE diskFile, diskMapping;
Byte* disk;
size_t diskSize;
qb::Shared* shared;
Byte *image, *code, *manager, *conditions, *gameMain;
size_t imageSize, codeSize;
void **frameworkGlobal, **atkGlobal;
GPose isGPose;
Tick originalTick;
void* tickTarget;
DWORD mainThread;
bool active, applying;
LONG lastReset;
uint32_t territory;
double seconds;
int frameAmplitude = 400, framePeriod = 450, frameScopes = 15;
bool frameRandom = false;
qb::Scale frameRelative;
qb::Quaternion frameRotation;
qb::RotationClock rotationClock;
bool frameRotate;
LARGE_INTEGER frequency, epoch;
std::atomic<int> callbacks;
struct HookEntry { void* target = nullptr; Update original = nullptr; };
std::array<HookEntry, 16> hooks;
size_t hookCount;
using Identity = qb::Identity;
struct Entry {
    Identity id{}; qb::OwnedScale scale{}; uint64_t phaseKey = 0; double phaseOffset = 0;
    void *updateFunction = nullptr, *transformFunction = nullptr, *drawTypeFunction = nullptr, *modelTypeFunction = nullptr;
    bool tracked = false, needsNotification = false; uint64_t lastFrame = 0; qb::OwnedRotation rotation;
};
std::array<Entry, qb::ObjectCount> entries;
std::array<unsigned, qb::ObjectCount> updateDepth{};
std::atomic<uint64_t> frame;
size_t cursor;
bool KeyDown[3]{};
#ifdef QB_PERF_TEST
uint64_t memoryQueries;
#endif

bool Readable(void* p, size_t size) {
    if (!p || reinterpret_cast<uintptr_t>(p) + size < reinterpret_cast<uintptr_t>(p)) return false;
    MEMORY_BASIC_INFORMATION info{};
#ifdef QB_PERF_TEST
    ++memoryQueries;
#endif
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return reinterpret_cast<Byte*>(p) + size <= static_cast<Byte*>(info.BaseAddress) + info.RegionSize;
}
bool InImage(void* p, size_t size = 1) { return static_cast<Byte*>(p) >= image && static_cast<Byte*>(p) + size <= image + imageSize; }
bool Executable(void* p) { return static_cast<Byte*>(p) >= code && static_cast<Byte*>(p) < code + codeSize; }
Byte* DiskRva(DWORD rva, size_t count) {
    if (!disk || diskSize < 4096) return nullptr;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(disk);
    if (dos->e_lfanew < 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > diskSize) return nullptr;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(disk + dos->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);
    if (reinterpret_cast<Byte*>(section + nt->FileHeader.NumberOfSections) > disk + diskSize) return nullptr;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (rva >= section[i].VirtualAddress && static_cast<uint64_t>(rva) + count <= static_cast<uint64_t>(section[i].VirtualAddress) + section[i].SizeOfRawData) {
            auto offset = static_cast<size_t>(section[i].PointerToRawData) + rva - section[i].VirtualAddress;
            return offset + count <= diskSize ? disk + offset : nullptr;
        }
    return nullptr;
}
bool FunctionMatches(void* address, qb::FunctionReference reference) {
    if (!Executable(address)) return false;
    DWORD rva = static_cast<DWORD>(static_cast<Byte*>(address) - image);
    auto bytes = DiskRva(rva, reference.length); if (!bytes) return false;
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(disk);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(disk + dos->e_lfanew);
    const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto functions = reinterpret_cast<RUNTIME_FUNCTION*>(DiskRva(directory.VirtualAddress, directory.Size));
    if (!functions) return false;
    bool found = false;
    for (size_t n = 0; n < directory.Size / sizeof(RUNTIME_FUNCTION); ++n) if (functions[n].BeginAddress == rva) {
        if (functions[n].EndAddress - rva != reference.length) return false;
        found = true; break;
    }
    if (!found && reference.length != 16) return false;
    return qb::BytesHash({bytes, reference.length}) == reference.hash;
}
bool OpenDiskImage() {
    wchar_t path[32768]{}; if (!GetModuleFileNameW(nullptr, path, 32768)) return false;
    diskFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (diskFile == INVALID_HANDLE_VALUE) { diskFile = nullptr; return false; }
    LARGE_INTEGER size{}; if (!GetFileSizeEx(diskFile, &size) || size.QuadPart < 4096) return false;
    diskSize = static_cast<size_t>(size.QuadPart);
    diskMapping = CreateFileMappingW(diskFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!diskMapping) return false;
    disk = static_cast<Byte*>(MapViewOfFile(diskMapping, FILE_MAP_READ, 0, 0, 0)); return disk != nullptr;
}
void Fail(const wchar_t* error) {
    if (!shared) return;
    wcsncpy_s(shared->error, error, _TRUNCATE); qb::Set(shared->enabled, 0); active = false;
    qb::Set(shared->stage, qb::Failed);
}
void* Unique(const char* pattern, int displacementOffset) {
    const auto hits = qb::Find({code, codeSize}, qb::Pattern(pattern));
    if (hits.size() != 1) return nullptr;
    auto match = code + hits[0];
    auto target = match + displacementOffset + 4 + *reinterpret_cast<int32_t*>(match + displacementOffset);
    return InImage(target) ? target : nullptr;
}
bool TypeAt(void* p, int& result) {
    if (!Readable(p, 0x90) || !Readable(*reinterpret_cast<void**>(p), 51 * sizeof(void*))) return false;
    auto fn = VFunction<GetType>(p, 2); if (!Executable(reinterpret_cast<void*>(fn))) return false;
    result = fn(p); return true;
}
bool Snapshot(int index, Identity& result) {
    auto actor = At<void*>(manager, 0x20 + index * sizeof(void*));
    if (!Readable(actor, 0x1B3C) || At<uint16_t>(actor, 0x8C) != index) return false;
    const auto kind = At<uint8_t>(actor, 0x90);
    if (kind != 1 && kind != 2 && kind != 3 && kind != 9 && kind != 10) return false;
    auto draw = At<void*>(actor, 0x100); int drawType = -1;
    if (!Readable(draw, 0xA8) || (At<uint8_t>(draw, 0x89) & 15) != 3 || !TypeAt(draw, drawType) || drawType != 3) return false;
    auto modelFn = VFunction<GetModel>(draw, 50);
    if (!Executable(reinterpret_cast<void*>(modelFn))) { Fail(L"关键模型类型函数异常，已停止。"); return false; }
    const auto modelType = modelFn(draw); if (modelType < 1 || modelType > 3 || !At<void*>(draw, 0xA0)) return false;
    auto parent = At<void*>(draw, 0x18);
    if (parent) { int parentType = -1; if (!TypeAt(parent, parentType) || parentType != 0) return false; }
    if (!Readable(*reinterpret_cast<void**>(actor), 37 * sizeof(void*))) return false;
    if (!Executable(reinterpret_cast<void*>(VFunction<Update>(actor, 36))) ||
        !Executable(reinterpret_cast<void*>(VFunction<Transforms>(draw, 7)))) {
        Fail(L"关键更新函数地址异常，已停止。"); return false;
    }
    const auto scale = At<qb::Scale>(draw, 0x70); if (!qb::Valid(scale)) return false;
    result = {actor, draw, *reinterpret_cast<void**>(actor), *reinterpret_cast<void**>(draw), At<void*>(draw, 0xA0),
        At<uint32_t>(actor, 0x78), At<uint32_t>(actor, 0x84), At<uint32_t>(actor, 0x7C), At<uint32_t>(actor, 0x88),
        At<int>(actor, 0x1B38), kind, static_cast<uint16_t>(index), parent, parent ? *reinterpret_cast<void**>(parent) : nullptr};
    return true;
}
void TrackEntry(Entry& entry, const Identity& id) {
    entry = {}; entry.id = id; entry.tracked = true;
    entry.phaseKey = (static_cast<uint64_t>(id.entity) << 32) ^ id.base ^ (static_cast<uint64_t>(id.index) << 16) ^ id.layout;
    entry.phaseOffset = qb::Phase(entry.phaseKey);
    entry.updateFunction = reinterpret_cast<void*>(VFunction<Update>(id.actor, 36));
    entry.transformFunction = reinterpret_cast<void*>(VFunction<Transforms>(id.draw, 7));
    entry.drawTypeFunction = reinterpret_cast<void*>(VFunction<GetType>(id.draw, 2));
    entry.modelTypeFunction = reinterpret_cast<void*>(VFunction<GetModel>(id.draw, 50));
}
bool Validate(const Entry& entry, Identity& now) {
    // Re-resolve the live slot before touching any cached address. This runs exclusively on the
    // game thread under the detour's SEH guard. Expensive page/type checks are done on admission.
    const auto& id = entry.id;
    auto actor = At<void*>(manager, 0x20 + id.index * sizeof(void*));
    if (!actor || actor != id.actor || *reinterpret_cast<void**>(actor) != id.actorVtable ||
        At<uint16_t>(actor, 0x8C) != id.index || At<uint8_t>(actor, 0x90) != id.kind ||
        At<uint32_t>(actor, 0x78) != id.entity || At<uint32_t>(actor, 0x84) != id.base ||
        At<uint32_t>(actor, 0x7C) != id.layout || At<uint32_t>(actor, 0x88) != id.owner ||
        At<int>(actor, 0x1B38) != id.model || At<void*>(actor, 0x100) != id.draw) return false;
    auto draw = id.draw;
    if (*reinterpret_cast<void**>(draw) != id.drawVtable || At<void*>(draw, 0xA0) != id.skeleton ||
        At<void*>(draw, 0x18) != id.parent || (At<uint8_t>(draw, 0x89) & 15) != 3 ||
        (id.parent && *reinterpret_cast<void**>(id.parent) != id.parentVtable) || !qb::Valid(At<qb::Scale>(draw, 0x70))) return false;
    if (reinterpret_cast<void*>(VFunction<Update>(actor, 36)) != entry.updateFunction ||
        reinterpret_cast<void*>(VFunction<Transforms>(draw, 7)) != entry.transformFunction ||
        reinterpret_cast<void*>(VFunction<GetType>(draw, 2)) != entry.drawTypeFunction ||
        reinterpret_cast<void*>(VFunction<GetModel>(draw, 50)) != entry.modelTypeFunction) {
        Fail(L"已缓存的关键函数入口发生变化，已停止。"); return false;
    }
    now = id; return true;
}
void WriteValidated(const Identity& now, qb::Scale value, bool immediate = true, const qb::Quaternion* rotation = nullptr, bool forceNotify = false) {
    if (!qb::Valid(value)) return;
    if (rotation && !qb::ValidRotation(*rotation)) return;
    bool same = At<qb::Scale>(now.draw, 0x70) == value && (!rotation || At<qb::Quaternion>(now.draw, 0x60) == *rotation);
    if (same && !forceNotify) return;
    if (!same) {
        At<qb::Scale>(now.draw, 0x70) = value;
        if (rotation) At<qb::Quaternion>(now.draw, 0x60) = *rotation;
    }
    At<uint64_t>(now.draw, 0x38) |= 2;
    if (immediate) VFunction<Transforms>(now.draw, 7)(now.draw, false);
}
void RestoreValidated(Entry& entry, const Identity& now, bool immediate) {
    if (!entry.scale.owned && !entry.rotation.owned && !entry.needsNotification) return;
    auto current = entry.rotation.owned ? At<qb::Quaternion>(now.draw, 0x60) : qb::Quaternion{};
    auto restored = entry.rotation.WithoutOwn(current);
    auto scale = At<qb::Scale>(now.draw, 0x70); auto baseline = entry.scale.WithoutOwn(scale);
    bool restoreRotation = entry.rotation.owned && qb::ValidRotation(current);
    bool changed = baseline != scale || (restoreRotation && current != restored);
    WriteValidated(now, baseline, immediate, restoreRotation ? &restored : nullptr, immediate && entry.needsNotification);
    entry.needsNotification = !immediate && (entry.needsNotification || changed);
    entry.scale.owned = false;
    entry.rotation.owned = false;
}
void Restore(Entry& entry) {
    if (!entry.scale.owned && !entry.rotation.owned && !entry.needsNotification) return;
    Identity now{};
    if (Validate(entry, now)) RestoreValidated(entry, now, true);
    entry.scale.owned = false;
    entry.rotation.owned = false;
}
void SafeRestore(Entry& entry) {
    __try { Restore(entry); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        entry.scale.owned = false;
        entry.rotation.owned = false;
        Fail(L"部分模型无法安全恢复，请切换区域或重新登录。");
    }
}
void Clear() {
    active = false;
    for (auto& entry : entries) { if (entry.tracked) SafeRestore(entry); entry = {}; }
    qb::Set(shared->tracked, 0); qb::Set(shared->processed, 0); cursor = 0; frame = 0;
    rotationClock.Reset();
}
bool Included(const Identity& id) {
    int scope = id.kind == 1 ? (id.index == 0 ? qb::Self : qb::Players) : id.kind == 2 ? qb::Monsters : qb::Npcs;
    return (frameScopes & scope) != 0;
}
void ActorAfter(int index, void* actor) {
    if (!active || applying || GetCurrentThreadId() != mainThread || index < 0 || index >= qb::ObjectCount) return;
    auto& entry = entries[index]; Identity now{};
    if (!entry.tracked || entry.id.actor != actor || !Validate(entry, now)) return;
    auto current = At<qb::Scale>(now.draw, 0x70);
    const auto relative = frameRandom ? qb::Evaluate(seconds, frameAmplitude, framePeriod, entry.phaseOffset) : frameRelative;
    const auto value = entry.scale.Apply(current, relative);
    if (!qb::Valid(value)) { entry.scale.owned = false; return; }
    auto rotation = frameRotate ? At<qb::Quaternion>(now.draw, 0x60) : qb::Quaternion{};
    bool spin = frameRotate && qb::ValidRotation(rotation);
    if (spin) rotation = entry.rotation.Apply(rotation, frameRotation);
    applying = true; WriteValidated(now, value, true, spin ? &rotation : nullptr, entry.needsNotification); applying = false;
    entry.needsNotification = false;
    if (entry.lastFrame != frame) { entry.lastFrame = frame; InterlockedIncrement(&shared->processed); }
}
template<size_t N> void __fastcall ActorDetour(void* actor) {
    callbacks.fetch_add(1);
    int index = -1; bool counted = false;
    __try {
        if (active && GetCurrentThreadId() == mainThread && actor) {
            index = At<uint16_t>(actor, 0x8C);
            if (index < qb::ObjectCount && entries[index].tracked && entries[index].id.actor == actor) {
                ++updateDepth[index]; counted = true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { Fail(L"角色入口读取异常，已停止。"); }
    hooks[N].original(actor);
    // Derived actor updates can call another hooked base implementation. Apply once after
    // the outermost update finishes, including any transform changes made after its base call.
    __try { if (!counted || --updateDepth[index] == 0) ActorAfter(index, actor); }
    __except (EXCEPTION_EXECUTE_HANDLER) { applying = false; Fail(L"模型变换异常，已停止。请切图或重新登录恢复模型。"); }
    callbacks.fetch_sub(1);
}
template<size_t... N> constexpr auto Detours(std::index_sequence<N...>) { return std::array<Update, sizeof...(N)>{ActorDetour<N>...}; }
constexpr auto detours = Detours(std::make_index_sequence<16>{});
bool EnsureHook(const Identity& id) {
    auto target = reinterpret_cast<void*>(VFunction<Update>(id.actor, 36));
    for (size_t i = 0; i < hookCount; ++i) if (hooks[i].target == target) return true;
    bool matches = false;
    for (auto reference : qb::UpdateReferences) if (FunctionMatches(target, reference)) { matches = true; break; }
    if (!matches) { Fail(L"关键角色更新函数代码已变化，已停止，需要更新适配。"); return false; }
    if (hookCount == hooks.size()) { Fail(L"更新入口数量异常，已停止。"); return false; }
    auto& hook = hooks[hookCount]; hook.target = target;
    if (MH_CreateHook(target, reinterpret_cast<void*>(detours[hookCount]), reinterpret_cast<void**>(&hook.original)) != MH_OK) {
        hook = {}; Fail(L"无法建立角色更新 Hook，关键入口可能已改变或被占用。"); return false;
    }
    ++hookCount;
    if (MH_EnableHook(target) != MH_OK) { Fail(L"无法启用角色更新 Hook。"); return false; }
    qb::Set(shared->hooks, static_cast<LONG>(hookCount)); return true;
}
void Keys() {
    if (!qb::Read(shared->hotkeys)) return;
    DWORD foregroundPid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    bool inputActive = true;
    if (atkGlobal && Readable(atkGlobal, sizeof(void*)) && Readable(*atkGlobal, 0x30)) {
        auto input = At<void*>(*atkGlobal, 0x28);
        if (Readable(input, 0x1897)) inputActive = At<bool>(input, 0x1896);
    }
    bool allowed = qb::Read(shared->hotkeys) && foregroundPid == GetCurrentProcessId() && !inputActive &&
        !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000);
    for (int i = 0; i < 3; ++i) {
        bool down = (GetAsyncKeyState(VK_F6 + i) & 0x8000) != 0;
        if (allowed && down && !KeyDown[i]) {
            if (i == 0) qb::Set(shared->enabled, !qb::Read(shared->enabled));
            else if (i == 1) { auto a = qb::Read(shared->amplitude); qb::Set(shared->amplitude, a < 190 ? 200 : a < 390 ? 400 : a < 590 ? 600 : 0); }
            else InterlockedIncrement(&shared->reset);
        }
        KeyDown[i] = down;
    }
}
void Frame() {
    mainThread = GetCurrentThreadId();
    if (qb::Read(shared->shutdown) || GetTickCount64() - static_cast<ULONGLONG>(qb::Heartbeat(*shared)) > 3000) {
        Clear(); qb::Set(shared->shutdown, 1); qb::Set(shared->stage, qb::Stopped); return;
    }
    if (qb::Read(shared->stage) == qb::Failed) { if (active || frame) Clear(); return; }
    Keys();
    const auto zone = At<uint32_t>(gameMain, 0x4108);
    bool scene = zone != 0 && At<uint32_t>(gameMain, 0x4100) == 2 && !isGPose() &&
        !conditions[35] && !conditions[45] && !conditions[51] && !conditions[58] && !conditions[78];
    qb::Set(shared->scene, scene);
    const auto reset = qb::Read(shared->reset);
    if (territory != zone || reset != lastReset) { Clear(); territory = zone; lastReset = reset; }
    if (!scene || !qb::Read(shared->enabled)) { if (active || frame) Clear(); return; }
    if (!active) { QueryPerformanceCounter(&epoch); active = true; }
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now); seconds = static_cast<double>(now.QuadPart - epoch.QuadPart) / frequency.QuadPart;
    frameAmplitude = qb::Read(shared->amplitude); framePeriod = qb::Read(shared->period);
    frameScopes = qb::Read(shared->scopes); frameRandom = qb::Read(shared->random) != 0;
    frameRelative = qb::Evaluate(seconds, frameAmplitude, framePeriod);
    frameRotate = qb::Read(shared->rotate) != 0;
    frameRotation = rotationClock.Advance(seconds, qb::Read(shared->rotationSpeed), frameRotate);
    ++frame; qb::Set(shared->processed, 0);
    for (auto& entry : entries) {
        if (!entry.tracked) continue;
        Identity current{};
        if (!Validate(entry, current)) {
            entry = {};
            if (qb::Read(shared->stage) == qb::Failed) return;
            continue;
        }
        // While running, remove our previous value before the game's own update. Marking the
        // render transform dirty is enough here; stop/unload restores still notify immediately.
        const bool included = Included(current);
        RestoreValidated(entry, current, !included); if (!included) entry = {};
    }
    const int budget = frame == 1 ? qb::ObjectCount : (qb::ObjectCount + 7) / 8;
    for (int n = 0; n < budget; ++n) {
        int index = static_cast<int>(cursor++ % qb::ObjectCount); if (cursor >= qb::ObjectCount) cursor = 0;
        if (entries[index].tracked) continue;
        Identity id{}; if (!Snapshot(index, id) || !Included(id)) continue;
        if (!EnsureHook(id)) return;
        TrackEntry(entries[index], id);
    }
    int count = 0; for (auto& entry : entries) count += entry.tracked;
    qb::Set(shared->tracked, count); qb::Set(shared->resetEcho, reset);
}
bool __fastcall TickDetour(void* framework) {
    callbacks.fetch_add(1);
    __try { Frame(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { applying = false; Fail(L"帧更新结构校验异常，已停止。请切图或重新登录恢复模型。"); }
    bool result = originalTick(framework);
    callbacks.fetch_sub(1); return result;
}
bool Initialize() {
    image = reinterpret_cast<Byte*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;
    imageSize = nt->OptionalHeader.SizeOfImage;
    auto section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) if (!memcmp(section[i].Name, ".text", 5)) {
        code = image + section[i].VirtualAddress; codeSize = section[i].Misc.VirtualSize;
    }
    if (!code || !OpenDiskImage()) return false;
    manager = static_cast<Byte*>(Unique("48 8D 35 ?? ?? ?? ?? 81 FA", 3));
    frameworkGlobal = static_cast<void**>(Unique("48 8B 1D ?? ?? ?? ?? 8B 7C 24", 3));
    conditions = static_cast<Byte*>(Unique("48 8D 0D ?? ?? ?? ?? 66 2B D8", 3));
    gameMain = static_cast<Byte*>(Unique("48 8D 0D ?? ?? ?? ?? 0F 28 F2 48 89 44 24 ??", 3));
    isGPose = reinterpret_cast<GPose>(Unique("E8 ?? ?? ?? ?? 41 83 7E ?? 00 4C 8D 3D", 1));
    atkGlobal = static_cast<void**>(Unique("48 8B 05 ?? ?? ?? ?? 4C 8B 40 18 45 8B 40 18", 3));
    auto frameworkVtable = Unique("48 8D 05 ?? ?? ?? ?? ?? ?? ?? 48 8B F1 49 89 6B", 3);
    if (!manager || !frameworkGlobal || !conditions || !gameMain || !isGPose || !atkGlobal || !frameworkVtable) return false;
    if (!InImage(manager, 0x20 + qb::ObjectCount * 8) || !InImage(gameMain, 0x410C) || !InImage(conditions, 112) ||
        !Readable(frameworkVtable, 5 * sizeof(void*))) return false;
    tickTarget = static_cast<void**>(frameworkVtable)[4];
    if (!Executable(tickTarget) || !Executable(reinterpret_cast<void*>(isGPose))) return false;
    if (!FunctionMatches(tickTarget, qb::TickReference) || !FunctionMatches(reinterpret_cast<void*>(isGPose), qb::GPoseReference)) {
        Fail(L"关键帧更新或 GPose 函数代码已变化，未启用，需要更新适配。"); return false;
    }
    if (!Readable(frameworkGlobal, 8) || !*frameworkGlobal || !Readable(*frameworkGlobal, 8)) return false;
    if (VFunction<void*>(*frameworkGlobal, 4) != tickTarget) return false;
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&epoch);
    if (MH_Initialize() != MH_OK) return false;
    return MH_CreateHook(tickTarget, reinterpret_cast<void*>(&TickDetour), reinterpret_cast<void**>(&originalTick)) == MH_OK && MH_EnableHook(tickTarget) == MH_OK;
}
bool SafeInitialize() {
    __try { return Initialize(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}

extern "C" __declspec(dllexport) DWORD WINAPI QBounceStart(void*) {
    if (InterlockedCompareExchange(&started, 1, 0)) return 1;
    wchar_t name[96]{}; qb::MappingName(name, 96, GetCurrentProcessId());
    mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!mapping) { InterlockedExchange(&started, 0); return 2; }
    shared = static_cast<qb::Shared*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(qb::Shared)));
    if (!shared || shared->magic != qb::Magic || shared->abi != qb::Abi) {
        if (shared) UnmapViewOfFile(shared); shared = nullptr; CloseHandle(mapping); mapping = nullptr; InterlockedExchange(&started, 0); return 3;
    }
    if (!SafeInitialize()) { if (!shared->error[0]) Fail(L"关键函数签名不唯一、入口或结构校验失败，未启用效果。"); }
    else {
        qb::Set(shared->stage, qb::Ready);
        while (qb::Read(shared->stage) != qb::Stopped) {
            if (qb::Read(shared->stage) == qb::Failed && !frame) break;
            Sleep(20);
        }
    }
    active = false;
    for (size_t i = 0; i < hookCount; ++i) if (hooks[i].target) MH_DisableHook(hooks[i].target);
    if (tickTarget) MH_DisableHook(tickTarget);
    while (callbacks.load() != 0) Sleep(1);
    for (size_t i = 0; i < hookCount; ++i) if (hooks[i].target) MH_RemoveHook(hooks[i].target);
    if (tickTarget) MH_RemoveHook(tickTarget);
    MH_Uninitialize(); hooks = {}; hookCount = 0; tickTarget = nullptr; mainThread = 0;
    if (disk) UnmapViewOfFile(disk); disk = nullptr;
    if (diskMapping) CloseHandle(diskMapping); diskMapping = nullptr;
    if (diskFile) CloseHandle(diskFile); diskFile = nullptr;
    if (qb::Read(shared->stage) != qb::Failed) qb::Set(shared->stage, qb::Stopped);
    UnmapViewOfFile(shared); shared = nullptr; CloseHandle(mapping); mapping = nullptr;
    InterlockedExchange(&started, 0);
    // Keep the inert image resident until the game exits, avoiding an unload/callback race.
    return 0;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) self = module;
    return TRUE;
}

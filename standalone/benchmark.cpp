#define QB_PERF_TEST
#include "module.cpp"
#include <iostream>
#include <vector>

namespace {
volatile LONG transformCalls;
int __fastcall MockDrawType(void*) { return 3; }
int __fastcall MockParentType(void*) { return 0; }
uint8_t __fastcall MockModelType(void*) { return 1; }
void __fastcall MockUpdate(void*) {}
void __fastcall MockTransforms(void*, bool) { InterlockedIncrement(&transformCalls); }
void __fastcall MockInheritedUpdate(void* actor) {
    ActorDetour<1>(actor);
    // A derived update is allowed to finish changing its draw transform after the base returns.
    At<qb::Scale>(At<void*>(actor, 0x100), 0x70) = {};
}
bool MockGPose() { return false; }
struct Fixture { std::array<Byte, 0x2400> actor{}; std::array<Byte, 0xA20> draw{}; };
}

int main(int argc, char** argv) {
    constexpr int actors = 100, iterations = 240;
    qb::Shared block; shared = &block; qb::Set(block.enabled, 1); qb::Set(block.stage, qb::Ready);
    image = reinterpret_cast<Byte*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image + reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
    imageSize = nt->OptionalHeader.SizeOfImage;
    auto sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) if (!memcmp(sections[i].Name, ".text", 5)) {
        code = image + sections[i].VirtualAddress; codeSize = sections[i].Misc.VirtualSize;
    }
    std::vector<Byte> objectManager(0x4D20), gameState(0x4170), flags(112);
    manager = objectManager.data(); gameMain = gameState.data(); conditions = flags.data(); isGPose = MockGPose;
    At<uint32_t>(gameMain, 0x4100) = 2; At<uint32_t>(gameMain, 0x4108) = 1;
    std::array<void*, 64> actorTable{}, drawTable{}, parentTable{};
    actorTable[36] = reinterpret_cast<void*>(&MockUpdate);
    drawTable[2] = reinterpret_cast<void*>(&MockDrawType); drawTable[7] = reinterpret_cast<void*>(&MockTransforms); drawTable[50] = reinterpret_cast<void*>(&MockModelType);
    parentTable[2] = reinterpret_cast<void*>(&MockParentType);
    std::array<Byte, 0x90> parent{}; At<void*>(parent.data(), 0) = parentTable.data();
    std::vector<Fixture> fixtures(actors);
    for (int i = 0; i < actors; ++i) {
        auto a = fixtures[i].actor.data(), d = fixtures[i].draw.data();
        At<void*>(a, 0) = actorTable.data(); At<uint16_t>(a, 0x8C) = static_cast<uint16_t>(i); At<uint8_t>(a, 0x90) = i == 0 ? 1 : 3;
        At<uint32_t>(a, 0x78) = 100 + i; At<void*>(a, 0x100) = d;
        At<void*>(d, 0) = drawTable.data(); At<void*>(d, 0x18) = parent.data(); At<uint8_t>(d, 0x89) = 3;
        At<void*>(d, 0xA0) = reinterpret_cast<void*>(12345); At<qb::Scale>(d, 0x70) = {};
        At<void*>(manager, 0x20 + i * 8) = a;
        Identity id{}; if (!Snapshot(i, id)) return 2;
        TrackEntry(entries[i], id);
    }
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&epoch);
    territory = 1; mainThread = GetCurrentThreadId(); active = true; frame = 1;
    memoryQueries = 0; transformCalls = 0;
    LARGE_INTEGER start{}, finish{}; QueryPerformanceCounter(&start);
    for (int n = 0; n < iterations; ++n) {
        InterlockedExchange64(&block.heartbeat, static_cast<LONG64>(GetTickCount64()));
        Frame();
        for (int i = 0; i < actors; ++i) ActorAfter(i, fixtures[i].actor.data());
        if (qb::Read(block.processed) != actors || qb::Read(block.stage) != qb::Ready) return 3;
    }
    QueryPerformanceCounter(&finish);
    double ms = (finish.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart / iterations;
    double queries = static_cast<double>(memoryQueries) / iterations, updates = static_cast<double>(transformCalls) / iterations;
    std::cout << "{\"Actors\":" << actors << ",\"Frames\":" << iterations << ",\"MillisecondsPerFrame\":" << ms
        << ",\"VirtualQueriesPerFrame\":" << queries << ",\"TransformNotificationsPerFrame\":" << updates << "}\n";
    // Character::Update may call the hooked GameObject::Update. Only the outer return should
    // apply the effect; applying at both returns duplicates the native skeleton transform work.
    hooks[0].original = MockInheritedUpdate; hooks[1].original = MockUpdate;
    entries[0].scale = {}; At<qb::Scale>(fixtures[0].draw.data(), 0x70) = {};
    frameRelative = qb::Evaluate(.1125, 400, 450); transformCalls = 0;
    ActorDetour<0>(fixtures[0].actor.data());
    if (transformCalls != 1) { std::cerr << "FAIL: inherited actor update must notify once\n"; return 4; }
    int safetyCases = 1;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::cerr << "FAIL: " << label << '\n'; std::exit(5); }
        ++safetyCases;
    };
    auto a = fixtures[0].actor.data(), d = fixtures[0].draw.data(); Identity live{};
    check(Validate(entries[0], live), "cached live actor remains valid");
    auto deformed = At<qb::Scale>(d, 0x70);
    At<void*>(manager, 0x20) = nullptr; Restore(entries[0]);
    check(At<qb::Scale>(d, 0x70) == deformed && !Validate(entries[0], live), "despawn must not restore cached draw");
    At<void*>(manager, 0x20) = a;
    At<uint32_t>(a, 0x78)++; check(!Validate(entries[0], live), "address reuse changes entity identity"); At<uint32_t>(a, 0x78)--;
    At<uint32_t>(a, 0x88)++; check(!Validate(entries[0], live), "owner identity changed"); At<uint32_t>(a, 0x88)--;
    At<int>(a, 0x1B38)++; check(!Validate(entries[0], live), "model identity changed"); At<int>(a, 0x1B38)--;
    At<void*>(d, 0xA0) = nullptr; check(!Validate(entries[0], live), "skeleton invalidated"); At<void*>(d, 0xA0) = entries[0].id.skeleton;
    At<void*>(d, 0x18) = nullptr; check(!Validate(entries[0], live), "parent changed"); At<void*>(d, 0x18) = parent.data();
    At<void*>(parent.data(), 0) = drawTable.data(); check(!Validate(entries[0], live), "parent vtable changed"); At<void*>(parent.data(), 0) = parentTable.data();
    At<uint8_t>(d, 0x89) = 0; check(!Validate(entries[0], live), "unloaded model rejected"); At<uint8_t>(d, 0x89) = 3;
    drawTable[2] = reinterpret_cast<void*>(&MockParentType); check(!Snapshot(0, live), "unknown draw type rejected on admission");
    drawTable[2] = reinterpret_cast<void*>(&MockDrawType);
    drawTable[7] = reinterpret_cast<void*>(&MockUpdate);
    check(!Validate(entries[0], live) && qb::Read(block.stage) == qb::Failed && !qb::Read(block.enabled), "changed critical function stops effect");
    drawTable[7] = reinterpret_cast<void*>(&MockTransforms); qb::Set(block.stage, qb::Ready); qb::Set(block.enabled, 1); active = true;
    // A dead cached actor pointer must not be read once its object table slot has gone away.
    auto guard = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
    check(guard != nullptr, "guard page allocated");
    auto guardedEntry = entries[0]; guardedEntry.id.actor = guard; At<void*>(manager, 0x20) = nullptr;
    check(!Validate(guardedEntry, live), "empty live slot checked before inaccessible cached actor");
    VirtualFree(guard, 0, MEM_RELEASE); At<void*>(manager, 0x20) = a;
    auto readmitted = entries[0].id;
    At<qb::Scale>(d, 0x70) = {}; TrackEntry(entries[0], readmitted);
    qb::Set(block.enabled, 0); Frame();
    check(!active && qb::Read(block.tracked) == 0, "disable clears tracked actors");
    for (int i = 0; i < actors; ++i) check(At<qb::Scale>(fixtures[i].draw.data(), 0x70) == qb::Scale{}, "disable restores original scale");
    hookCount = 1; hooks[0].target = reinterpret_cast<void*>(&MockUpdate);
    qb::Set(block.enabled, 1); Frame();
    check(qb::Read(block.tracked) == actors, "enable discovers loaded actors");
    for (int i = 0; i < actors; ++i) ActorAfter(i, fixtures[i].actor.data());
    At<void*>(manager, 0x28) = nullptr; Frame();
    check(!entries[1].tracked, "despawn drops entry on next frame");
    auto npc = fixtures[1].actor.data(), npcDraw = fixtures[1].draw.data();
    At<uint32_t>(npc, 0x78) += 500; At<qb::Scale>(npcDraw, 0x70) = {2, 1, 3};
    At<void*>(manager, 0x28) = npc;
    for (int n = 0; n < 8; ++n) Frame();
    check(entries[1].tracked && entries[1].id.entity == At<uint32_t>(npc, 0x78), "new NPC and reused slot admitted within eight frames");
    ActorAfter(1, npc);
    At<uint32_t>(gameMain, 0x4108) = 0; Frame();
    check(!active && qb::Read(block.tracked) == 0 && At<qb::Scale>(npcDraw, 0x70) == qb::Scale{2, 1, 3}, "territory transition restores current identity baseline");
    At<uint32_t>(gameMain, 0x4108) = 2; Frame();
    check(active && entries[1].tracked, "new territory rediscovers models");
    ActorAfter(1, npc); qb::Set(block.shutdown, 1); Frame();
    check(qb::Read(block.stage) == qb::Stopped && !active && At<qb::Scale>(npcDraw, 0x70) == qb::Scale{2, 1, 3}, "shutdown restores and stops callbacks");
    std::cout << "PASS: " << safetyCases << " production callback/lifecycle checks\n";
    if (argc > 1 && !strcmp(argv[1], "--assert-efficient") && (queries > 1000 || updates > 150)) {
        std::cerr << "FAIL: redundant per-frame validation/transform work\n"; return 1;
    }
    return 0;
}

#pragma once
#include <Windows.h>
#include <cstdint>
#include <cwchar>

namespace qb {
constexpr uint32_t Magic = 0x51424E43;
constexpr uint32_t Abi = 2;
constexpr int ObjectCount = 819;
constexpr wchar_t ReferenceVersion[] = L"2026.11.03.0000.0000";
enum Stage : LONG { Idle, Initializing, Ready, Stopping, Stopped, Failed };
inline bool MayEnable(LONG stage) { return stage == Ready; }
enum Scope : LONG { Self = 1, Players = 2, Npcs = 4, Monsters = 8 };
struct alignas(8) Shared {
    uint32_t magic = Magic, abi = Abi;
    DWORD controllerPid = 0;
    volatile LONG enabled = 0, amplitude = 400, period = 450, random = 0, scopes = 15;
    volatile LONG hotkeys = 0, bgm = 1, muted = 0, volume = 350, reset = 0, shutdown = 0;
    alignas(8) volatile LONG64 heartbeat = 0;
    volatile LONG stage = Initializing, processed = 0, tracked = 0, hooks = 0, scene = 0, resetEcho = 0;
    wchar_t error[256]{};
    volatile LONG rotate = 0, rotationSpeed = 90;
};
inline LONG Read(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
inline void Set(volatile LONG& value, LONG next) { InterlockedExchange(&value, next); }
inline LONG64 Heartbeat(Shared& block) { return InterlockedCompareExchange64(&block.heartbeat, 0, 0); }
inline void MappingName(wchar_t* text, size_t count, DWORD pid) { swprintf_s(text, count, L"Local\\QBounceStandalone-%lu", pid); }
}

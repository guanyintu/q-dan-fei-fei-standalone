#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace qb {
inline uint64_t BytesHash(std::span<const uint8_t> bytes) {
    uint64_t value = 14695981039346656037ULL;
    for (auto byte : bytes) { value ^= byte; value *= 1099511628211ULL; }
    return value;
}
struct Identity {
    void *actor = nullptr, *draw = nullptr, *actorVtable = nullptr, *drawVtable = nullptr, *skeleton = nullptr;
    uint32_t entity = 0, base = 0, layout = 0, owner = 0;
    int model = 0; uint8_t kind = 0; uint16_t index = 0;
    void* parent = nullptr;
    void* parentVtable = nullptr;
    bool operator==(const Identity&) const = default;
};
struct Scale { float x = 1, y = 1, z = 1; bool operator==(const Scale&) const = default; };
inline Scale Multiply(Scale a, Scale b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline bool Valid(Scale s) {
    return std::isfinite(s.x) && std::isfinite(s.y) && std::isfinite(s.z) &&
        s.x > .0001f && s.y > .0001f && s.z > .0001f && s.x < 10000 && s.y < 10000 && s.z < 10000;
}
inline double Phase(uint64_t key) {
    auto v = key + 0x9E3779B97F4A7C15ULL;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
    return ((v ^ (v >> 31)) >> 11) * (1.0 / (1ULL << 53));
}
inline Scale Evaluate(double time, int amplitude, int periodMs, double phase = 0) {
    if (!std::isfinite(time) || !std::isfinite(phase)) return {};
    auto cycles = time / (std::clamp(periodMs, 200, 1500) / 1000.0) + phase;
    float y = 1 + std::clamp(amplitude, 0, 600) / 1000.0f * static_cast<float>(std::sin(6.283185307179586 * (cycles - std::floor(cycles))));
    auto h = 1 / std::sqrt(y);
    return {h, y, h};
}
struct OwnedScale {
    Scale baseline{}, last{};
    bool owned = false;
    Scale WithoutOwn(Scale value) const {
        if (!owned) return value;
        return {value.x == last.x ? baseline.x : value.x,
            value.y == last.y ? baseline.y : value.y, value.z == last.z ? baseline.z : value.z};
    }
    Scale Apply(Scale current, Scale relative) {
        baseline = WithoutOwn(current); last = Multiply(baseline, relative); owned = true; return last;
    }
};
inline std::vector<int> Pattern(std::string_view text) {
    std::vector<int> result;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') { ++i; continue; }
        if (text[i] == '?') { result.push_back(-1); while (i < text.size() && text[i] == '?') ++i; continue; }
        if (i + 1 >= text.size()) return {};
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        int a = hex(text[i]), b = hex(text[i + 1]); if (a < 0 || b < 0) return {};
        result.push_back(a * 16 + b); i += 2;
    }
    return result;
}
inline std::vector<size_t> Find(std::span<const uint8_t> data, const std::vector<int>& pattern) {
    std::vector<size_t> hits;
    if (pattern.empty() || pattern.size() > data.size()) return hits;
    for (size_t i = 0; i <= data.size() - pattern.size(); ++i) {
        size_t j = 0; for (; j < pattern.size(); ++j) if (pattern[j] >= 0 && data[i + j] != pattern[j]) break;
        if (j == pattern.size()) hits.push_back(i);
    }
    return hits;
}
}

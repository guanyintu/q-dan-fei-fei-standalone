#include "core.hpp"
#include "protocol.hpp"
#include "reference.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

void Check(bool result, const char* label) { if (!result) throw std::runtime_error(label); }
int main() {
    int cases = 0;
    try {
        for (int n = 0; n <= 10000; ++n) {
            auto value = qb::Evaluate(n * .45 / 10000, 400, 450);
            Check(value.y >= .6f - 1e-6f && value.y <= 1.4f + 1e-6f, "default vertical range");
            Check(std::abs(value.x * value.y * value.z - 1) < 1e-6f, "volume compensation");
            Check(value.x == value.z && std::abs(value.x - 1 / std::sqrt(value.y)) < 1e-6f, "horizontal axes");
        } ++cases;
        Check(std::abs(qb::Evaluate(.1125, 400, 450).y - 1.4f) < 1e-6f, "maximum"); ++cases;
        Check(std::abs(qb::Evaluate(.3375, 400, 450).y - .6f) < 1e-6f, "minimum"); ++cases;
        Check(qb::Evaluate(10, 0, 450) == qb::Scale{}, "zero strength"); ++cases;
        Check(qb::Evaluate(std::numeric_limits<double>::quiet_NaN(), 400, 450) == qb::Scale{}, "invalid clock"); ++cases;
        for (auto a : {-100, 0, 400, 600, 999}) for (auto period : {-5, 0, 450, 1500, 9000})
            Check(qb::Valid(qb::Evaluate(10, a, period)), "clamped settings"); ++cases;
        for (auto value : {0., 1000., 315360000.}) {
            auto a = qb::Evaluate(value, 400, 450), b = qb::Evaluate(value + .45, 400, 450);
            Check(std::abs(a.y - b.y) < 1e-6f, "period stability");
        } ++cases;
        Check(qb::Phase(123) == qb::Phase(123) && qb::Phase(123) != qb::Phase(456), "random phase stability"); ++cases;
        for (uint64_t id = 0; id < 1000; ++id) Check(qb::Phase(id) >= 0 && qb::Phase(id) < 1, "random phase bounds"); ++cases;
        qb::OwnedScale state; qb::Scale original{1.2f, 1.5f, .9f};
        auto current = original;
        for (int n = 0; n < 10000; ++n) {
            auto relative = qb::Evaluate(n / 60., 400, 450);
            current = state.Apply(current, relative); current = state.Apply(current, relative);
            Check(current == qb::Multiply(original, relative), "non-accumulation");
            current = state.WithoutOwn(current); state.owned = false;
            Check(current == original, "restore baseline");
        } ++cases;
        current = state.Apply(original, qb::Evaluate(.1, 400, 450)); current.y = 2.5f;
        Check(state.WithoutOwn(current) == qb::Scale{original.x, 2.5f, original.z}, "external axis ownership"); ++cases;
        qb::Scale external{2, 3, 4}; Check(state.WithoutOwn(external) == external, "external replacement"); ++cases;
        Check(!qb::Valid({std::numeric_limits<float>::infinity(), 1, 1}) && !qb::Valid({0, 1, 1}), "invalid scale rejected"); ++cases;
        qb::Identity identity{}; identity.actor = reinterpret_cast<void*>(100); identity.entity = 1;
        auto replacement = identity; replacement.entity = 2; Check(replacement != identity, "same address new entity"); ++cases;
        replacement = identity; replacement.draw = reinterpret_cast<void*>(200); Check(replacement != identity, "redraw identity"); ++cases;
        replacement = identity; replacement.skeleton = reinterpret_cast<void*>(300); Check(replacement != identity, "skeleton identity"); ++cases;
        const uint8_t bytes[]{0x48,0x8D,0x10,0x20,0x48,0x8D,0x11,0x20};
        auto hits = qb::Find(bytes, qb::Pattern("48 8D ?? 20")); Check(hits.size() == 2 && hits[1] == 4, "ambiguous signature"); ++cases;
        Check(qb::Find(bytes, qb::Pattern("48 8D 10 20")).size() == 1, "unique signature"); ++cases;
        Check(qb::Find(bytes, qb::Pattern("FF FF")).empty(), "missing critical signature"); ++cases;
        Check(qb::Pattern("GG").empty() && qb::Find(bytes, {}).empty(), "invalid signature"); ++cases;
        std::vector<uint8_t> changed(bytes, bytes + sizeof(bytes));
        auto reference = qb::BytesHash(changed); changed[3] ^= 1;
        Check(qb::BytesHash(changed) != reference, "critical code mutation detection"); ++cases;
        qb::Shared shared; Check(shared.enabled == 0 && shared.amplitude == 400 && shared.period == 450, "safe startup"); ++cases;
        qb::Set(shared.shutdown, 1); Check(qb::Read(shared.shutdown) == 1, "control IPC"); ++cases;
        bool companion = false;
        for (auto entryReference : qb::UpdateReferences)
            companion |= entryReference.length == 1104 && entryReference.hash == 0x07c2a6c43166811fULL;
        Check(companion, "current-client companion update entry must be included"); ++cases;
        Check(qb::MayEnable(qb::Ready) && !qb::MayEnable(qb::Failed) && !qb::MayEnable(qb::Initializing) &&
            !qb::MayEnable(qb::Stopping) && !qb::MayEnable(qb::Stopped), "effect switch must reject failed connection state"); ++cases;
        std::cout << "PASS: " << cases << " native test cases\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}

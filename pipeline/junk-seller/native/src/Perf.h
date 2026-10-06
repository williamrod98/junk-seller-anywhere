#pragma once
// Per-stage cost meters. B-06 asks for the preparation cost, the form walk and
// the xList walk to be metered separately, so every stage owns one Meter and the
// HEALTH row prints p99 and max per stage instead of a single aggregate.
#include <array>
#include <atomic>
#include <cstdint>
namespace js {
class Meter {
public:
    // Bucket k covers [k * StepUs, (k + 1) * StepUs) microseconds.
    // The last bucket is the overflow bucket and reports the observed max.
    static constexpr unsigned Buckets = 64;
    static constexpr std::uint64_t StepUs = 16;
    void Configure(std::uint64_t limitUs) noexcept { _limitUs.store(limitUs); }
    std::uint64_t Limit() const noexcept { return _limitUs.load(); }
    bool Observe(std::uint64_t us) noexcept {
        ++samples;
        sum += us;
        std::uint64_t seen = max.load();
        while (us > seen && !max.compare_exchange_weak(seen, us)) {}
        auto index = us / StepUs;
        if (index >= Buckets) index = Buckets - 1;
        ++_buckets[static_cast<std::size_t>(index)];
        const auto limit = _limitUs.load();
        if (limit && us > limit) { ++overruns; return false; }
        return true;
    }
    // Upper edge of the bucket holding the 99th percentile. The overflow bucket
    // reports the observed maximum, never a fabricated interpolation.
    // r3 (probe-js01 anomaly 3): the bucket edge is an upper bound on the values
    // in that bucket, not a value anyone observed, so a stage whose samples were
    // all 0 us used to report p99=16 against max=0. The edge is now clamped to
    // the observed maximum, which no percentile can exceed by definition.
    std::uint64_t P99() const noexcept {
        const auto total = samples.load();
        if (!total) return 0;
        const auto observed = max.load();
        const auto target = (total * 99 + 99) / 100;
        std::uint64_t seen = 0;
        for (unsigned i = 0; i < Buckets; ++i) {
            seen += _buckets[i].load();
            if (seen >= target) {
                if (i + 1 == Buckets) return observed;
                const std::uint64_t edge = (i + 1) * StepUs;
                return edge < observed ? edge : observed;
            }
        }
        return observed;
    }
    std::atomic<std::uint64_t> samples{}, sum{}, max{}, overruns{};
private:
    std::array<std::atomic<std::uint64_t>, Buckets> _buckets{};
    std::atomic<std::uint64_t> _limitUs{};
};
// Budget arithmetic shared by every staged task (B-01 / B-02). Kept engine free
// so the offline harness can pin the boundary behaviour.
inline std::uint64_t DeadlineNs(std::uint64_t startNs, std::uint64_t budgetUs) noexcept {
    return budgetUs ? startNs + budgetUs * 1000 : 0;
}
inline bool DeadlineReached(std::uint64_t nowNs, std::uint64_t deadlineNs) noexcept {
    return deadlineNs != 0 && nowNs >= deadlineNs;
}
// Stage meters. Sink and hook capture run on unverified threads; task, format
// and the three inventory stages run inside SKSE tasks.
inline Meter sinkMeter, hookMeter, taskMeter, formatMeter, prepareMeter, formScanMeter, xListScanMeter;
}

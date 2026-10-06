#pragma once
// Compact, engine-free diagnostics for the last published oracle sample.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace js {
struct OracleSample;

struct OracleVerdict {
    std::uint64_t lastSampleId{}, lastSpanUs{}, menuSamples{}, menuValid{};
    const char* lastOrigin{"none"};
    bool lastSampleValid{}, lastTargeted{}, cachedBeforeUsable{};
    const char* lastInvalidReason{"none"};
    std::array<std::uint32_t, 6> lastStageCostUs{};
    std::array<std::uint32_t, 6> lastChunks{};
    std::uint32_t lastRestarts{};
    std::uint32_t lastScanned{}, lastBaseCountVisits{}, lastTruncatedStages{}, lastUnsampled{};
    std::array<std::uint64_t, 6> menuInvalid{};
};

OracleVerdict SummarizeSample(const OracleSample& sample) noexcept;

struct MenuOracleTally {
    std::uint64_t generation{}, samples{}, valid{};
    std::array<std::uint64_t, 6> invalid{};

    void Reset(std::uint64_t newGeneration) noexcept {
        generation = newGeneration;
        samples = valid = 0;
        invalid = {};
    }
    void Add(const OracleVerdict& verdict) noexcept {
        ++samples;
        if (verdict.lastSampleValid) {
            ++valid;
            return;
        }
        constexpr const char* reasons[] = {"TRUNCATED", "UNSAMPLED_CANDIDATE",
            "INVENTORY_CHANGED", "STAGES_INCOMPLETE", "VISIT_CAP"};
        for (std::size_t i = 0; i < 5; ++i) {
            if (std::strcmp(verdict.lastInvalidReason, reasons[i]) == 0) {
                ++invalid[i];
                return;
            }
        }
        ++invalid[5];
    }
};
}  // namespace js

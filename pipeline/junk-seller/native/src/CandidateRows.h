#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace js {
inline constexpr std::array<const char*, 25> CandidateExclusionTerms{
    "type", "price", "weight", "ratio", "contract", "quest", "enchanted",
    "favorite", "equipped", "owner_unknown", "name_protected", "formlist_protected",
    "stolen", "mixed_protection", "candidate_invalid", "balance", "quote",
    "zero_gold", "run_limit", "balance_unknown", "not_displayed", "no_quote",
    "identity_unresolved", "facts_unreadable", "category_off"};

struct CandidateRowSummary {
    static constexpr std::uint32_t ExcludeRowCap = 256;
    std::array<std::uint32_t, CandidateExclusionTerms.size()> excludedByTerm{};
    std::uint32_t excludeRows{}, candidateRowsSuppressed{};

    // Only planning-time EXCLUDE rows use this policy. Dispatch-time rows and
    // ISSUE rows bypass it so every attempted sale remains visible.
    bool RecordExclude(std::string_view reason) noexcept {
        for (std::size_t i = 0; i < CandidateExclusionTerms.size(); ++i) {
            if (reason == CandidateExclusionTerms[i]) {
                ++excludedByTerm[i];
                break;
            }
        }
        if (reason == "type" || reason == "category_off" || excludeRows == ExcludeRowCap) {
            ++candidateRowsSuppressed;
            return false;
        }
        ++excludeRows;
        return true;
    }
};
}

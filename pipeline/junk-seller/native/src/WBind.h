#pragma once
// Engine-free BindKey candidate and source decisions.
#include <cstddef>
#include <cstdint>
#include <string>

namespace js {
struct BindCandidateInput {
    std::uint32_t itemBase{}, entryFormId{};
    bool entryFormIdKnown{}, filteredIndexKnown{}, infoValueKnown{};
};
struct BindDecision { std::int32_t index{-1}; bool truncated{}; const char* reason{"no_items_match"}; };
inline BindDecision FindBindCandidate(const BindCandidateInput* entries, std::size_t size,
                                      std::uint32_t target) noexcept {
    const auto limit = size < 256 ? size : 256;
    const char* lastReason = "no_items_match";
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& e = entries[i];
        if (e.itemBase != target) continue;
        if (!e.entryFormIdKnown || e.entryFormId != target) {
            lastReason = "entry_formid_mismatch";
            continue;
        }
        if (!e.filteredIndexKnown) {
            lastReason = "filtered_index_null";
            continue;
        }
        return {static_cast<std::int32_t>(i), size > limit, "none"};
    }
    return {-1, size > limit, lastReason};
}
inline const char* BindTaskRefusal(bool bindEnabled, bool menuOpen, bool inFlight) noexcept {
    if (!bindEnabled) return "bind_disabled";
    if (!menuOpen) return "menu_closed";
    return inFlight ? "in_flight" : nullptr;
}
inline bool BindReadbackMatches(bool candidateFound, bool selectedSameCandidate,
                                std::uint32_t selectedBase, std::uint32_t target) noexcept {
    return candidateFound && selectedSameCandidate && selectedBase == target;
}
inline bool ParseBindTargetBase(const std::string& value, std::uint32_t& target) noexcept {
    try {
        std::size_t used = 0;
        const auto number = std::stoul(value, &used, 0);
        if (used != value.size() || number > UINT32_MAX) return false;
        target = static_cast<std::uint32_t>(number);
        return true;
    } catch (...) { return false; }
}
inline const char* BindResponseValueSource(bool fromBind, bool captureAccepted) noexcept {
    return fromBind ? (captureAccepted ? "manual_capture_same_base" : "fixed_zero")
                    : (captureAccepted ? "manual_capture_same_base" : "none");
}
inline const char* BindUnitPriceSource(bool fromBind, bool gfxKnown, bool manualKnown) noexcept {
    return fromBind ? (gfxKnown ? "gfx_infoValue" : "none") : (manualKnown ? "manual_capture" : "none");
}
// w-probe-r23 (R24-3 a): the hand-off gate refuses only when the refreshed
// selectionFacts do not match this bind (facts_mismatch). rowCapReached and
// visitCapReached are recorded on the row (factsRowCap/factsVisitCap) but
// never refuse: R24-2 measured that LogSelection publishes selectionFacts
// unconditionally before either cap is checked, so a cap stop is never a
// sign the facts are stale.
struct BindHandoffDecision { bool handoff{}; const char* reason{"none"}; bool factsRowCap{}; bool factsVisitCap{}; };
inline BindHandoffDecision DecideBindHandoff(bool factsCurrent, bool rowCapReached, bool visitCapReached) noexcept {
    if (!factsCurrent) return {false, "facts_mismatch", rowCapReached, visitCapReached};
    return {true, "none", rowCapReached, visitCapReached};
}
}

#pragma once
// Engine free W observation rules (w-probe-r1).
//
// Why this file exists: the W stage needs answers about the ItemSelect
// arguments, the funds question, the identity of the highlighted item and the
// phase a read ran in. Every one of those answers is a classification of values
// the probe already copied, so the rules live here, with no engine type in
// sight, and the offline harness drives exactly the code the runtime uses.
//
// What this file is NOT: none of these rules builds a sale request, names a
// callback to invoke or returns a permission. The eligibility rule below can
// only answer REJECTED or UNPROVEN, by construction (design section 1).
#include "receipt/Log.h"
#include <cmath>
#include <cstddef>
#include <cstdint>
namespace js {
// The total bound is what the receipt can represent, because the confirmation
// equation compares q*p against an int32 itemCount.
constexpr double MaxObservedTotal = 2147483647.0;

// The exact name the sale callback arrives under.
constexpr const char* ItemSelectName = "ItemSelect";

// The funds question (U-W1). Every input has to be known before the answer is
// allowed to be anything but unknown: an invalid sample, a missing quote or a
// balance the walk never read must not come out as "insufficient".
constexpr const char* FundsKnownSufficient = "FUNDS_KNOWN_SUFFICIENT";
constexpr const char* FundsKnownInsufficient = "FUNDS_KNOWN_INSUFFICIENT";
constexpr const char* FundsUnknown = "FUNDS_UNKNOWN";
inline const char* ClassifyFunds(bool quoteKnown, bool sampleValid, std::int32_t vendorGold,
                                 double total) noexcept {
    if (!quoteKnown || !sampleValid) return FundsUnknown;
    if (vendorGold < 0) return FundsUnknown;
    if (!std::isfinite(total) || total <= 0.0 || total > MaxObservedTotal) return FundsUnknown;
    return static_cast<double>(vendorGold) >= total ? FundsKnownSufficient : FundsKnownInsufficient;
}

// The shape of the highlighted stack, from the xList count that was already
// read. This is a shape and never an identity: two stacks of the same base can
// both be "extended" and still be different items.
inline const char* InstanceKindName(bool entryRead, std::uint32_t xListCount) noexcept {
    if (!entryRead) return "unknown";
    return xListCount ? "extended" : "unextended";
}
// How well the row identifies one item. A uniqueID read needs a phase this
// build cannot defend, so base_and_uid is a name the runtime never produces
// here; it exists so the parser and the harness agree on the vocabulary.
inline const char* IdentityQualityName(bool entryRead, std::uint32_t baseObj, bool uidRead) noexcept {
    if (!entryRead || !baseObj) return "unknown";
    return uidRead ? "base_and_uid" : "base_only";
}

// The vocabulary of the selection read. It lives here, with the rules, so the
// offline harness and the parser use the same words the runtime writes.
constexpr const char* SelectionSource = "itemlist_selected_item";
constexpr const char* SelectionIndexSource = "gfx_root_selectedIndex";
// The highlighted row moved between the two index reads, so the two halves of
// the row describe two different selections (design section 2.2).
constexpr const char* SelectionNotAtCall = "SELECTION_NOT_AT_CALL";
// A read this build refuses for want of a defensible phase (design section 2.2).
constexpr const char* ReadPhaseUnproven = "READ_PHASE_UNPROVEN";

// Tri-state external evidence (design section 4.2). The native side has no
// foreground or modal detection API, so it says unknown and names the source
// the parser has to supply instead.
constexpr const char* EvidenceTrue = "observed_true";
constexpr const char* EvidenceFalse = "observed_false";
constexpr const char* EvidenceUnknown = "unknown";
inline const char* EvidenceName(bool observed, bool value) noexcept {
    if (!observed) return EvidenceUnknown;
    return value ? EvidenceTrue : EvidenceFalse;
}

// Where a quantity came from. A quantity the probe did not see in a trace and
// that no manifest fixture pinned is unknown, never 1.
constexpr const char* QuantitySourceTrace = "trace";
constexpr const char* QuantitySourceFixture = "manifest_fixture";
constexpr const char* QuantitySourceUnknown = "unknown";

// Where a unit price came from. The only proven source is args[2] of a manual
// callback; there is no proven pre-dispatch quote path at all, which is why
// preDispatchQuoteSource is null by contract (design section 2.3).
constexpr const char* QuoteSourceManual = "manual_callback_args2";
constexpr const char* QuoteSourceNone = "none";
constexpr const char* ResponseLifetimeUnknown = "unknown";

// The prerequisites that are unproven for every observation this build can
// make. They are set on every row, so no row can look like a cleared gate.
constexpr std::uint32_t kAlwaysMissingPrerequisites =
    kPrereqResponseIdSource | kPrereqResponseLifetime | kPrereqPreDispatchQuote |
    kPrereqNullArgMeaning | kPrereqInstanceBinding | kPrereqForegroundGate |
    kPrereqModalDetection;
// The subset whose absence makes the answer a refusal rather than a
// non-answer. It is non-empty by construction above, so REJECTED is what this
// probe reports; the rule is written out anyway so the harness can show that
// a complete observation would still only reach UNPROVEN.
constexpr std::uint32_t kRequiredPrerequisites =
    kPrereqResponseIdSource | kPrereqResponseLifetime | kPrereqPreDispatchQuote |
    kPrereqNullArgMeaning | kPrereqInstanceBinding;
constexpr const char* DispatchRejected = "REJECTED";
constexpr const char* DispatchUnproven = "UNPROVEN";
// Returns REJECTED or UNPROVEN and nothing else. There is no input for which
// this function returns a permission (design section 1).
inline const char* DispatchEligibility(std::uint32_t missingPrerequisites) noexcept {
    return (missingPrerequisites & kRequiredPrerequisites) ? DispatchRejected : DispatchUnproven;
}

// The single outstanding observation rule (design section 8): a second request
// while one is armed is refused and counted, and an armed request is dropped
// when the generation it belonged to is gone.
constexpr unsigned MaxPendingObservations = 1;
inline bool ObservationArmable(unsigned pending) noexcept { return pending < MaxPendingObservations; }
inline bool ObservationStale(std::uint64_t armedLoadGeneration, std::uint64_t currentLoadGeneration,
                             std::uint64_t armedMenuGeneration,
                             std::uint64_t currentMenuGeneration) noexcept {
    return armedLoadGeneration != currentLoadGeneration || armedMenuGeneration != currentMenuGeneration;
}

// A session cap may suppress only its diagnostic row. The menu sample and
// functional body still run after the row cap is crossed.
struct SessionCapDecision {
    bool sampleSession{};
    bool logRow{};
};

inline SessionCapDecision SessionCapAction(std::uint64_t sessionNumber,
                                           std::uint64_t maxSessions) noexcept {
    return SessionCapDecision{true, sessionNumber < maxSessions};
}

// Cumulative visit accounting. Returns how many of the requested visits may be
// charged; zero means the cap is reached and the caller must stop reading
// instead of paying for more (design section 4.3).
inline std::uint64_t VisitAllowance(std::uint64_t used, std::uint64_t cap,
                                    std::uint64_t requested) noexcept {
    if (!cap) return requested;
    if (used >= cap) return 0;
    const auto left = cap - used;
    return requested < left ? requested : left;
}

// Engine-free rule for the U-W6 mixed-stack walk. Every candidate
// InventoryEntryData consumes one visit, even when it has no extra list. Every
// non-null ExtraDataList node consumes one more visit. mixedStacks counts one
// qualifying non-selected InventoryEntryData at most once, when it has at least
// one extra-list node and the base matches; multiple nodes on that entry are
// still one stack, and the selected entry is never counted. A cap stop leaves
// the count as a lower bound.
struct MixedStackScan {
    std::uint32_t selectedBase{};
    std::uintptr_t selectedEntry{};
    std::uint32_t maxVisits{};
    std::uint32_t visits{};
    std::uint32_t mixedStacks{};
    bool truncated{};
    bool otherMatchingEntry{};
    bool matchingStackCounted{};

    bool Charge() noexcept {
        if (visits >= maxVisits) {
            truncated = true;
            return false;
        }
        ++visits;
        return true;
    }

    bool VisitEntry(std::uint32_t base, std::uintptr_t entry) noexcept {
        if (!Charge()) return false;
        otherMatchingEntry = base == selectedBase && entry != selectedEntry;
        matchingStackCounted = false;
        return true;
    }

    bool VisitExtra(std::uintptr_t extra) noexcept {
        if (!extra) return true;
        if (!Charge()) return false;
        if (otherMatchingEntry && !matchingStackCounted) {
            ++mixedStacks;
            matchingStackCounted = true;
        }
        return true;
    }
};

// r7: the oracle's visit budget is spent per menu session and across the
// process using the configured VisitCap. R-F1 measured 328 visits per sample
// and six samples in a barter session that contains one manual sale; the old
// fixed 4,096 bound was too small for the large R-W2 inventory. The cap answers
// how much one barter session may read; reaching it is reported, never
// absorbed. Stage deadlines remain independent of this visit cap. Since r56
// it is no longer also a process-wide valve (OracleStageAllowance).
struct SessionVisitBudget {
    std::uint64_t session{};
    std::uint64_t used{};
};
// Opens the budget of one menu session. The reset happens exactly once per
// session, when the session number first differs, and it returns true only for
// that call so the caller can publish the reset instead of guessing at it.
inline bool BeginSessionVisits(SessionVisitBudget& budget, std::uint64_t session) noexcept {
    if (budget.session == session) return false;
    budget.session = session;
    budget.used = 0;
    return true;
}
// Charges visits to the budget that BeginSessionVisits already opened. It never
// resets, so a charge can not lose the visits a reset would have cleared, and
// the caller charges exactly one counter per read.
inline void ChargeSessionVisits(SessionVisitBudget& budget, std::uint64_t visits) noexcept {
    budget.used += visits;
}
// r56 (heavy-fix decision 2): the allowance of one oracle stage is what
// the current barter session has left of its cap, and nothing else. Up to r55
// Probe.cpp also bounded it by the process-lifetime oracle total, a counter
// nothing resets; on a ~1,700-entry inventory (about 1,700-2,000 visits per
// sample) that valve closed after a few dozen barter visits in one game
// process and every later press planned nothing until restart. The runaway
// guard stays: one session still cannot read past its cap.
inline std::uint64_t OracleStageAllowance(const SessionVisitBudget& budget, std::uint64_t cap) noexcept {
    return VisitAllowance(budget.used, cap, cap);
}

// w-probe-r16 (R16-2): the shape of one `js_gfx_list` row's population is
// engine free by construction. `total` is every ItemList entry the caller
// reports; entries above Row::MaxGfxListEntries are neither read nor counted
// individually, and `truncated` says so instead of under-reporting the list
// as if it had fewer entries. `fill(index, entry)` is the only place that may
// touch an engine type (a real rendered-GFx-object read in Probe.cpp, or
// a fake reader here in the offline harness); this function never does.
template <class Fill>
inline void PopulateGfxListEntries(Row& row, std::size_t total, Fill&& fill) {
    // R21-4: the four GFx arrays live on the heap payload now; MutableGfx()
    // allocates it on first touch so row.gfx is never null once this runs.
    auto& payload = row.MutableGfx();
    payload.gfxListEntryCountTotal = static_cast<unsigned>(total);
    payload.gfxListTruncated = total > Row::MaxGfxListEntries;
    const std::size_t limit = total < Row::MaxGfxListEntries ? total : Row::MaxGfxListEntries;
    for (std::size_t i = 0; i < limit; ++i) {
        auto& dst = payload.gfxListEntries[payload.gfxListEntryCount++];
        dst.index = static_cast<std::int32_t>(i);
        fill(i, dst);
    }
}
}

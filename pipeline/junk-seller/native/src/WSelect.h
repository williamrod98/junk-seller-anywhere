#pragma once
// Engine-free W-5 rule-run candidate building on top of the accepted T-W06
// Selector.h core (the D-W06 decision rules are unchanged; w-probe-r31 moved
// that file's run limit from the input index to the count of selected rows,
// R31-1). Design ruling section 25a and the first item of a later review
// fix apply here.
//
// Identity here follows the same honesty the accepted read (Selection.h)
// already uses: base_only is the normal case, and the individual flag
// (ExtraUniqueID presence) is informational, read fresh on every call and
// never retained across a UI task as a pointer or an index. A candidate is
// excluded with a reason when any required field cannot be read; a list
// read that fails entirely is the caller's job to turn into
// ADAPTER_UNAVAILABLE (no row here dispatches on its own).
#include "Selector.h"
// The four request-gate terms are shared with the bind path (RequestGateReason
// in WIssue.h), so the scheduler and the bind gate can never drift apart.
#include "WIssue.h"
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace js {

// w31 fix round 2 (a review A-003): the number of
// candidates one run may plan and issue is the run snapshot's own `MaxItems`
// (R29-1's transaction bound), not a compile-time stage constant -- the old
// MaxRuleRunCandidates=3 silently cut every accepted value above 3 back to
// three issuances, and the values 4..40 could never control the cap the
// settings contract promises. The bound is SelectorMaxTransactions, which is
// also the Settings.h ceiling, so a plan can never exceed the selector's own
// transaction unit. Pure and host-testable: Probe.cpp cannot be host-compiled.
inline std::size_t RulePlanCap(std::size_t maxItems) noexcept {
    if (maxItems < 1) return 1;
    return maxItems > SelectorMaxTransactions ? SelectorMaxTransactions : maxItems;
}

// w-probe-r31 (R31-1): after the selector fix the cap is spent inside
// SelectCandidates, so the rows a run's `MaxItems` cut are the selector's own
// `kRemainder` / `kRunLimit` results -- in every position, including behind
// ineligible rows. The run row's `capReached` is exactly that fact: at least
// one result the cap cut. Pure, so the host test and Probe.cpp cannot drift.
inline bool RuleRunLimitReached(const SelectionRun& selection) noexcept {
    for (std::size_t i = 0; i < selection.resultCount && i < selection.results.size(); ++i) {
        if (selection.results[i].decision == SelectorDecision::kRemainder &&
            selection.results[i].term == SelectorTerm::kRunLimit) {
            return true;
        }
    }
    return false;
}

// w-probe-r33 (R38-1): how many candidates passed every exclusion except the
// run limit -- the selector's own `kSelect` rows plus the `kRemainder` /
// `kRunLimit` rows its cap cut (a row the cap cut passed the type, stolen,
// absolute, owner and threshold rules first, so it was selectable). This is
// what R-W24d's verdict template read as "selectable"; `candidatesEligible`
// counts the rows that survived the read/type filter only and therefore reads
// the same number for a run whose cap cut nothing and one whose cap cut
// everything. Never a position in the displayed list and never the balance
// outcome (an insufficient-funds or adapter-unavailable row is decided past
// the cap, so it is not counted here). Pure, so the host test cannot drift
// from the counter the row carries. R40-1 makes the matching receipt invariant
// equality for COMPLETED runs and an upper bound for every other run reason:
// a plan entry leaves an ISSUE row, a pre-dispatch re-verification / bind
// refusal row, or -- when the run was cancelled before it reached that entry
// -- no row at all. The receipt rows therefore lower-bound this plan-time
// count on non-COMPLETED runs (with candidatesEligible as its upper bound).
inline std::size_t RuleCandidatesSelectable(const SelectionRun& selection) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < selection.resultCount && i < selection.results.size(); ++i) {
        const auto& result = selection.results[i];
        if (result.decision == SelectorDecision::kSelect ||
            (result.decision == SelectorDecision::kRemainder &&
             result.term == SelectorTerm::kRunLimit)) {
            ++count;
        }
    }
    return count;
}

// A rule read retains only indices and integer addresses between UI tasks.
// The caller validates generations and list identity before each chunk.
constexpr std::size_t MaxRuleReadChunks = 64;
constexpr std::uint64_t RuleReadGapNs = 16000000;
constexpr std::uint64_t RuleReadBudgetNs = 1000000;
constexpr std::size_t RuleReadDeadlineStride = 8;

// heavy-fix (w-probe-r56): the extra-list visit budget of ONE rule run
// (its paged read and the pre-dispatch re-reads of its rows), opened fresh by
// every run. Up to r55 the per-entry individual read charged the
// process-lifetime xListVisits counter against VisitCap. That counter is
// never reset and the 500 ms BarterMenu selection poll (LogSelection ->
// ReadSelected's mixed-stack scan) charges one visit per player inventory
// entry, so a ~1,700-row inventory spent the whole 262,144 cap after about
// 76 s of BarterMenu time in one game process (a 150-row one after about
// 14 min); from then on every read was denied and every row became
// identity_unresolved (L1b r55: 0 rows sold). The cap value stays the one
// configured cap (Settings.h VisitCap, design ruling R8-2; the host test asserts
// they are equal); only its scope changes from the process to the run.
inline constexpr std::uint64_t RuleRunXListVisitCap = 262144;

struct RuleXListBudget {
    std::uint64_t cap{RuleRunXListVisitCap};
    std::uint64_t used{};
    // One visit per entry whose extra-data lists are walked. False (the read
    // is denied, never defaulted to "no individual") once the run has spent
    // its cap.
    bool Take() noexcept {
        if (used >= cap) return false;
        ++used;
        return true;
    }
};

enum class RulePageResult { kContinue, kComplete, kInvalidated, kError };

struct RulePageWindow {
    bool menuOpen{};
    std::uint64_t menuGeneration{}, loadGeneration{}, saveGeneration{}, configGeneration{};
    std::uintptr_t listAddress{};
    std::size_t entries{};
    std::uintptr_t anchorAddress{};
};

struct RulePageState {
    std::size_t cursor{}, chunks{};
    std::uintptr_t anchorAddress{};
    std::uint64_t notBeforeNs{};
    RulePageWindow window{};
};

inline RulePageResult BeginRulePage(RulePageState& page, const RulePageWindow& current) noexcept {
    const auto& expected = page.window;
    if (!current.menuOpen || current.menuGeneration != expected.menuGeneration ||
        current.loadGeneration != expected.loadGeneration ||
        current.saveGeneration != expected.saveGeneration ||
        current.configGeneration != expected.configGeneration ||
        current.listAddress != expected.listAddress || current.entries != expected.entries ||
        (page.cursor && current.anchorAddress != page.anchorAddress))
        return RulePageResult::kInvalidated;
    if (page.cursor == expected.entries) return RulePageResult::kComplete;
    if (page.chunks >= MaxRuleReadChunks) return RulePageResult::kError;
    ++page.chunks;
    return RulePageResult::kContinue;
}

inline RulePageResult EndRulePage(RulePageState& page, std::size_t nextCursor,
                                 std::uintptr_t anchorAddress, std::uint64_t nowNs) noexcept {
    page.cursor = nextCursor;
    page.anchorAddress = anchorAddress;
    if (page.cursor == page.window.entries) return RulePageResult::kComplete;
    page.notBeforeNs = nowNs + RuleReadGapNs;
    return RulePageResult::kContinue;
}

enum class CandidateReadFailure : unsigned {
    kNone,
    kNotDisplayed,
    kNoQuote,
    kIdentityUnresolved,
    // w-probe-r24 fix round 2 (A-02): a candidate whose name/form-list/
    // mixed-base protection facts were not positively read. Counted with
    // excludedIdentityUnresolved on the run row (no new schema field for
    // r24), but kept as its own failure code so the per-candidate reason
    // stays precise.
    kFactsUnreadable,
};

inline const char* CandidateReadFailureName(CandidateReadFailure value) noexcept {
    switch (value) {
    case CandidateReadFailure::kNone: return "none";
    case CandidateReadFailure::kNotDisplayed: return "not_displayed";
    case CandidateReadFailure::kNoQuote: return "no_quote";
    case CandidateReadFailure::kIdentityUnresolved: return "identity_unresolved";
    case CandidateReadFailure::kFactsUnreadable: return "facts_unreadable";
    }
    return "unknown";
}

// One js_gfx_list-shaped entry, exactly the read set D-W06 r2 (13.1) named:
// filteredIndex non-null selects the displayed side, infoValue is the unit
// sell price, text is display-only (never identity). candidate/merchant are
// pre-filled by the adapter with the protection and balance facts the
// unchanged Selector.h core decides on.
struct GfxListCandidateInput {
    bool baseObjKnown{};
    std::uint32_t baseObj{};
    bool filteredIndexKnown{};
    std::int32_t filteredIndex{-1};
    bool infoValueKnown{};
    double infoValue{};
    // Informational only (never required for a build to succeed): whether
    // this read found a per-stack ExtraUniqueID and, if so, its value.
    bool hasIndividual{};
    std::uint32_t individualId{};
    // w-probe-r24 fix round 2 (A-03): true only when the individual read
    // (the ExtraUniqueID walk) positively completed -- either there was no
    // extra-data list at all, or there was one and it was fully read under
    // its visit budget. False means the read was denied (budget exhausted),
    // which is not the same as positively finding no individual, and must
    // exclude rather than default to base-only identity.
    bool individualReadOk{};
    // w-probe-r24 fix round 2 (A-02); the source was replaced by w-probe-r25
    // (R27-2 a): true only when nameProtected and formListProtected were
    // positively read for this candidate. The adapter sets this from the
    // probe-wide state ResolveProtectedForms decided at load -- both required
    // INI keys parsed and every ProtectedForms key resolved -- so while it is
    // false every candidate is still excluded fail-closed (kFactsUnreadable)
    // rather than defaulting the facts themselves to "safe".
    // sameBaseMixedProtection is not part of this flag: it stays false by
    // design (R27-2 c) and is still unresolved.
    bool protectionFactsKnown{};
    CandidateRecord candidate{};
    MerchantRecord merchant{};
};

struct RuleCandidateBuildResult {
    CandidateReadFailure failure{CandidateReadFailure::kNone};
    CandidateMerchantRecord record{};
};

inline RuleCandidateBuildResult BuildRuleCandidate(const GfxListCandidateInput& input) noexcept {
    RuleCandidateBuildResult result;
    if (!input.filteredIndexKnown) {
        result.failure = CandidateReadFailure::kNotDisplayed;
        return result;
    }
    if (!input.infoValueKnown || !std::isfinite(input.infoValue) || !(input.infoValue > 0.0) ||
        std::floor(input.infoValue) != input.infoValue) {
        result.failure = CandidateReadFailure::kNoQuote;
        return result;
    }
    if (!input.baseObjKnown || input.baseObj == 0 || !input.individualReadOk) {
        result.failure = CandidateReadFailure::kIdentityUnresolved;
        return result;
    }
    if (!input.protectionFactsKnown) {
        result.failure = CandidateReadFailure::kFactsUnreadable;
        return result;
    }
    result.record.candidate = input.candidate;
    result.record.candidate.baseFormKey = input.baseObj;
    result.record.merchant = input.merchant;
    return result;
}

struct RuleSelectionCandidate {
    std::size_t sourceIndex{};  // index into the original js_gfx_list read
    CandidateMerchantRecord record{};
    bool hasIndividual{};
    std::uint32_t individualId{};
    double infoValue{};
};

struct RuleSelectionRun {
    SelectionRun selection{};
    std::vector<RuleSelectionCandidate> candidates{};  // built (Selector-eligible) inputs, source order
    std::size_t candidatesRead{};
    std::size_t candidatesEligible{};
    // w-probe-r33 (R38-1): rows that passed every exclusion except the run
    // limit (RuleCandidatesSelectable above).
    std::size_t candidatesSelectable{};
    std::size_t excludedNotDisplayed{};
    std::size_t excludedNoQuote{};
    std::size_t excludedIdentityUnresolved{};
    std::size_t excludedFactsUnreadable{};
};

inline RuleSelectionRun SelectRuleCandidates(std::span<const GfxListCandidateInput> entries,
                                             SelectorContract contract,
                                             const SelectorThresholds& thresholds) {
    RuleSelectionRun run;
    run.candidatesRead = entries.size();
    std::vector<CandidateMerchantRecord> eligible;
    eligible.reserve(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto built = BuildRuleCandidate(entries[i]);
        if (built.failure != CandidateReadFailure::kNone) {
            if (built.failure == CandidateReadFailure::kNotDisplayed) ++run.excludedNotDisplayed;
            else if (built.failure == CandidateReadFailure::kNoQuote) ++run.excludedNoQuote;
            else if (built.failure == CandidateReadFailure::kIdentityUnresolved) {
                ++run.excludedIdentityUnresolved;
            } else {
                ++run.excludedFactsUnreadable;
            }
            continue;
        }
        RuleSelectionCandidate candidate;
        candidate.sourceIndex = i;
        candidate.record = built.record;
        candidate.hasIndividual = entries[i].hasIndividual;
        candidate.individualId = entries[i].individualId;
        candidate.infoValue = entries[i].infoValue;
        run.candidates.push_back(candidate);
        eligible.push_back(built.record);
    }
    run.candidatesEligible = run.candidates.size();
    run.selection = SelectCandidates(eligible, contract, thresholds);
    // w-probe-r33 (R38-1): decided from the selector's own results, after the
    // cap has been spent, so it states the rows this run could have sold.
    run.candidatesSelectable = RuleCandidatesSelectable(run.selection);
    return run;
}

// w-probe-r28 (R29-3): the default-snapshot spelling every accepted call site
// already uses, so the T-W06 behaviour is byte-for-byte what it was.
inline RuleSelectionRun SelectRuleCandidates(std::span<const GfxListCandidateInput> entries,
                                             SelectorContract contract = DefaultSelectorContract) {
    return SelectRuleCandidates(entries, contract, DefaultSelectorThresholds);
}

// Selector.h (accepted T-W06 core, unchanged) has no name function for
// SelectorTerm; this stage's js_rule_candidate EXCLUDE rows need one for
// their reason field, so it lives here instead of touching that file.
inline const char* SelectorTermName(SelectorTerm value) noexcept {
    switch (value) {
    case SelectorTerm::kNone: return "none";
    case SelectorTerm::kType: return "type";
    case SelectorTerm::kPrice: return "price";
    case SelectorTerm::kWeight: return "weight";
    case SelectorTerm::kRatio: return "ratio";
    case SelectorTerm::kContract: return "contract";
    case SelectorTerm::kQuest: return "quest";
    case SelectorTerm::kEnchanted: return "enchanted";
    case SelectorTerm::kFavorite: return "favorite";
    case SelectorTerm::kEquipped: return "equipped";
    case SelectorTerm::kOwnerUnknown: return "owner_unknown";
    case SelectorTerm::kNameProtected: return "name_protected";
    case SelectorTerm::kFormListProtected: return "formlist_protected";
    case SelectorTerm::kStolen: return "stolen";
    case SelectorTerm::kMixedProtection: return "mixed_protection";
    case SelectorTerm::kCandidateInvalid: return "candidate_invalid";
    case SelectorTerm::kBalance: return "balance";
    case SelectorTerm::kQuote: return "quote";
    case SelectorTerm::kZeroGold: return "zero_gold";
    case SelectorTerm::kRunLimit: return "run_limit";
    // w-probe-r26 (R27d-3): the adapter-unavailable balance case reads as
    // balance_unknown, never as "balance".
    case SelectorTerm::kBalanceUnknown: return "balance_unknown";
    case SelectorTerm::kCategoryOff: return "category_off";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// w-probe-r26 (R27d-2): where a rule run's merchant balance comes from. A
// rule run plans its candidates before any bind target exists, so the
// cachedBeforeSample the bind path uses -- filled in Probe.cpp only by a
// menu_open / observation sample with a non-zero targetBase and a known
// measured pair -- is usually empty here (measured in R-W21). The rule run
// therefore takes the merchant container gold, in order, from
// cachedBeforeSample when that sample belongs to this menu session, then
// from the last valid sample of this menu/load generation, then from a
// complete VENC slot in that window, else leaves the balance unknown. The
// generation discipline is the funds question's
// (Selector.h's caller, Probe.cpp BuildObservation: valid + same menu +
// same load) WITHOUT the inventory generation, because a container event
// after the sample does not make the merchant's gold unknown for a
// pre-selection estimate; the run row records that delta instead. The issue
// path's own issue_before sample stays the authoritative funds check before
// dispatch.
// ---------------------------------------------------------------------------
struct RuleBalanceCandidate {
    bool valid{};
    // Never negative on a usable candidate: -1 is the "the walk did not read
    // a balance" sentinel (Probe.cpp lastSample), not minus one gold.
    std::int32_t gold{-1};
    std::uint64_t startMenuGeneration{}, endMenuGeneration{};
    std::uint64_t startLoadGeneration{}, endLoadGeneration{};
    std::uint64_t endInventoryGeneration{};
};

enum class RuleBalanceOrigin : unsigned { kNone, kCachedBefore, kLastSample, kVencSlot };

inline const char* RuleBalanceOriginName(RuleBalanceOrigin value) noexcept {
    switch (value) {
    case RuleBalanceOrigin::kCachedBefore: return "cached_before";
    case RuleBalanceOrigin::kLastSample: return "last_sample";
    case RuleBalanceOrigin::kVencSlot: return "venc_slot";
    case RuleBalanceOrigin::kNone: return "none";
    }
    return "none";
}

struct RuleBalanceSourceResult {
    bool known{};
    std::int64_t value{-1};
    RuleBalanceOrigin source{RuleBalanceOrigin::kNone};
    // inventoryGeneration now minus the chosen sample's own inventory
    // generation, or -1 when no sample was chosen.
    std::int64_t inventoryDelta{-1};
};

// A candidate fits this menu session only when it was read inside it (both
// edges of its menu and load window are the current generations) and it
// positively read a balance.
inline bool RuleBalanceUsableForMenu(const RuleBalanceCandidate& candidate,
                                    std::uint64_t expectedMenuGeneration,
                                    std::uint64_t expectedLoadGeneration) noexcept {
    return candidate.valid && candidate.gold >= 0 &&
           candidate.startMenuGeneration == expectedMenuGeneration &&
           candidate.endMenuGeneration == expectedMenuGeneration &&
           candidate.startLoadGeneration == expectedLoadGeneration &&
           candidate.endLoadGeneration == expectedLoadGeneration;
}

// A slot can supply funds only when its own walk completed with a resolved
// merchant and container. Other slots do not affect this verdict.
inline bool RuleVencSlotComplete(bool ran, bool truncated, bool unsampled,
                                bool visitCapReached, bool merchantResolved,
                                bool containerResolved) noexcept {
    return ran && !truncated && !unsampled && !visitCapReached &&
           merchantResolved && containerResolved;
}

inline bool RuleVencSlotInventoryStable(std::uint64_t startInventoryGeneration,
                                       std::uint64_t endInventoryGeneration) noexcept {
    return startInventoryGeneration == endInventoryGeneration;
}

struct RuleVencMerchantWindow {
    std::uint64_t startMenuGeneration{}, endMenuGeneration{};
};

inline bool RuleVencSlotMerchantWindow(const RuleVencMerchantWindow& merchant,
                                      std::uint64_t slotStartMenuGeneration,
                                      std::uint64_t slotEndMenuGeneration) noexcept {
    return merchant.startMenuGeneration == slotStartMenuGeneration &&
           merchant.endMenuGeneration == slotStartMenuGeneration &&
           slotStartMenuGeneration == slotEndMenuGeneration;
}

// Keep the accepted full-sample priority, then use a complete VENC slot in
// this menu/load window. The issue path never consumes this choice.
inline RuleBalanceSourceResult RuleBalanceSource(const RuleBalanceCandidate& cachedBefore,
                                                const RuleBalanceCandidate& lastSample,
                                                const RuleBalanceCandidate& vencSlot,
                                                std::uint64_t expectedMenuGeneration,
                                                std::uint64_t expectedLoadGeneration,
                                                std::uint64_t currentInventoryGeneration) noexcept {
    RuleBalanceSourceResult result;
    if (RuleBalanceUsableForMenu(cachedBefore, expectedMenuGeneration, expectedLoadGeneration)) {
        result.known = true;
        result.value = cachedBefore.gold;
        result.source = RuleBalanceOrigin::kCachedBefore;
        result.inventoryDelta = static_cast<std::int64_t>(currentInventoryGeneration) -
                                static_cast<std::int64_t>(cachedBefore.endInventoryGeneration);
    } else if (RuleBalanceUsableForMenu(lastSample, expectedMenuGeneration, expectedLoadGeneration)) {
        result.known = true;
        result.value = lastSample.gold;
        result.source = RuleBalanceOrigin::kLastSample;
        result.inventoryDelta = static_cast<std::int64_t>(currentInventoryGeneration) -
                                static_cast<std::int64_t>(lastSample.endInventoryGeneration);
    } else if (RuleBalanceUsableForMenu(vencSlot, expectedMenuGeneration, expectedLoadGeneration)) {
        result.known = true;
        result.value = vencSlot.gold;
        result.source = RuleBalanceOrigin::kVencSlot;
        result.inventoryDelta = static_cast<std::int64_t>(currentInventoryGeneration) -
                                static_cast<std::int64_t>(vencSlot.endInventoryGeneration);
    }
    return result;
}

// Pre-dispatch re-verification (review fix item 2): a fresh read of the same
// source entry must still show the same base and quote before a queued
// candidate is issued. When the original
// read found an individual (ExtraUniqueID), the fresh read must find the
// same one; a candidate with no individual the first time is re-verified on
// base + quote alone, the same guarantee the accepted BindKey path already
// gives (Selection.h: base_only is the normal case). An index drift is
// accepted only after this identity check and is resolved by the caller.
// w-probe-r24 fix round 5 (A-02): the re-verification now names why it
// refused, because an identity-presence transition needs its own reason on
// the js_rule_candidate row. The filtered index is only a position hint:
// once identity is unique, a changed index is accepted and the fresh index
// is used for binding.
enum class ReverifyFailure : unsigned {
    kNone,
    kReadFailed,
    kIndexChanged,
    kBaseChanged,
    kQuoteChanged,
    kIdentityChanged,
};

inline const char* ReverifyFailureName(ReverifyFailure value) noexcept {
    switch (value) {
    case ReverifyFailure::kNone: return "none";
    case ReverifyFailure::kReadFailed: return "reverify_failed";
    case ReverifyFailure::kIndexChanged: return "index_changed";
    case ReverifyFailure::kBaseChanged: return "base_changed";
    case ReverifyFailure::kQuoteChanged: return "quote_changed";
    case ReverifyFailure::kIdentityChanged: return "identity_changed";
    }
    return "reverify_failed";
}

inline ReverifyFailure RuleCandidateReverifyFailure(const GfxListCandidateInput& original,
                                                    const GfxListCandidateInput& fresh) noexcept {
    if (!fresh.filteredIndexKnown || !fresh.baseObjKnown || !fresh.infoValueKnown) {
        return ReverifyFailure::kReadFailed;
    }
    // A-03 (w-probe-r24 fix round 2): the fresh read must itself positively
    // resolve identity and quote, not merely still agree with a stale value.
    // A-02 (fix round 5): the original read must have resolved it too --
    // an original whose individual walk was denied is not a base-only
    // candidate, it is an unknown one, and can never be re-verified.
    if (!fresh.individualReadOk || !original.individualReadOk) return ReverifyFailure::kReadFailed;
    if (!(fresh.infoValue > 0.0)) return ReverifyFailure::kQuoteChanged;
    if (fresh.baseObj != original.baseObj) return ReverifyFailure::kBaseChanged;
    if (fresh.infoValue != original.infoValue) return ReverifyFailure::kQuoteChanged;
    // A-02 (w-probe-r24 fix round 5): individual-presence parity in both
    // directions. A base with several stacks can resolve an individual the
    // planning read did not see (or lose the one it did), and that fresh
    // individual may belong to a different stack than the rule selected, so
    // any transition fails closed instead of being treated as the same
    // base-only candidate.
    if (original.hasIndividual != fresh.hasIndividual) return ReverifyFailure::kIdentityChanged;
    if (original.hasIndividual && fresh.individualId != original.individualId) {
        return ReverifyFailure::kIdentityChanged;
    }
    return ReverifyFailure::kNone;
}

struct RuleCandidateReverifyResolution {
    std::int32_t matchedIndex{-1};
    bool ambiguous{};
};

// D-W06 identity matching: a unique identity match wins regardless of its
// filteredIndex. Several identity matches are narrowed by the planned index;
// no unique survivor is an ambiguity, never a guessed identity.
inline RuleCandidateReverifyResolution ResolveRuleCandidateMatch(
    std::span<const GfxListCandidateInput> freshCandidates,
    const GfxListCandidateInput& original) noexcept {
    RuleCandidateReverifyResolution result;
    std::int32_t firstMatch = -1;
    std::size_t matchCount = 0;
    for (std::size_t i = 0; i < freshCandidates.size(); ++i) {
        if (RuleCandidateReverifyFailure(original, freshCandidates[i]) != ReverifyFailure::kNone) {
            continue;
        }
        if (firstMatch < 0) firstMatch = static_cast<std::int32_t>(i);
        ++matchCount;
    }
    if (matchCount == 1) {
        result.matchedIndex = firstMatch;
        return result;
    }
    if (matchCount == 0) return result;

    std::int32_t plannedMatch = -1;
    for (std::size_t i = 0; i < freshCandidates.size(); ++i) {
        if (RuleCandidateReverifyFailure(original, freshCandidates[i]) != ReverifyFailure::kNone ||
            freshCandidates[i].filteredIndex != original.filteredIndex) {
            continue;
        }
        if (plannedMatch >= 0) {
            result.ambiguous = true;
            return result;
        }
        plannedMatch = static_cast<std::int32_t>(i);
    }
    if (plannedMatch >= 0) {
        result.matchedIndex = plannedMatch;
    } else {
        result.ambiguous = true;
    }
    return result;
}

inline bool RuleCandidateReverified(const GfxListCandidateInput& original,
                                    const GfxListCandidateInput& fresh) noexcept {
    return RuleCandidateReverifyFailure(original, fresh) == ReverifyFailure::kNone;
}

// ---------------------------------------------------------------------------
// w-probe-r24 fix round 5 (S-01): the engine-free half of the rule path's
// binding step. R20-3 / R21-2 proved the method live on the BindKey path: the
// items[] entry is located by base FormID (its AS `formId` member must agree
// with the ItemList entry's own base) and a non-null `filteredIndex` equal to
// the fresh one, `selectedIndex` is written as a means only, and the write
// is believed only after GetSelectedItem() reads back the same Item* and the
// same objDesc. Nothing here is an identity: the entry that was re-verified
// is the identity, and this only finds where in items[] it sits.
// ---------------------------------------------------------------------------
struct RuleBindEntry {
    bool formIdKnown{};
    std::uint32_t formId{};
    bool filteredIndexKnown{};
    std::int32_t filteredIndex{-1};
    // True only for the entry the fresh re-verification accepted (same
    // Item*), so a second stack of the same base can never be bound instead.
    bool reverifiedCandidate{};
};

// The items[] index to write, or -1 when no single entry qualifies (missing,
// or ambiguous -- both fail closed as bind_entry_missing).
inline std::int32_t FindRuleBindIndex(std::span<const RuleBindEntry> entries,
                                      std::uint32_t base, std::int32_t filteredIndex) noexcept {
    std::int32_t found = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (!entry.reverifiedCandidate) continue;
        if (!entry.formIdKnown || entry.formId != base || base == 0) continue;
        if (!entry.filteredIndexKnown || entry.filteredIndex != filteredIndex) continue;
        if (found >= 0) return -1;
        found = static_cast<std::int32_t>(i);
    }
    return found;
}

enum class RuleBindOutcome : unsigned { kBound, kEntryMissing, kReadbackMismatch };

inline const char* RuleBindOutcomeReason(RuleBindOutcome value) noexcept {
    switch (value) {
    case RuleBindOutcome::kBound: return "none";
    case RuleBindOutcome::kEntryMissing: return "bind_entry_missing";
    case RuleBindOutcome::kReadbackMismatch: return "bind_readback_mismatch";
    }
    return "bind_readback_mismatch";
}

// entryIndex < 0 means the scan above found nothing to write. Otherwise the
// write happened and only the read-back decides: the same Item*, the same
// objDesc pointer and the same base, or the bind is refused.
inline RuleBindOutcome DecideRuleBind(std::int32_t entryIndex, bool readbackSameItem,
                                      bool readbackSameObjDesc, std::uint32_t readbackBase,
                                      std::uint32_t plannedBase) noexcept {
    if (entryIndex < 0) return RuleBindOutcome::kEntryMissing;
    if (!readbackSameItem || !readbackSameObjDesc) return RuleBindOutcome::kReadbackMismatch;
    if (plannedBase == 0 || readbackBase != plannedBase) return RuleBindOutcome::kReadbackMismatch;
    return RuleBindOutcome::kBound;
}

// w-probe-r24 design hand review (w27 A-01): a rule run belongs to the BarterMenu
// session it was planned in. Once that menu closed, another menu generation
// opened or a load changed the generation, the plan describes a list that
// no longer exists; advancing it would read another merchant's list and
// dispatch a sale nobody planned. Pure so the host test can pin it; the
// caller cancels the run with reason run_invalidated when this is true.
inline bool RuleRunInvalidated(bool barterOpen, std::uint64_t runMenuGeneration,
                               std::uint64_t currentMenuGeneration,
                               std::uint64_t runLoadGeneration,
                               std::uint64_t currentLoadGeneration) noexcept {
    return !barterOpen || runMenuGeneration != currentMenuGeneration ||
           runLoadGeneration != currentLoadGeneration;
}

// ---------------------------------------------------------------------------
// w-probe-r27 (R28-1): the D-W07 foreground gate (design section 5, state
// table row IDLE). A new run is only allowed when the front of the UI really
// is this merchant's sell list: the BarterMenu open, no *other* menu named as
// the front-most one, and none of the modal menus (QuantityMenu /
// MessageBoxMenu / Console) open. The BarterMenu is a *paused* menu -- every
// R-W20 phase row records paused=1 -- so gamePaused is recorded evidence and
// deliberately not a term here. The plain-bool struct keeps this decision
// engine-free: Probe.cpp reads the live UI singleton (UI.h:83-102) and fills
// it, and the host test covers every reason without a CommonLib translation
// unit.
//
// w-probe-r27 fix round 2: the topmost term is positive
// evidence only, never a required identity. R-W5 measured GetTopMostMenu
// returning null while the BarterMenu was the only and topmost menu, so
// requiring `top == BarterMenu` refused every ordinary run. A read that named
// nothing is not evidence of another menu and no longer refuses; the term
// fires only when the read named a *different* front menu. Across the rw2..
// rw22 live receipts every row recorded negativeTopmost=false, so the term
// never fired in a measured session.
// ---------------------------------------------------------------------------
struct SchedulerPhase {
    bool barterOpen{};
    // True only when GetTopMostMenu positively named a menu other than the
    // BarterMenu. The measured R-W5 case (nothing named while the BarterMenu
    // was open) leaves it false and does not refuse on this term.
    bool topmostOther{};
    bool modalOpen{};
    bool quantityOpen{};
    bool messageOpen{};
    bool consoleOpen{};
    bool issueInFlight{};
    bool issueLatched{};
    bool requestPending{};
};

// The one reason a run is refused with, or "OK". Five foreground terms in
// the order the gate reads them (the menu itself first), then the four
// request-gate terms through RequestGateReason, whose own order
// (BARTER_CLOSED / ISSUE_IN_FLIGHT / ISSUE_LATCHED / REQUEST_PENDING) is
// kept. One reason per cause: the caller writes the returned value verbatim
// on the js_rule_run row.
inline const char* SchedulerGateReason(const SchedulerPhase& phase) noexcept {
    if (!phase.barterOpen) return "BARTER_CLOSED";
    if (phase.topmostOther) return "TOPMOST_OTHER";
    if (phase.modalOpen) return "MODAL_OPEN";
    if (phase.quantityOpen) return "QUANTITY_OPEN";
    if (phase.messageOpen) return "MESSAGE_OPEN";
    if (phase.consoleOpen) return "CONSOLE_OPEN";
    if (const auto* request = RequestGateReason(phase.barterOpen, phase.issueInFlight,
                                                phase.issueLatched, phase.requestPending)) {
        return request;
    }
    return "OK";
}

// "OK" as a boolean without pulling <cstring> into Probe.cpp.
inline bool SchedulerGatePassed(const char* reason) noexcept {
    return reason != nullptr && reason[0] == 'O' && reason[1] == 'K' && reason[2] == '\0';
}

// The topmost question as a predicate of the two pointers the live read
// produced (UI.h:102): true only when the read positively named a front menu
// that is not the BarterMenu. A null read named nothing -- exactly what R-W5
// measured while the BarterMenu was open -- and is not evidence of another
// menu, so it never refuses on this term.
inline bool SchedulerTopmostOther(const void* topmost, const void* barterMenu) noexcept {
    return topmost != nullptr && topmost != barterMenu;
}

// w-probe-r27 fix round 2: the pending<=1 claim as one atomic
// state the input sink and the UI task exchange. The sink may only claim an
// idle slot (compare_exchange, so two sink threads can never both claim), the
// UI task publishes a claim as active once the run really starts, and every
// end path -- including a refused or failed start -- returns the slot to
// idle. The sink therefore never reads the UI-task-owned run struct.
enum class SchedulerSlot : unsigned { kIdle = 0, kQueued = 1, kActive = 2 };

// w-probe-r34 (D-W08): the stop receipt counts a queued or active rule run,
// an outstanding issue transaction, and an already queued rule-run advance.
// The active scheduler slot and its issue transaction are one unit until a
// cancellation releases the slot; the transaction then remains one unit until
// its normal finalizer clears it.
inline unsigned RuleStopPendingCount(SchedulerSlot slot, bool advanceQueued,
                                     bool issueInFlight) noexcept {
    return static_cast<unsigned>(slot != SchedulerSlot::kIdle || issueInFlight) +
           static_cast<unsigned>(advanceQueued);
}

inline bool RuleStopIdle(unsigned pendingAfter, std::uint64_t idleUs) noexcept {
    return pendingAfter == 0 && idleUs <= 1000000;
}

inline bool RuleStopLatched(bool idle) noexcept {
    return !idle;
}

// w37 loop2 (S-001): every saved Enabled true -> false transition owns one
// request until the UI confirmation task writes its row. This fixed-capacity
// FIFO is deliberately engine-free so the host harness can prove its order
// and full-queue behavior. The native owner serializes calls with its mutex.
struct SettingsStopRequest {
    std::uint64_t applyNs{};
    std::uint64_t configGeneration{};
    unsigned pendingBefore{};
    unsigned cancelled{};
};

inline constexpr std::size_t SettingsStopQueueCapacity = 8;

class SettingsStopQueue {
public:
    bool Push(const SettingsStopRequest& request) noexcept {
        if (size_ == SettingsStopQueueCapacity) return false;
        requests_[(head_ + size_) % SettingsStopQueueCapacity] = request;
        ++size_;
        return true;
    }

    bool Pop(SettingsStopRequest& request) noexcept {
        if (size_ == 0) return false;
        request = requests_[head_];
        head_ = (head_ + 1) % SettingsStopQueueCapacity;
        --size_;
        return true;
    }

    void AddCancelled() noexcept {
        for (std::size_t index = 0; index < size_; ++index) {
            ++requests_[(head_ + index) % SettingsStopQueueCapacity].cancelled;
        }
    }

    bool Empty() const noexcept { return size_ == 0; }
    std::size_t Size() const noexcept { return size_; }

private:
    std::array<SettingsStopRequest, SettingsStopQueueCapacity> requests_{};
    std::size_t head_{};
    std::size_t size_{};
};

// The sink's claim question: only an idle slot may be claimed. A queued or
// active slot drops the edge instead of queueing a second run.
inline bool SchedulerSlotClaimable(SchedulerSlot slot) noexcept {
    return slot == SchedulerSlot::kIdle;
}

// The queued-run evidence the js_barter_open row records: a claim still
// queued (never published active) belongs to the previous menu generation and
// refuses at its own entry.
inline bool SchedulerSlotQueued(SchedulerSlot slot) noexcept {
    return slot == SchedulerSlot::kQueued;
}

// w-probe-r27 fix round 2: the save start is a generation event
// exactly like the menu generation and the load epoch. A run queued or
// planned before the save start (a different save generation) must not read
// the list or dispatch; nothing survives it into the save. A run planned
// after it is a new decision in the new save generation, and the probe has no
// save-completion notification, so that is where the invalidation ends.
inline bool SaveStartedSince(std::uint64_t queuedSaveGeneration,
                             std::uint64_t currentSaveGeneration) noexcept {
    return queuedSaveGeneration != currentSaveGeneration;
}

// w-probe-r27 (R28-1): the queued advance task's own entry question. A run
// cancelled before the queued task ran (menu close / load change / save
// start) has already folded its pending terminal and finished, so
// ruleRun.active is false: the task must fold nothing and read no list. A run
// still active folds the posted terminal exactly once. Pure so the host test
// can pin both directions.
inline bool RuleAdvanceFolds(bool runActive) noexcept { return runActive; }

}  // namespace js

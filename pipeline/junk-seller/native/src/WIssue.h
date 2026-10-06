#pragma once
// Engine-free W-2 issuance, protection, quote, and oracle rules.
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <mutex>
// w-probe-r45: the stolen-goods acceptance gate the dispatch
// re-check below consults.
#include "StolenRule.h"

namespace js {

enum class SaleTerminal : unsigned {
    kIssued,
    kCancelledNotStarted,
    kProtected,
    kAdapterUnavailable,
    kQuoteInvalid,
    kQuantityInvalid,
    kInsufficientFunds,
    kBudgetExceeded,
    kStaleTarget,
    kReceiptUnavailable,
    kConfirmed,
    kAttemptTimeout,
    kCancelledNoEventTimeout,
    kUnknownMutation,
    kOracleMismatch,
    kMenuClosed,
    kLoadChanged,
    // W-3: a recorded key-edge request the oracle task could not honour.
    kRequestExpired,
    kRequestMismatch,
    // W-4: the fresh issue_before sample was truncated and no safe cache exists.
    kBeforeUnavailable,
};

inline const char* SaleTerminalName(SaleTerminal value) noexcept {
    switch (value) {
    case SaleTerminal::kIssued: return "ISSUED";
    case SaleTerminal::kCancelledNotStarted: return "CANCELLED_NOT_STARTED";
    case SaleTerminal::kProtected: return "PROTECTED";
    case SaleTerminal::kAdapterUnavailable: return "ADAPTER_UNAVAILABLE";
    case SaleTerminal::kQuoteInvalid: return "QUOTE_INVALID";
    case SaleTerminal::kQuantityInvalid: return "QUANTITY_INVALID";
    case SaleTerminal::kInsufficientFunds: return "INSUFFICIENT_FUNDS";
    case SaleTerminal::kBudgetExceeded: return "BUDGET_EXCEEDED";
    case SaleTerminal::kStaleTarget: return "STALE_TARGET";
    case SaleTerminal::kReceiptUnavailable: return "RECEIPT_UNAVAILABLE";
    case SaleTerminal::kConfirmed: return "CONFIRMED";
    case SaleTerminal::kAttemptTimeout: return "ATTEMPT_TIMEOUT";
    case SaleTerminal::kCancelledNoEventTimeout: return "CANCELLED_NO_EVENT_TIMEOUT";
    case SaleTerminal::kUnknownMutation: return "UNKNOWN_MUTATION";
    case SaleTerminal::kOracleMismatch: return "ORACLE_MISMATCH";
    case SaleTerminal::kMenuClosed: return "MENU_CLOSED";
    case SaleTerminal::kLoadChanged: return "LOAD_CHANGED";
    case SaleTerminal::kRequestExpired: return "REQUEST_EXPIRED";
    case SaleTerminal::kRequestMismatch: return "REQUEST_MISMATCH";
    case SaleTerminal::kBeforeUnavailable: return "BEFORE_UNAVAILABLE";
    }
    return "UNKNOWN";
}

inline bool IsTerminal(SaleTerminal value) noexcept { return value != SaleTerminal::kIssued; }

inline const char* RequestGateReason(bool barterOpen, bool issueInFlight,
                                     bool issueLatched, bool requestPending) noexcept {
    if (!barterOpen) return "BARTER_CLOSED";
    if (issueInFlight) return "ISSUE_IN_FLIGHT";
    if (issueLatched) return "ISSUE_LATCHED";
    if (requestPending) return "REQUEST_PENDING";
    return nullptr;
}

// w-probe-r45: the three kOtherAccepted* values are a foreign owner
// the run's merchant accepts as stolen goods (StolenRule.h); the name states
// which engine input accepted it, on the attempt row's protectionRule field.
enum class OwnershipRule : unsigned {
    kNone, kPlayer, kOther, kUnreadable, kStolen,
    kOtherAcceptedVendor, kOtherAcceptedPerk, kOtherAcceptedBoth,
};
inline const char* OwnershipRuleName(OwnershipRule value) noexcept {
    switch (value) {
    case OwnershipRule::kNone: return "owner_none_stolen_unknown_allowed";
    case OwnershipRule::kPlayer: return "owner_player_stolen_unknown_allowed";
    case OwnershipRule::kOther: return "owner_other_protected";
    case OwnershipRule::kUnreadable: return "owner_unreadable_protected";
    case OwnershipRule::kStolen: return "stolen_protected";
    case OwnershipRule::kOtherAcceptedVendor: return "owner_other_accepted_vendor";
    case OwnershipRule::kOtherAcceptedPerk: return "owner_other_accepted_perk";
    case OwnershipRule::kOtherAcceptedBoth: return "owner_other_accepted_both";
    }
    return "owner_unreadable_protected";
}

struct ProtectionDecision {
    bool protectedItem{};
    bool ownershipSafe{};
    OwnershipRule rule{OwnershipRule::kUnreadable};
};

inline ProtectionDecision DecideProtection(std::uint32_t ownerForm,
                                           std::uint32_t playerForm,
                                           bool ownerReadable,
                                           bool stolenKnown,
                                           bool stolen,
                                           bool questObject,
                                           bool favorite,
                                           bool worn) noexcept {
    ProtectionDecision decision;
    if (questObject || favorite || worn) {
        decision.protectedItem = true;
        decision.rule = OwnershipRule::kOther;
        return decision;
    }
    if (!ownerReadable) {
        decision.protectedItem = true;
        decision.rule = OwnershipRule::kUnreadable;
        return decision;
    }
    if (stolenKnown && stolen) {
        decision.protectedItem = true;
        decision.rule = OwnershipRule::kStolen;
        return decision;
    }
    if (!ownerForm) {
        decision.ownershipSafe = true;
        decision.rule = OwnershipRule::kNone;
        return decision;
    }
    if (playerForm && ownerForm == playerForm) {
        decision.ownershipSafe = true;
        decision.rule = OwnershipRule::kPlayer;
        return decision;
    }
    // Unknown stolen status is safe only for no owner or player ownership.
    (void)stolenKnown;
    decision.protectedItem = true;
    decision.rule = OwnershipRule::kOther;
    return decision;
}

// w-probe-r45: the rule-run dispatch re-check. With
// StolenAcceptance::kRefused it returns exactly the eight-argument decision
// above (the r42 behaviour). Otherwise a foreign-owned item (kOther, or a
// known theft verdict kStolen) becomes ownership-safe, but only when the owner
// was read and the item is not a quest object, a favourite or worn -- those
// three stay protected before any ownership test, as in r42.
inline ProtectionDecision DecideProtection(std::uint32_t ownerForm,
                                           std::uint32_t playerForm,
                                           bool ownerReadable,
                                           bool stolenKnown,
                                           bool stolen,
                                           bool questObject,
                                           bool favorite,
                                           bool worn,
                                           StolenAcceptance acceptance) noexcept {
    ProtectionDecision decision = DecideProtection(ownerForm, playerForm, ownerReadable, stolenKnown,
                                                   stolen, questObject, favorite, worn);
    if (!StolenAccepted(acceptance) || questObject || favorite || worn || !ownerReadable) {
        return decision;
    }
    if (decision.rule != OwnershipRule::kOther && decision.rule != OwnershipRule::kStolen) {
        return decision;
    }
    if (decision.rule == OwnershipRule::kOther && !ForeignOwner(ownerForm, playerForm)) {
        return decision;
    }
    decision.protectedItem = false;
    decision.ownershipSafe = true;
    decision.rule = acceptance == StolenAcceptance::kVendorBuysStolen ? OwnershipRule::kOtherAcceptedVendor
                    : acceptance == StolenAcceptance::kBypassPerk     ? OwnershipRule::kOtherAcceptedPerk
                                                                      : OwnershipRule::kOtherAcceptedBoth;
    return decision;
}

struct QuoteEvidence {
    bool valid{};
    bool manualOrigin{};
    bool ownDispatch{};
    std::uint32_t baseObj{};
    std::uint64_t menuGeneration{}, loadGeneration{};
    double responseValue{}, unitPrice{};
    std::int32_t quantity{-1};
};

inline bool QuoteEvidenceAccepted(const QuoteEvidence& evidence,
                                  std::uint32_t selectedBase,
                                  std::uint64_t expectedMenuGeneration,
                                  std::uint64_t expectedLoadGeneration) noexcept {
    return evidence.valid && evidence.manualOrigin && !evidence.ownDispatch &&
           evidence.baseObj != 0 && evidence.baseObj == selectedBase &&
           evidence.menuGeneration == expectedMenuGeneration &&
           evidence.loadGeneration == expectedLoadGeneration;
}

struct BalanceSnapshot {
    bool valid{};
    std::uint64_t sampleId{}, menuGeneration{}, loadGeneration{}, inventoryGeneration{};
    std::int32_t playerGold{-1}, merchantActorGold{-1}, merchantContainerGold{-1}, merchantInventoryGold{-1};
};

inline bool SnapshotCurrent(const BalanceSnapshot& snapshot,
                            std::uint64_t expectedMenuGeneration,
                            std::uint64_t expectedLoadGeneration,
                            std::uint64_t expectedInventoryGeneration) noexcept {
    return snapshot.valid && snapshot.sampleId != 0 &&
           snapshot.menuGeneration == expectedMenuGeneration &&
           snapshot.loadGeneration == expectedLoadGeneration &&
           snapshot.inventoryGeneration == expectedInventoryGeneration &&
           snapshot.playerGold >= 0 && snapshot.merchantActorGold >= 0 &&
           snapshot.merchantContainerGold >= 0 && snapshot.merchantInventoryGold >= 0;
}

inline bool BalanceDeltasMatch(const std::array<std::int32_t, 4>& deltas,
                               const std::array<std::int32_t, 2>& inventoryDeltas,
                               std::int32_t quantity,
                               std::int32_t unitPrice) noexcept {
    if (quantity < 1 || unitPrice < 1) return false;
    const auto total = static_cast<std::int64_t>(quantity) * unitPrice;
    return total <= 2147483647LL && deltas[0] == total && deltas[1] == 0 &&
           deltas[2] == -total && deltas[3] == -total &&
           inventoryDeltas[0] == -quantity && inventoryDeltas[1] == quantity;
}

// ---------------------------------------------------------------------------
// W-3 (design ruling 1): the key edge only records a request. Issuance happens in
// the oracle task right after that task's own four-balance snapshot, and only
// when the request still describes that snapshot. Anything else is dropped,
// never dispatched; target-conflicting oracle work waits for its own sample.
// ---------------------------------------------------------------------------
constexpr std::uint32_t RequestGoldFormID = 0x0000000Fu;  // Skyrim.esm gold, never a sale target

struct SaleRequest {
    bool valid{};
    bool fromBind{};
    bool bindInfoValueKnown{};
    bool latchCleared{};
    double bindInfoValue{};
    std::uint32_t baseFormID{};
    std::int32_t quantity{};
    std::uint64_t menuGeneration{}, loadGeneration{}, tickMs{}, token{}, sampleId{};
    // w-probe-r24 (W-5): non-zero when this request was dispatched by the
    // rule-selected sequencer rather than the BindKey/manual path; carried
    // onto the attempt so the receipt can join it back to its js_rule_run.
    std::uint64_t runToken{};
    // w-probe-r24 fix round 2 (A-04): the rule path's own re-verified
    // protection facts for this exact candidate, read immediately before
    // dispatch (DispatchNextRuleCandidate) and carried on the request
    // itself. IssueFromSnapshot must use these for a rule-issued request
    // instead of the unrelated global last-selection snapshot, which
    // describes whatever the player is currently hovering, not the planned
    // candidate. Unused (false/0) for a bind/manual request.
    bool ruleFactsKnown{};
    std::uint32_t ruleOwnerForm{};
    bool ruleOwnerReadable{};
    bool ruleQuestObject{}, ruleFavorite{}, ruleWorn{};
    // w-probe-r45: the rule run's own stolen-goods acceptance
    // (RuleRunState::stolenAcceptance), decided once at planning. A bind or
    // manual request keeps kRefused, so its dispatch decision is r42's.
    StolenAcceptance ruleStolenAcceptance{StolenAcceptance::kRefused};
};

// What the oracle task knows when it has just published its snapshot: the base
// the sample counted, the base the menu has selected now, and the generations
// the sample was built in.
struct RequestSnapshot {
    std::uint32_t targetBase{}, selectedBase{};
    std::uint64_t menuGeneration{}, loadGeneration{};
};

inline std::uint64_t RequestAgeMs(const SaleRequest& request, std::uint64_t nowMs) noexcept {
    return nowMs >= request.tickMs ? nowMs - request.tickMs : 0;
}

// kIssued means "the request may go on to the sale validation"; the two
// REQUEST_* terminals are fail-closed drops. A clock that runs backwards is
// treated as expired rather than as age zero.
inline SaleTerminal DecideRequest(const SaleRequest& request, const RequestSnapshot& snapshot,
                                  std::uint64_t nowMs, std::uint64_t ttlMs) noexcept {
    if (!request.valid) return SaleTerminal::kRequestMismatch;
    if (nowMs < request.tickMs || nowMs - request.tickMs >= ttlMs) return SaleTerminal::kRequestExpired;
    if (!request.baseFormID || request.baseFormID == RequestGoldFormID ||
        request.baseFormID != snapshot.targetBase || request.baseFormID != snapshot.selectedBase) {
        return SaleTerminal::kRequestMismatch;
    }
    if (request.menuGeneration != snapshot.menuGeneration ||
        request.loadGeneration != snapshot.loadGeneration) {
        return SaleTerminal::kRequestMismatch;
    }
    if (request.quantity < 1) return SaleTerminal::kRequestMismatch;
    return SaleTerminal::kIssued;
}

// ---------------------------------------------------------------------------
// W-3 (design rulings 2 and 3): what the oracle measured, not what the request
// asked for. -1 means "not measured" and can never satisfy an equality.
// ---------------------------------------------------------------------------
struct MeasuredPair {
    std::int32_t playerBase{-1}, vendorBase{-1}, playerGold{-1}, vendorGold{-1};
};

inline bool MeasuredPairKnown(const MeasuredPair& pair) noexcept {
    return pair.playerBase >= 0 && pair.vendorBase >= 0 && pair.playerGold >= 0 && pair.vendorGold >= 0;
}

// Inventory deltas derived from the two measured pairs: {player, vendor}.
inline std::array<std::int32_t, 2> MeasuredInventoryDeltas(const MeasuredPair& before,
                                                           const MeasuredPair& after) noexcept {
    return {after.playerBase - before.playerBase, after.vendorBase - before.vendorBase};
}

// The ruling 2 (b) equalities: player(base) - q, vendor(base) + q, player gold
// + price * q, and the vendor gold - price * q that D-W04 already requires.
inline bool MeasuredPairsMatch(const MeasuredPair& before, const MeasuredPair& after,
                               std::int32_t quantity, std::int32_t unitPrice) noexcept {
    if (!MeasuredPairKnown(before) || !MeasuredPairKnown(after)) return false;
    if (quantity < 1 || unitPrice < 1) return false;
    const auto total = static_cast<std::int64_t>(quantity) * unitPrice;
    if (total > 2147483647LL) return false;
    return static_cast<std::int64_t>(after.playerBase) == static_cast<std::int64_t>(before.playerBase) - quantity &&
           static_cast<std::int64_t>(after.vendorBase) == static_cast<std::int64_t>(before.vendorBase) + quantity &&
           static_cast<std::int64_t>(after.playerGold) == static_cast<std::int64_t>(before.playerGold) + total &&
           static_cast<std::int64_t>(after.vendorGold) == static_cast<std::int64_t>(before.vendorGold) - total;
}

// Ruling 2 (a): the container events recorded between dispatch and settle are
// exactly the expected pair. observed counts every container event the sink
// saw in that window (before any row cap), handled counts the ones the attempt
// examined; a gap between the two is an event nobody checked.
inline bool EventSetExact(bool itemEvent, bool goldEvent, bool foreignEvent,
                          std::uint64_t observed, std::uint64_t handled) noexcept {
    return itemEvent && goldEvent && !foreignEvent && observed == 2 && handled == 2;
}

// ---------------------------------------------------------------------------
// w-probe-r12 (design rulings R12-1 and R12-2). Two engine-free rules, both taken
// from the R-W7 evidence:
//   R12-2  A settle request must resolve to a sample that will publish. In r11
//          the single queued oracle slot was overwritten by the next deferred
//          request, so the id the settle had already been handed (8) was never
//          started and the attempt waited for a sample nobody would build.
//   R12-1  The attempt deadline is measured from the settle request, not from
//          the dispatch, and it is long enough for the six-stage sample the
//          paused BarterMenu takes about 1010 ms to finish.
// ---------------------------------------------------------------------------

// What a sample is being taken for. The rank decides what a coalesced sample
// measures: a settle needs the attempt's target base, a quiet sample does not,
// so the higher rank wins when two requests share one sample.
enum class OracleRequestKind : unsigned { kQuiet = 0, kIssueBefore = 1, kIssueSettle = 2 };

inline unsigned OracleRequestRank(OracleRequestKind kind) noexcept {
    return static_cast<unsigned>(kind);
}

// The next queued oracle request: the sample that will start as soon as the
// running job publishes. Additional target-conflicting requests wait in order.
struct OracleQueueSlot {
    bool valid{};
    std::uint64_t sampleId{};
    OracleRequestKind kind{OracleRequestKind::kQuiet};
    std::uint32_t targetBase{};
    const char* origin{};
};

// sampleId is the id the caller must use; it is always an id that will be
// published, either by the job started here or by a waiting one. coalescedInto
// is that queued id when this request was folded into an already scheduled
// sample, and 0 when the request got a sample of its own.
struct OracleRequestDecision {
    std::uint64_t sampleId{};
    std::uint64_t coalescedInto{};
    bool started{};
    bool deferred{};
    bool upgraded{};
    bool refused{};
    bool waiting{};
    OracleQueueSlot slot{};
};

// nextId is the id to hand out when a fresh sample is needed; it is consumed
// only when the decision says so (started or a newly queued slot), so a
// coalesced request burns no id. A request is never allowed to overwrite the
// queued slot: the displaced sample would be one somebody is already waiting
// for, which is exactly the r11 defect.
inline OracleRequestDecision DecideOracleRequest(bool busy, const OracleQueueSlot& queued,
                                                 OracleRequestKind kind,
                                                 std::uint64_t nextId,
                                                 std::uint32_t targetBase = 0) noexcept {
    OracleRequestDecision decision;
    if (!busy) {
        decision.sampleId = nextId;
        decision.started = true;
        decision.slot = OracleQueueSlot{};
        return decision;
    }
    decision.deferred = true;
    if (!queued.valid || !queued.sampleId) {
        decision.sampleId = nextId;
        decision.slot = OracleQueueSlot{true, nextId, kind, targetBase};
        return decision;
    }
    if (queued.targetBase != targetBase) {
        decision.refused = true;
        return decision;
    }
    decision.sampleId = queued.sampleId;
    decision.coalescedInto = queued.sampleId;
    decision.slot = queued;
    if (OracleRequestRank(kind) > OracleRequestRank(queued.kind)) {
        decision.slot.kind = kind;
        decision.upgraded = true;
    }
    return decision;
}

// The queue state is one transition, not several independently observed
// atomics. A requester either starts a sample, owns the next queued slot,
// shares a same-target slot, or waits in order behind a foreign target.
class OracleRequestQueue {
public:
    OracleRequestDecision Request(const char* origin, OracleRequestKind kind,
                                  std::uint32_t targetBase = 0) {
        std::lock_guard lock(_mutex);
        auto decision = DecideOracleRequest(_busy, _queued, kind, _nextSampleId + 1, targetBase);
        if (decision.started) {
            _busy = true;
            ++_nextSampleId;
            return decision;
        }
        if (decision.refused) {
            for (auto& waiting : _waiting) {
                if (waiting.targetBase != targetBase) continue;
                decision.sampleId = waiting.sampleId;
                decision.coalescedInto = waiting.sampleId;
                decision.refused = false;
                decision.slot = waiting;
                if (OracleRequestRank(kind) > OracleRequestRank(waiting.kind)) {
                    waiting.kind = kind;
                    waiting.origin = origin;
                    decision.slot = waiting;
                    decision.upgraded = true;
                }
                return decision;
            }
            ++_nextSampleId;
            const OracleQueueSlot waiting{true, _nextSampleId, kind, targetBase, origin};
            _waiting.push_back(waiting);
            decision.sampleId = waiting.sampleId;
            decision.slot = waiting;
            decision.refused = false;
            decision.waiting = true;
            return decision;
        }
        if (!decision.coalescedInto) {
            ++_nextSampleId;
            decision.slot.origin = origin;
        } else if (decision.upgraded) {
            decision.slot.origin = origin;
        }
        _queued = decision.slot;
        return decision;
    }

    OracleQueueSlot Complete() {
        std::lock_guard lock(_mutex);
        _busy = false;
        OracleQueueSlot next;
        if (_queued.valid) {
            next = _queued;
            _queued = {};
        } else if (!_waiting.empty()) {
            next = _waiting.front();
            _waiting.pop_front();
        }
        if (next.valid) _busy = true;
        return next;
    }

    bool Abort() {
        std::lock_guard lock(_mutex);
        const bool wasBusy = _busy;
        _busy = false;
        _queued = {};
        _waiting.clear();
        return wasBusy;
    }

    bool Busy() const {
        std::lock_guard lock(_mutex);
        return _busy;
    }

private:
    mutable std::mutex _mutex;
    bool _busy{};
    std::uint64_t _nextSampleId{};
    OracleQueueSlot _queued{};
    std::deque<OracleQueueSlot> _waiting{};
};

// What happened to the sample an attempt's settle asked for.
enum class SettleSampleState : unsigned { kNone, kPending, kPublished, kLost };

inline const char* SettleSampleStateName(SettleSampleState value) noexcept {
    switch (value) {
    case SettleSampleState::kNone: return "none";
    case SettleSampleState::kPending: return "pending";
    case SettleSampleState::kPublished: return "published";
    case SettleSampleState::kLost: return "lost";
    }
    return "none";
}

// A settle sample that has not arrived although a later sample already
// published is lost, not merely slow: sample ids publish in the order they are
// started. This is the r11 failure stated as a reading instead of a silence.
inline SettleSampleState ClassifySettleSample(bool requested, bool pending,
                                              std::uint64_t settleSampleId,
                                              std::uint64_t lastPublishedSampleId) noexcept {
    if (!requested || !settleSampleId) return SettleSampleState::kNone;
    if (!pending) return SettleSampleState::kPublished;
    return lastPublishedSampleId > settleSampleId ? SettleSampleState::kLost
                                                  : SettleSampleState::kPending;
}

// A rule run may dispatch its next candidate only after the previous settle
// sample has published or has been classified lost. Pending is the only state
// that keeps the sequencer at its current candidate.
inline bool RuleAdvanceMayDispatch(SettleSampleState state) noexcept {
    return state != SettleSampleState::kPending;
}

// R12-1. The deadline runs from the settle request once one exists, because
// that is the moment the measurement the attempt waits for was asked for; until
// then it runs from the dispatch. A clock that runs backwards never expires an
// attempt early.
constexpr std::uint64_t AttemptDeadlineMs = 5000;

inline std::uint64_t AttemptDeadlineBaseMs(std::uint64_t startMs,
                                           std::uint64_t settleRequestMs) noexcept {
    return settleRequestMs ? settleRequestMs : startMs;
}

inline std::uint64_t AttemptElapsedMs(std::uint64_t sinceMs, std::uint64_t nowMs) noexcept {
    return nowMs >= sinceMs ? nowMs - sinceMs : 0;
}

inline bool AttemptDeadlineReached(std::uint64_t startMs, std::uint64_t settleRequestMs,
                                   std::uint64_t nowMs, std::uint64_t deadlineMs) noexcept {
    const auto base = AttemptDeadlineBaseMs(startMs, settleRequestMs);
    return nowMs >= base && nowMs - base >= deadlineMs;
}

// w-probe-r24 fix round 3 (A-03): whether a container event's own capture
// stamp landed after the attempt's deadline. attemptStartMs is the attempt's
// dispatch time (not the settle-adjusted base: A-03 measures from the
// dispatch, matching the token stamped at dispatch in Probe.cpp), so an
// event exactly at AttemptDeadlineMs is still accepted and one past it is
// late. AttemptElapsedMs already treats a clock running backwards as zero
// elapsed, so a backwards capture stamp is never late.
inline bool EventCaptureLate(std::uint64_t attemptStartMs, std::uint64_t captureMs,
                             std::uint64_t deadlineMs) noexcept {
    return AttemptElapsedMs(attemptStartMs, captureMs) > deadlineMs;
}

struct SaleInput {
    bool foreground{}, inFlight{}, latched{}, protectedItem{}, ownershipSafe{};
    bool delegateAvailable{true}, uiMovieAvailable{true};
    bool quoteSameBase{}, quoteGenerationCurrent{}, fromBind{};
    bool priceKnown{}, budgetExceeded{}, staleTarget{}, receiptAvailable{true};
    double price{};
    std::int32_t quantity{}, count{}, merchantGold{};
};

inline void ApplyBindQuote(SaleInput& input, bool fromBind, bool valueKnown,
                           double value) noexcept {
    input.quoteSameBase = fromBind && valueKnown;
    input.quoteGenerationCurrent = fromBind;
    input.priceKnown = fromBind && valueKnown;
    input.price = fromBind ? value : 0.0;
    input.fromBind = fromBind;
}

struct SaleValidation {
    SaleTerminal terminal{SaleTerminal::kIssued};
    const char* refusalTerm{"none"};
};

inline SaleValidation ValidateSaleDetailed(const SaleInput& input) noexcept {
    if (!input.foreground || input.inFlight || input.latched) {
        return {SaleTerminal::kCancelledNotStarted, "none"};
    }
    if (input.budgetExceeded) return {SaleTerminal::kBudgetExceeded, "none"};
    if (input.staleTarget) return {SaleTerminal::kStaleTarget, "none"};
    if (!input.receiptAvailable) return {SaleTerminal::kReceiptUnavailable, "none"};
    if (!input.delegateAvailable) return {SaleTerminal::kAdapterUnavailable, "delegate"};
    if (!input.uiMovieAvailable) return {SaleTerminal::kAdapterUnavailable, "uiMovie"};
    if (input.protectedItem || !input.ownershipSafe) return {SaleTerminal::kProtected, "none"};
    if (!input.quoteSameBase) return {SaleTerminal::kQuoteInvalid, "quoteSameBase"};
    if (!input.quoteGenerationCurrent) {
        return {SaleTerminal::kQuoteInvalid, "quoteGenerationCurrent"};
    }
    if (!input.priceKnown) return {SaleTerminal::kQuoteInvalid, "priceKnown"};
    if (!std::isfinite(input.price)) return {SaleTerminal::kQuoteInvalid, "priceFinite"};
    if (input.price <= 0) return {SaleTerminal::kQuoteInvalid, "pricePositive"};
    if (std::floor(input.price) != input.price || input.price > 2147483647.0) {
        return {SaleTerminal::kQuoteInvalid, "priceIntegral"};
    }
    if (input.quantity < 1) return {SaleTerminal::kQuantityInvalid, "quantity"};
    if (input.quantity > input.count) return {SaleTerminal::kQuantityInvalid, "count"};
    const auto total = static_cast<std::int64_t>(input.quantity) *
                       static_cast<std::int64_t>(input.price);
    return {input.merchantGold < total ? SaleTerminal::kInsufficientFunds : SaleTerminal::kIssued,
            "none"};
}

inline const char* SaleRefusalTerm(const SaleInput& input) noexcept {
    return ValidateSaleDetailed(input).refusalTerm;
}

inline SaleTerminal ValidateSale(const SaleInput& input) noexcept {
    return ValidateSaleDetailed(input).terminal;
}

struct AttemptState {
    bool inFlight{}, errorLatched{}, itemEvent{}, goldEvent{}, duplicateEvent{}, foreignItemSelect{};
    bool terminal{};
    // W-3: a container event that is not the expected item or gold move.
    bool foreignEvent{};
    SaleTerminal terminalReason{SaleTerminal::kIssued};
    std::uint64_t generation{}, token{};
    std::int32_t quantity{}, unitPrice{};
    // w-probe-r24 fix round 3 (A-03): a same-shaped event whose own capture
    // stamp landed after AttemptDeadlineMs. It never sets itemEvent/
    // goldEvent (so it can never contribute to CONFIRMED) and is counted
    // here for the receipt row.
    std::uint64_t lateEvents{};
    // w-probe-r24 fix round 5 (A-01): container events this attempt lost
    // before they could be examined -- dropped by the process-wide row cap,
    // refused by a full event ring, or accepted by the sink and never
    // handled. A lost event is an unexamined mutation, so a pair that looks
    // exact can never confirm while this is non-zero.
    std::uint64_t eventsLost{};
};

// w-probe-r24 fix round 5 (A-01): how many events one attempt lost, from the
// three independent symptoms the runtime can measure. sinkLost (the attempt's
// own drop sites) and eventRingDropsSinceDispatch (the event ring's own
// refusal counter) describe the same losses from two sides, so the count is
// the larger of the two, never their sum. An observed/handled gap is a
// separate symptom: an event the sink did accept that the attempt never
// examined. design hand review (w27 A-02): the callback trace ring's drops are
// health evidence only and never reach this count; a full call trace during
// an exact item+gold pair is not a lost container event.
inline std::uint64_t AttemptEventsLost(std::uint64_t sinkLost,
                                       std::uint64_t eventRingDropsSinceDispatch,
                                       std::uint64_t observed, std::uint64_t handled) noexcept {
    std::uint64_t lost = sinkLost > eventRingDropsSinceDispatch ? sinkLost : eventRingDropsSinceDispatch;
    if (observed > handled) lost += observed - handled;
    return lost;
}

// R25-3 (D-W04 amended): only an exact pair from this attempt can confirm.
// The caller snapshots and drains the event queue at the deadline, then passes
// this exact-set result. Settle samples remain diagnostic-only.
inline SaleTerminal ClassifyAttempt(const AttemptState& state,
                                    bool timeout,
                                    bool matches) noexcept {
    if (!timeout) return SaleTerminal::kIssued;
    const bool exact = state.itemEvent && state.goldEvent &&
        !state.foreignItemSelect && !state.duplicateEvent && !state.foreignEvent &&
        state.lateEvents == 0 && state.eventsLost == 0;
    return matches && exact ? SaleTerminal::kConfirmed : SaleTerminal::kAttemptTimeout;
}

inline const char* AttemptTimeoutDetail(const AttemptState& state) noexcept {
    // w-probe-r24 fix round 5 (A-01): a lost event is named first. It says
    // the attempt's own record of the window is incomplete, which is a
    // stronger statement than what the events that did arrive looked like.
    if (state.eventsLost != 0) return "events_lost";
    if (state.lateEvents != 0) return "late_event";
    if (state.foreignItemSelect || state.duplicateEvent || state.foreignEvent) return "extra_event";
    return "missing_event";
}

// Latching is a post-dispatch rule. A pre-dispatch validation can use the
// same terminal token but must not consume the menu generation.
inline bool TerminalLatches(SaleTerminal value, bool dispatched) noexcept {
    return dispatched && (value == SaleTerminal::kStaleTarget ||
           value == SaleTerminal::kCancelledNoEventTimeout ||
           value == SaleTerminal::kUnknownMutation || value == SaleTerminal::kOracleMismatch);
}

// Terminals decided before the one dispatch; a row carrying one of them must
// say dispatchCount 0.
inline bool TerminalPreDispatch(SaleTerminal value) noexcept {
    switch (value) {
    case SaleTerminal::kCancelledNotStarted:
    case SaleTerminal::kProtected:
    case SaleTerminal::kAdapterUnavailable:
    case SaleTerminal::kQuoteInvalid:
    case SaleTerminal::kQuantityInvalid:
    case SaleTerminal::kInsufficientFunds:
    case SaleTerminal::kBudgetExceeded:
    case SaleTerminal::kStaleTarget:
    case SaleTerminal::kReceiptUnavailable:
    case SaleTerminal::kRequestExpired:
    case SaleTerminal::kRequestMismatch:
    case SaleTerminal::kBeforeUnavailable:
        return true;
    default:
        return false;
    }
}

struct AttemptMachine {
    AttemptState state{};

    SaleTerminal Start(const SaleInput& input, std::uint64_t generation, std::uint64_t token) noexcept {
        SaleInput checked = input;
        checked.inFlight = checked.inFlight || state.inFlight;
        checked.latched = checked.latched || state.errorLatched;
        const auto result = ValidateSale(checked);
        if (result != SaleTerminal::kIssued) return result;
        state = {};
        state.inFlight = true;
        state.generation = generation;
        state.token = token;
        state.quantity = input.quantity;
        state.unitPrice = static_cast<std::int32_t>(input.price);
        return result;
    }

    SaleTerminal Event(bool item, bool gold, bool identityMatches, std::uint64_t generation) noexcept {
        if (!state.inFlight || state.terminal || generation != state.generation ||
            !identityMatches || item == gold) {
            state.foreignItemSelect = true;
            return SaleTerminal::kUnknownMutation;
        }
        bool& seen = item ? state.itemEvent : state.goldEvent;
        if (seen) state.duplicateEvent = true;
        seen = true;
        return state.duplicateEvent ? SaleTerminal::kUnknownMutation : SaleTerminal::kIssued;
    }

    void ForeignItemSelect() noexcept { state.foreignItemSelect = true; }

    // W-3 (ruling 2): a foreign container event ends the attempt as
    // ORACLE_MISMATCH, which latches.
    SaleTerminal ForeignEvent() noexcept {
        state.foreignEvent = true;
        return SaleTerminal::kOracleMismatch;
    }

    // w-probe-r24 fix round 3 (A-03): a same-shaped event captured after the
    // deadline is counted separately from an identity-foreign one (the
    // receipt row can tell "arrived late" from "never ours"), but it ends
    // the attempt the same way -- otherwise treated like a foreign event --
    // so it can never set a pair flag or contribute to CONFIRMED.
    SaleTerminal LateEvent() noexcept {
        ++state.lateEvents;
        return ForeignEvent();
    }

    // w-probe-r24 fix round 5 (A-01): the caller measures the loss at the
    // deadline (sink drop counters and the observed/handled gap) and states
    // it here before Finish, so the sole terminal gate sees it.
    void NoteEventsLost(std::uint64_t count) noexcept { state.eventsLost = count; }

    SaleTerminal Finish(bool timeout, bool exactEventSet) noexcept {
        if (state.terminal) return state.terminalReason;
        const auto result = ClassifyAttempt(state, timeout, exactEventSet);
        if (result == SaleTerminal::kIssued) return result;
        return Abort(result);
    }

    // R25-3: true as soon as this attempt's own item and gold events have
    // both landed (Event() already checked identity and generation), so the
    // caller can confirm immediately instead of waiting on the settle
    // sample or the deadline. Never true once the attempt is already
    // terminal, foreign or duplicated.
    bool EventPairComplete() const noexcept {
        // w-probe-r24 fix round 5 (A-01): eventsLost is part of the pair
        // being complete. An attempt that lost an event has no complete
        // record of its own window, whatever the two flags say.
        return state.inFlight && !state.terminal && state.itemEvent && state.goldEvent &&
               !state.foreignItemSelect && !state.duplicateEvent && !state.foreignEvent &&
               state.eventsLost == 0;
    }

    SaleTerminal Abort(SaleTerminal result) noexcept {
        state.inFlight = false;
        state.terminal = true;
        state.terminalReason = result;
        state.errorLatched = state.errorLatched || TerminalLatches(result, true);
        return result;
    }

    SaleTerminal Close(bool load) noexcept {
        state.inFlight = false;
        state.terminal = true;
        state.terminalReason = load ? SaleTerminal::kLoadChanged : SaleTerminal::kMenuClosed;
        state.errorLatched = load;
        return state.terminalReason;
    }

    void ClearMenuLatch() noexcept {
        state.errorLatched = false;
        state.terminal = false;
        state.terminalReason = SaleTerminal::kIssued;
    }
};

}
